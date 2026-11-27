// linein's audio inputs on Windows (mods/linein.h): WASAPI. The devices are the active capture endpoints (name: their
// friendly name, UID: the endpoint ID). A device is read in shared mode, in the format Windows mixes it at (the
// endpoint's mix format: 32-bit float as a rule, 16, 24 or 32-bit integers are taken too), event-driven on a thread of
// its own at MMCSS "Pro Audio" priority, which turns each packet into float frames in the ring (the layout the macOS
// IO callback writes). Its block is the device's default period (10 ms as a rule). A device unplugged (or changed in
// Windows' sound settings) ends the stream with AUDCLNT_E_DEVICE_INVALIDATED: the Capture is marked lost and the
// thread ends; linein.cpp's manage() then lets go of it and opens it again once it is back, as on macOS.
// Windows' microphone privacy switch for desktop apps covers every input: with it off the stream is refused
// (E_ACCESSDENIED) and the log says where to allow it.
// For testing on a PC without an input: with the environment variable RBM_LINEIN_LOOPBACK=1 the list also has every
// active output as "<name> (loopback)" (uid "loopback:<endpoint ID>"), read as WASAPI loopback: what Windows plays there.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmreg.h>
#include <propsys.h>
#include "../core/rbmod.h"
#include "linein.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace rbm;
using namespace linein;

namespace {

// (defined here rather than through functiondiscoverykeys_devpkey.h / ksmedia.h, which need INITGUID in one file)
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};  // PKEY_Device_FriendlyName
const PROPERTYKEY kDeviceFormat = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};   // PKEY_AudioEngine_DeviceFormat
const GUID kSubPcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};    // KSDATAFORMAT_SUBTYPE_PCM
const GUID kSubFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT

const REFERENCE_TIME kBufferHns = 1000000;  // the stream's buffer: 100 ms (only what the thread hasn't fetched yet waits in it)
const DWORD kOpenWaitMs = 5000;
const wchar_t kLoopback[] = L"loopback:";  // (uid prefix of an output read as an input: testing, see the top)

// A COM pointer, released when it goes.
template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** put() { if (p) p->Release(), p = nullptr; return &p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string utf8(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string hex(HRESULT hr) {
    char b[16];
    snprintf(b, sizeof b, "0x%08x", (unsigned)hr);
    return b;
}

// COM on the calling thread (the main thread, for the device list) while it lives: rekordbox's main thread has it
// already as a rule (then this only counts up and down; in another mode it fails and changes nothing).
struct ComHere {
    bool mine = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    ~ComHere() { if (mine) CoUninitialize(); }
};

// The sample formats taken: what one sample of the stream is.
enum Format { kNone, kF32, kI16, kI24, kI32 };
Format formatOf(const WAVEFORMATEX* f) {
    if (!f || !f->nChannels) return kNone;
    bool isFloat;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const GUID& sub = ((const WAVEFORMATEXTENSIBLE*)f)->SubFormat;
        if (IsEqualGUID(sub, kSubFloat)) isFloat = true;
        else if (IsEqualGUID(sub, kSubPcm)) isFloat = false;
        else return kNone;
    } else if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) isFloat = true;
    else if (f->wFormatTag == WAVE_FORMAT_PCM) isFloat = false;
    else return kNone;
    if (f->nBlockAlign != f->nChannels * (f->wBitsPerSample / 8)) return kNone;
    if (isFloat) return f->wBitsPerSample == 32 ? kF32 : kNone;
    // (24 valid bits in a 32-bit container are left-justified: read as 32-bit, the same scale)
    switch (f->wBitsPerSample) {
        case 16: return kI16;
        case 24: return kI24;
        case 32: return kI32;
        default: return kNone;
    }
}

