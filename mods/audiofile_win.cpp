// See audiofile.h. Windows: reading through Media Foundation's source reader (WAV, FLAC, MP3, AAC... whatever
// Windows decodes), and AIFF, which it doesn't read, through aiff.h. sameSamples compares two files as Media
// Foundation decodes them.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "audiofile.h"
#include "aiff.h"
#include "../core/rbmod.h"
#include "../core/plat.h"
#include <fstream>
#include <mutex>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace audiofile {

namespace {

// COM and Media Foundation for this thread (the callers are worker threads), and MF started once.
struct MfThread {
    bool com = false, ok = false;
    MfThread() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE: the thread has COM already, fine
        static std::once_flag once;
        static bool started = false;
        std::call_once(once, [] { started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); });
        ok = started;
    }
    ~MfThread() {
        if (com) CoUninitialize();
    }
};

template <class T>
struct Ptr {  // a minimal COM pointer
    T* p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p; }
};

std::wstring wide(const std::string& s) { return rbm::plat::fsPath(s).wstring(); }

bool openReader(const std::string& path, Ptr<IMFSourceReader>& r) {
    return SUCCEEDED(MFCreateSourceReaderFromURL(wide(path).c_str(), nullptr, &r)) &&
           SUCCEEDED(r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE)) &&
           SUCCEEDED(r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE));
}

int nativeRate(IMFSourceReader* r) {
    Ptr<IMFMediaType> t;
    UINT32 rate = 0;
    if (SUCCEEDED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &t))) t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    return (int)rate;
}

double seconds(IMFSourceReader* r) {
    PROPVARIANT v;
    PropVariantInit(&v);
    double s = 0;
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)) && v.vt == VT_UI8)
        s = v.uhVal.QuadPart / 1e7;
    PropVariantClear(&v);
    return s;
}

// The reader decodes to 16-bit stereo PCM at the file's own rate.
bool decodeTo16(IMFSourceReader* r, int rate) {
    Ptr<IMFMediaType> t;
    if (FAILED(MFCreateMediaType(&t))) return false;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 4);
    return SUCCEEDED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t.p));
}

// The next decoded bytes into out (appended); false at the end or on an error (*err).
bool readMore(IMFSourceReader* r, std::vector<uint8_t>& out, bool* err) {
    for (;;) {
        DWORD flags = 0;
        Ptr<IMFSample> s;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s))) return *err = true, false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) return *err = true, false;
        if (!s) continue;
        Ptr<IMFMediaBuffer> b;
        if (FAILED(s->ConvertToContiguousBuffer(&b))) return *err = true, false;
        BYTE* p = nullptr;
        DWORD n = 0;
        if (FAILED(b->Lock(&p, nullptr, &n))) return *err = true, false;
        out.insert(out.end(), p, p + n);
        b->Unlock();
        if (n) return true;
    }
}

// An AIFF file, opened, and its header (not ok: any other file, which Media Foundation gets).
struct Aiff {
    std::ifstream f;
    aiff::Format x;
    bool ok;
    explicit Aiff(const std::string& path) : f(rbm::plat::fsPath(path), std::ios::binary) { ok = f && aiff::open(f, &x); }
    int rate() const { return (int)(x.rate + 0.5); }
};

}  // namespace

bool facts(const std::string& path, int* rate, long long* frames) {
    if (Aiff a(path); a.ok) return *rate = a.rate(), *frames = a.x.frames, true;
    MfThread mf;
    Ptr<IMFSourceReader> r;
    if (!mf.ok || !openReader(path, r)) return false;
    int hz = nativeRate(r.p);
    double s = seconds(r.p);
    if (hz <= 0 || s <= 0) return false;
    *rate = hz, *frames = (long long)(s * hz + 0.5);
    return true;
}

bool probe(const std::string& path, Info* out) {
    if (Aiff a(path); a.ok) {
        out->seconds = a.x.frames / a.x.rate, out->sampleRate = a.x.rate;
        out->bitRate = a.x.rate * a.x.channels * a.x.bytes() * 8;
        return true;
    }
    MfThread mf;
    Ptr<IMFSourceReader> r;
    if (!mf.ok || !openReader(path, r)) return false;
    out->seconds = seconds(r.p);
    out->sampleRate = nativeRate(r.p);
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_AUDIO_ENCODING_BITRATE, &v)) && v.vt == VT_UI4)
        out->bitRate = v.ulVal;
    PropVariantClear(&v);
    return true;
}

bool readStereo16(const std::string& path, std::vector<int16_t>* out, int* rate) {
    if (Aiff a(path); a.ok) return *rate = a.rate(), aiff::readStereo16(a.f, a.x, out);
    MfThread mf;
    Ptr<IMFSourceReader> r;
    if (!mf.ok || !openReader(path, r)) return false;
    int hz = nativeRate(r.p);
    if (hz <= 0 || !decodeTo16(r.p, hz)) return false;
    *rate = hz;
    out->clear();
    std::vector<uint8_t> b;
    bool err = false;
    while (readMore(r.p, b, &err)) {
        size_t n = b.size() / 4;
        const int16_t* s = (const int16_t*)b.data();
        out->insert(out->end(), s, s + n * 2);
        b.erase(b.begin(), b.begin() + n * 4);
    }
    return !err && !out->empty();
}

bool readMono(const std::string& path, std::vector<float>* out, int* rate) {
    std::vector<int16_t> x;
    if (!readStereo16(path, &x, rate)) return false;
    out->resize(x.size() / 2);
    for (size_t i = 0; i < out->size(); i++) (*out)[i] = (x[2 * i] + x[2 * i + 1]) / 65536.f;
    return true;
}

bool sameSamples(const std::string& a, const std::string& b) {
    MfThread mf;
    Ptr<IMFSourceReader> ra, rb;
    if (!mf.ok || !openReader(a, ra) || !openReader(b, rb)) return false;
    int rate = nativeRate(ra.p);
    if (rate <= 0 || rate != nativeRate(rb.p) || !decodeTo16(ra.p, rate) || !decodeTo16(rb.p, rate)) return false;
    // Both decoded side by side, compared as far as both have come; the rest waits for the next read.
    std::vector<uint8_t> x, y;
    bool endA = false, endB = false, err = false;
    uint64_t total = 0;
    for (;;) {
        if (!endA && (x.size() <= y.size() || endB)) endA = !readMore(ra.p, x, &err);
        else if (!endB) endB = !readMore(rb.p, y, &err);
        if (err) return false;
        size_t n = std::min(x.size(), y.size());
        if (n) {
            if (memcmp(x.data(), y.data(), n)) return false;
            x.erase(x.begin(), x.begin() + n), y.erase(y.begin(), y.begin() + n);
            total += n;
        }
        if (endA && endB) return x.empty() && y.empty() && total > 0;
        if ((endA && x.empty() && !y.empty()) || (endB && y.empty() && !x.empty())) return false;  // lengths differ
    }
}

}  // namespace audiofile