// One packet of the stream (frames of nch interleaved samples in format fmt; null: silence) into the ring at w, the
// first `keep` channels.
void put(Capture* c, uint64_t w, const BYTE* data, UINT32 frames, int nch, Format fmt) {
    float* ring = c->ring.data();
    int keep = c->channels;
    for (UINT32 i = 0; i < frames; i++) {
        float* dst = ring + (size_t)((w + i) & (kRing - 1)) * (size_t)keep;
        if (!data) {
            for (int ch = 0; ch < keep; ch++) dst[ch] = 0;
            continue;
        }
        size_t s = (size_t)i * (size_t)nch;
        switch (fmt) {
            case kF32: {
                const float* src = (const float*)data + s;
                for (int ch = 0; ch < keep; ch++) dst[ch] = src[ch];
                break;
            }
            case kI16: {
                const int16_t* src = (const int16_t*)data + s;
                for (int ch = 0; ch < keep; ch++) dst[ch] = (float)src[ch] * (1.0f / 32768.0f);
                break;
            }
            case kI24: {
                const BYTE* src = data + s * 3;
                for (int ch = 0; ch < keep; ch++) {
                    const BYTE* b = src + ch * 3;
                    int32_t v = (int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24);
                    dst[ch] = (float)v * (1.0f / 2147483648.0f);
                }
                break;
            }
            case kI32: {
                const int32_t* src = (const int32_t*)data + s;
                for (int ch = 0; ch < keep; ch++) dst[ch] = (float)src[ch] * (1.0f / 2147483648.0f);
                break;
            }
            default: break;
        }
    }
}

typedef HANDLE(WINAPI* AvSetFn)(LPCWSTR, LPDWORD);
typedef BOOL(WINAPI* AvRevertFn)(HANDLE);

// The capture thread's state; the Capture's sys.
struct Sys {
    HANDLE stopEv = nullptr;   // set by stop()
    HANDLE readyEv = nullptr;  // set by the thread once it runs (ok) or gave up (err, hr)
    std::thread thread;
    bool ok = false;
    HRESULT hr = S_OK;
    std::string err;
};

std::string openError(HRESULT hr, const char* what) {
    if (hr == E_ACCESSDENIED) return "Windows doesn't let rekordbox use audio inputs";
    if (hr == AUDCLNT_E_DEVICE_IN_USE) return "another program has it to itself (exclusive mode)";
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == E_NOTFOUND || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return "it went away";
    return std::string("Windows could not ") + what + " it (" + hex(hr) + ")";
}

// The thread: opens the endpoint, starts the stream, then fetches each packet as its event comes until stop() or
// the device goes. COM (multithreaded) is this thread's own.
void run(Capture* c, Sys* s, std::wstring id) {
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    HANDLE mm = nullptr;
    AvRevertFn avRevert = nullptr;
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    if (avrt) {
        AvSetFn avSet = (AvSetFn)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
        avRevert = (AvRevertFn)(void*)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
        DWORD task = 0;
        if (avSet) mm = avSet(L"Pro Audio", &task);
    }
    if (!mm) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {
        Com<IMMDeviceEnumerator> en;
        Com<IMMDevice> dev;
        Com<IAudioClient> client;
        Com<IAudioCaptureClient> cap;
        WAVEFORMATEX* wfx = nullptr;
        Format fmt = kNone;
        int nch = 0;
        bool running = false;
        HRESULT hr = ev ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        const char* what = "open";
        if (SUCCEEDED(hr)) hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)en.put());
        bool loopback = id.rfind(kLoopback, 0) == 0;
        if (loopback) id = id.substr(wcslen(kLoopback));
        if (SUCCEEDED(hr)) hr = en->GetDevice(id.c_str(), dev.put());
        // Opened "raw" first: without the gain control and noise suppression Windows may run on a microphone-class
        // input, which a turntable should not get. A device that can't do that is opened again without.
        for (int attempt = 0; attempt < 2 && SUCCEEDED(hr); attempt++) {
            bool raw = attempt == 0 && !loopback;
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.put());
            if (FAILED(hr)) break;
            if (raw) {
                Com<IAudioClient2> client2;
                AudioClientProperties props = {};
                props.cbSize = sizeof props;
                props.eCategory = AudioCategory_Other;
                props.Options = AUDCLNT_STREAMOPTIONS_RAW;
                if (FAILED(client->QueryInterface(__uuidof(IAudioClient2), (void**)client2.put())) || FAILED(client2->SetClientProperties(&props))) continue;
            }
            if (wfx) CoTaskMemFree(wfx), wfx = nullptr;
            hr = client->GetMixFormat(&wfx);
            if (FAILED(hr)) break;
            fmt = formatOf(wfx);
            nch = wfx->nChannels;
            if (fmt == kNone) { s->err = "its audio isn't in a format this can read"; break; }
            HRESULT init = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0), kBufferHns, 0, wfx, nullptr);
            if (FAILED(init) && raw && init != E_ACCESSDENIED && init != AUDCLNT_E_DEVICE_IN_USE) continue;  // (again, as an ordinary stream)
            hr = init;
            if (FAILED(hr)) break;
            c->rate = wfx->nSamplesPerSec;
            c->channels = std::min(nch, kMaxChannels);
            c->ring.assign((size_t)kRing * (size_t)c->channels, 0.f);
            REFERENCE_TIME def = 0, min = 0;
            if (SUCCEEDED(client->GetDevicePeriod(&def, &min)) && def > 0) c->block = (uint32_t)llround((double)def * c->rate / 1e7);
            if (!c->block) c->block = 512;
            // An input that is Windows' "communications" device would otherwise count as a call, and Windows turn
            // every other program's sound down, rekordbox's own output included.
            Com<IAudioSessionControl2> session;
            if (SUCCEEDED(client->GetService(__uuidof(IAudioSessionControl2), (void**)session.put()))) session->SetDuckingPreference(TRUE);
            hr = client->SetEventHandle(ev);
            if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)cap.put());
            if (SUCCEEDED(hr)) what = "start", hr = client->Start();
            running = SUCCEEDED(hr);
            break;
        }
        if (wfx) CoTaskMemFree(wfx);
        if (!running) {
            if (s->err.empty()) s->err = openError(hr, what);
            s->hr = hr;
        }
        s->ok = running;
        SetEvent(s->readyEv);
        // (open() gave up waiting: nobody reads it)
        if (running && WaitForSingleObject(s->stopEv, 0) == WAIT_OBJECT_0) running = false, client->Stop();
        while (running) {
            HANDLE wait[2] = {s->stopEv, ev};
            DWORD r = WaitForMultipleObjects(2, wait, FALSE, 1000);
            if (r == WAIT_OBJECT_0) break;
            // Every packet there is (also after a second without an event: a device that went says so here).
            uint64_t w = c->written.load(std::memory_order_relaxed);
            uint32_t got = 0;
            for (;;) {
                UINT32 next = 0;
                hr = cap->GetNextPacketSize(&next);
                if (FAILED(hr) || !next) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                if (hr == AUDCLNT_S_BUFFER_EMPTY || !frames) { if (hr != AUDCLNT_S_BUFFER_EMPTY) cap->ReleaseBuffer(frames); break; }
                if (frames <= (UINT32)kRing / 2) {
                    if (got + frames > (uint32_t)kRing / 2) {  // (a lot at once: what came so far is the reader's first)
                        c->lastBlock.store(got, std::memory_order_relaxed);
                        c->written.store(w + got, std::memory_order_release);
                        w += got, got = 0;
                    }
                    put(c, w + got, (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, frames, nch, fmt);
                    got += frames;
                }
                cap->ReleaseBuffer(frames);
            }
            if (got) {
                c->lastBlock.store(got, std::memory_order_relaxed);
                c->written.store(w + got, std::memory_order_release);
            }
            if (FAILED(hr)) {
                // AUDCLNT_E_DEVICE_INVALIDATED: unplugged, or its format changed in Windows' sound settings
                c->lost.store(true, std::memory_order_release);
                logf("linein: '%s' stopped delivering audio (%s)", c->name.c_str(), hex(hr).c_str());
                break;
            }
        }
        if (cap) client->Stop();
    }
    if (ev) CloseHandle(ev);
    if (mm && avRevert) avRevert(mm);
    if (avrt) FreeLibrary(avrt);
    if (com) CoUninitialize();
}

bool g_deniedLogged;

}  // namespace

namespace linein::sys {

std::vector<Input> inputs() {
    std::vector<Input> v;
    ComHere com;
    Com<IMMDeviceEnumerator> en;
    Com<IMMDeviceCollection> all;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)en.put()))) return v;
    char loopEnv[8] = {};
    bool loopback = GetEnvironmentVariableA("RBM_LINEIN_LOOPBACK", loopEnv, sizeof loopEnv) && loopEnv[0] == '1';
    for (EDataFlow flow : {eCapture, eRender}) {
    if (flow == eRender && !loopback) break;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, all.put()))) continue;
    UINT n = 0;
    all->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        Com<IMMDevice> dev;
        if (FAILED(all->Item(i, dev.put()))) continue;
        Input in;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id)) || !id) continue;
        in.uid = utf8(id);
        CoTaskMemFree(id);
        Com<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(kFriendlyName, &pv)) && pv.vt == VT_LPWSTR) in.name = utf8(pv.pwszVal);
            PropVariantClear(&pv);
            // (the format Windows mixes it at: what a shared-mode stream gets, rate and channels)
            if (SUCCEEDED(props->GetValue(kDeviceFormat, &pv)) && pv.vt == VT_BLOB && pv.blob.pBlobData && pv.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                const WAVEFORMATEX* f = (const WAVEFORMATEX*)pv.blob.pBlobData;
                in.channels = f->nChannels, in.rate = f->nSamplesPerSec;
            }
            PropVariantClear(&pv);
        }
        if (in.channels < 1) in.channels = 2;  // (not said: a capture endpoint has input either way)
        if (in.name.empty()) in.name = in.uid;
        if (flow == eRender) in.name += " (loopback)", in.uid = utf8(kLoopback) + in.uid;
        v.push_back(std::move(in));
    }
    }
    return v;
}

// (Windows' microphone switch shows only when a stream is opened: open() says so.)
bool allowed(std::string*) { return true; }

std::unique_ptr<Capture> open(const Input& in, std::string* err) {
    auto c = std::make_unique<Capture>();
    c->id = in.id, c->uid = in.uid, c->name = in.name;
    Sys* s = new Sys;
    s->stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s->readyEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s->stopEv || !s->readyEv) {
        if (s->stopEv) CloseHandle(s->stopEv);
        if (s->readyEv) CloseHandle(s->readyEv);
        delete s;
        *err = "Windows could not open it (no event)";
        return nullptr;
    }
    Capture* raw = c.get();
    s->thread = std::thread(run, raw, s, wide(in.uid));
    if (WaitForSingleObject(s->readyEv, kOpenWaitMs) != WAIT_OBJECT_0) {
        // The thread hangs in Windows' audio service: it may still come back and use both, so they stay (a leak, once).
        SetEvent(s->stopEv);
        s->thread.detach();
        (void)c.release();
        *err = "Windows did not open it in time";
        return nullptr;
    }
    if (!s->ok) {
        s->thread.join();
        *err = s->err;
        if (s->hr == E_ACCESSDENIED && !g_deniedLogged) {
            g_deniedLogged = true;
            logf("linein: Windows doesn't let rekordbox use audio inputs: allow it in Settings > Privacy & security > Microphone > Let desktop apps access your microphone");
        }
        CloseHandle(s->stopEv), CloseHandle(s->readyEv);
        delete s;
        return nullptr;
    }
    c->sys = s;
    c->started = true;
    return c;
}

void stop(Capture* c) {
    Sys* s = (Sys*)c->sys;
    if (!s) return;
    SetEvent(s->stopEv);
    c->sys = nullptr;
    c->started = false;
    if (s->thread.joinable()) {
        // A thread stuck in Windows' audio service (it is restarting) must not hold up the window or a quit: it is
        // left to itself, with what it uses (a leak, once).
        if (WaitForSingleObject((HANDLE)s->thread.native_handle(), 2000) != WAIT_OBJECT_0) {
            s->thread.detach();
            logf("linein: '%s' did not stop in time: left to itself", c->name.c_str());
            return;
        }
        s->thread.join();
    }
    CloseHandle(s->stopEv), CloseHandle(s->readyEv);
    delete s;
}

}  // namespace linein::sys
