// rbglitch.exe: test tool for "does a deck load make the playing deck's sound drop out" on Windows.
// Built by tools/win/glitch/build.sh, run on the test PC (see docs/windows.md, "Speed"). The scripts beside it drive a
// run from the Mac (g.sh, case.sh, setup.sh) and read the results (analyze.py, summ.py, thr.py).
//
//   rbglitch rec <out.wav> <seconds> [device name part]   WASAPI loopback of a playback device (default: the
//                                                         default one): mono 16-bit WAV + <out.wav>.pkt.csv
//   rbglitch suspend <pid> <ms>                           stops every thread of a process for <ms> (a dropout on purpose)
//   rbglitch threads <pid> <seconds> <out.csv> [ms]       CPU time per thread per interval, priorities, names, modules
//   rbglitch run <script> <outdir>                        runs a timed script (below) in the desktop session
//   rbglitch                                              the same, arguments read from C:\rb\glitch.args (for a task)
//
// Script lines: "<ms from start> <command> <args>", commands:
//   rec <seconds> [device]      -> audio.wav, audio.wav.pkt.csv
//   threads <seconds> [ms]      -> threads.csv (rekordbox.exe)
//   ui <seconds>                -> ui.csv: answers of rekordbox's main window slower than 50 ms, total CPU twice a second
//   strip <x> <y> <w> <h> <seconds> <ms> [name] [dump] -> <name>.csv: how much of a screen area is lit, when it changes
//   log <seconds>               -> mods.log: the mods' log lines, each with the time it was written
//   click <x> <y> [left|right|double]
//   key <combo> [times]         e.g. ctrl+right, shift+left, space, esc
//   wheel <x> <y> <notches>     mouse wheel (negative: down)
//   type <text>                 typed text, '_' for a space
//   suspend <ms>                rekordbox.exe
//   end                         waits for everything and exits
// Every output line starts with the local time of day in ms.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <algorithm>

static double nowMs() {  // local time of day, ms
    FILETIME ft, lt;
    GetSystemTimePreciseAsFileTime(&ft);
    FileTimeToLocalFileTime(&ft, &lt);
    ULONGLONG t = ((ULONGLONG)lt.dwHighDateTime << 32) | lt.dwLowDateTime;
    return (double)(t % 864000000000ULL) / 10000.0;
}
static std::string clock(double ms) {
    char b[32];
    int s = (int)(ms / 1000);
    snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", s / 3600, s / 60 % 60, s % 60, (int)fmod(ms, 1000));
    return b;
}
static std::string narrow(const wchar_t *w) {
    char b[512] = "";
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
    return b;
}
static DWORD findPid(const wchar_t *exe) {  // the instance with the most threads (rekordbox starts helpers of the same name)
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{sizeof e};
    DWORD pid = 0, most = 0;
    for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e))
        if (!_wcsicmp(e.szExeFile, exe) && e.cntThreads > most) most = e.cntThreads, pid = e.th32ProcessID;
    CloseHandle(s);
    return pid;
}

// ---- rec ----
static int rec(const std::string &out, double seconds, const std::string &want) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en))) return printf("rec: no enumerator\n"), 1;
    IMMDevice *dev = nullptr;
    std::string name;
    auto nameOf = [](IMMDevice *d) {
        IPropertyStore *ps = nullptr;
        std::string n;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) n = narrow(v.pwszVal);
            PropVariantClear(&v);
            ps->Release();
        }
        return n;
    };
    if (!want.empty()) {
        IMMDeviceCollection *col = nullptr;
        en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
        UINT n = 0;
        if (col) col->GetCount(&n);
        for (UINT i = 0; i < n && !dev; i++) {
            IMMDevice *d = nullptr;
            col->Item(i, &d);
            std::string nm = nameOf(d);
            if (nm.find(want) != std::string::npos) dev = d, name = nm;
            else d->Release();
        }
    }
    if (!dev) {
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return printf("rec: no playback device\n"), 1;
        name = nameOf(dev);
    }
    IAudioClient *ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&ac))) return printf("rec: Activate failed\n"), 1;
    WAVEFORMATEX *wf = nullptr;
    ac->GetMixFormat(&wf);
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    ac->GetDevicePeriod(&defPeriod, &minPeriod);
    HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 20000000, 0, wf, nullptr);
    if (FAILED(hr)) return printf("rec: Initialize failed %08lx\n", hr), 1;
    IAudioCaptureClient *cc = nullptr;
    if (FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void **)&cc))) return printf("rec: no capture client\n"), 1;
    bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->wBitsPerSample == 32);
    int ch = wf->nChannels, rate = wf->nSamplesPerSec, bits = wf->wBitsPerSample;
    FILE *f = fopen(out.c_str(), "wb");
    FILE *pk = fopen((out + ".pkt.csv").c_str(), "w");
    if (!f || !pk) return printf("rec: cannot write %s\n", out.c_str()), 1;
    fprintf(pk, "# device=%s rate=%d channels=%d bits=%d float=%d period_ms=%.2f min_period_ms=%.2f\n", name.c_str(), rate, ch, bits, isFloat, defPeriod / 1e4, minPeriod / 1e4);
    fprintf(pk, "time_ms,frame,frames,flags,devpos,qpc_ms\n");
    unsigned char hdr[44] = {};
    fwrite(hdr, 1, 44, f);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ac->Start();
    double t0 = nowMs();
    unsigned long long total = 0;
    std::vector<short> mono;
    while (nowMs() - t0 < seconds * 1000) {
        UINT32 n = 0;
        while (SUCCEEDED(cc->GetNextPacketSize(&n)) && n) {
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devpos = 0, qpc = 0;
            if (FAILED(cc->GetBuffer(&data, &frames, &flags, &devpos, &qpc))) break;
            mono.resize(frames);
            for (UINT32 i = 0; i < frames; i++) {
                double s = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    if (isFloat) for (int c = 0; c < std::min(ch, 2); c++) s += ((float *)data)[i * ch + c];
                    else if (bits == 16) for (int c = 0; c < std::min(ch, 2); c++) s += ((short *)data)[i * ch + c] / 32768.0;
                    else if (bits == 32) for (int c = 0; c < std::min(ch, 2); c++) s += ((int *)data)[i * ch + c] / 2147483648.0;
                    s /= std::min(ch, 2);
                }
                double v = s * 32767.0;
                mono[i] = (short)std::lround(std::max(-32768.0, std::min(32767.0, v)));
            }
            fwrite(mono.data(), 2, frames, f);
            fprintf(pk, "%.1f,%llu,%u,%lu,%llu,%.2f\n", nowMs(), total, frames, flags, (unsigned long long)devpos, qpc / 1e4);
            total += frames;
            cc->ReleaseBuffer(frames);
        }
        Sleep(3);
    }
    ac->Stop();
    unsigned bytes = (unsigned)(total * 2);
    auto put = [&](int o, unsigned v, int n) { memcpy(hdr + o, &v, n); };
    memcpy(hdr, "RIFF", 4); put(4, 36 + bytes, 4); memcpy(hdr + 8, "WAVEfmt ", 8); put(16, 16, 4); put(20, 1, 2); put(22, 1, 2);
    put(24, rate, 4); put(28, rate * 2, 4); put(32, 2, 2); put(34, 16, 2); memcpy(hdr + 36, "data", 4); put(40, bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(hdr, 1, 44, f);
    fclose(f);
    fclose(pk);
    printf("%s rec: %llu frames at %d Hz from %s\n", clock(nowMs()).c_str(), total, rate, name.c_str());
    return 0;
}

// ---- suspend ----
extern "C" {
NTSYSAPI LONG NTAPI NtSuspendProcess(HANDLE);
NTSYSAPI LONG NTAPI NtResumeProcess(HANDLE);
NTSYSAPI LONG NTAPI NtQuerySystemInformation(int, PVOID, ULONG, PULONG);
NTSYSAPI LONG NTAPI NtQueryInformationThread(HANDLE, int, PVOID, ULONG, PULONG);
}
static int suspend(DWORD pid, int ms) {
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h) return printf("suspend: cannot open %lu (%lu)\n", pid, GetLastError()), 1;
    double a = nowMs();
    NtSuspendProcess(h);
    Sleep(ms);
    NtResumeProcess(h);
    double b = nowMs();
    CloseHandle(h);
    printf("%s suspend: pid %lu stopped for %.1f ms (from %.1f)\n", clock(a).c_str(), pid, b - a, a);
    return 0;
}

// ---- threads ----
namespace {
struct SysThread {
    LARGE_INTEGER KernelTime, UserTime, CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    HANDLE Pid, Tid;
    LONG Priority, BasePriority;
    ULONG ContextSwitches, ThreadState, WaitReason;
};
struct SysProc {
    ULONG NextEntryOffset, NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount, NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    struct { USHORT Length, MaximumLength; PWSTR Buffer; } ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId, InheritedFromUniqueProcessId;
    ULONG HandleCount, SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize, VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
    SysThread Threads[1];
};
struct ThreadSeen {
    double cpu = 0, first = -1, total = 0, maxSlice = 0, last = 0;
    LONG base = 0, minPrio = 99, maxPrio = 0;
    ULONG cs = 0;
    ULONG_PTR start = 0;
    std::string name;
};
}
static HWND g_main;
static BOOL CALLBACK findMain(HWND w, LPARAM pid) {
    DWORD p = 0;
    GetWindowThreadProcessId(w, &p);
    RECT r;
    if (p == (DWORD)pid && IsWindowVisible(w) && GetWindowRect(w, &r) && r.right - r.left > 800 && !GetWindow(w, GW_OWNER)) { g_main = w; return FALSE; }
    return TRUE;
}

static int threads(DWORD pid, double seconds, const std::string &out, int interval) {
    FILE *f = fopen(out.c_str(), "w");
    if (!f) return printf("threads: cannot write %s\n", out.c_str()), 1;
    fprintf(f, "# rows: T,time_ms,tid,cpu_ms,priority,base,context_switches | P,time_ms,process,cpu_ms | S,time_ms,total_cpu_pct | D,time_ms,read_bytes,write_bytes (rekordbox)\n");
    std::vector<char> buf(4 << 20);
    std::map<DWORD, ThreadSeen> seen;
    std::map<DWORD, std::pair<std::string, double>> procs;  // other processes: name, cpu so far
    FILETIME i0, k0, u0;
    GetSystemTimes(&i0, &k0, &u0);
    auto ft = [](FILETIME x) { return (double)(((ULONGLONG)x.dwHighDateTime << 32) | x.dwLowDateTime) / 1e4; };
    double t0 = nowMs(), lastIo[2] = {-1, -1};
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    while (nowMs() - t0 < seconds * 1000) {
        ULONG need = 0;
        while (NtQuerySystemInformation(5, buf.data(), (ULONG)buf.size(), &need) < 0) buf.resize(buf.size() * 2);
        double t = nowMs();
        for (SysProc *p = (SysProc *)buf.data();; p = (SysProc *)((char *)p + p->NextEntryOffset)) {
            DWORD id = (DWORD)(ULONG_PTR)p->UniqueProcessId;
            double cpu = (p->UserTime.QuadPart + p->KernelTime.QuadPart) / 1e4;
            if (id == pid) {
                for (ULONG i = 0; i < p->NumberOfThreads; i++) {
                    SysThread &th = p->Threads[i];
                    ThreadSeen &s = seen[(DWORD)(ULONG_PTR)th.Tid];
                    double c = (th.KernelTime.QuadPart + th.UserTime.QuadPart) / 1e4;
                    s.last = t;
                    if (s.first < 0) {
                        s.first = t;
                        if (HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, (DWORD)(ULONG_PTR)th.Tid)) {
                            NtQueryInformationThread(h, 9, &s.start, sizeof s.start, nullptr);
                            CloseHandle(h);
                        }
                        if (t - t0 > 300) {  // a thread started during the sampling: its CPU so far counts
                            s.cpu = 0, s.cs = 0;
                            goto count;
                        }
                    } else {
                    count:
                        if (c - s.cpu >= 0.5) {
                        fprintf(f, "T,%.0f,%lu,%.1f,%ld,%ld,%lu\n", t, (DWORD)(ULONG_PTR)th.Tid, c - s.cpu, th.Priority, th.BasePriority, th.ContextSwitches - s.cs);
                        s.total += c - s.cpu;
                        s.maxSlice = std::max(s.maxSlice, c - s.cpu);
                        }
                    }
                    s.cpu = c, s.cs = th.ContextSwitches, s.base = th.BasePriority;
                    s.minPrio = std::min(s.minPrio, th.Priority), s.maxPrio = std::max(s.maxPrio, th.Priority);
                }
                double rd = (double)p->ReadTransferCount.QuadPart, wr = (double)p->WriteTransferCount.QuadPart;
                if (lastIo[0] >= 0 && (rd - lastIo[0] > 65536 || wr - lastIo[1] > 65536)) fprintf(f, "D,%.0f,%.0f,%.0f\n", t, rd - lastIo[0], wr - lastIo[1]);
                lastIo[0] = rd, lastIo[1] = wr;
            } else if (id) {
                auto it = procs.find(id);
                if (it == procs.end()) procs[id] = {narrow(std::wstring(p->ImageName.Buffer ? p->ImageName.Buffer : L"?", p->ImageName.Length / 2).c_str()), cpu};
                else {
                    if (cpu - it->second.second >= 5) fprintf(f, "P,%.0f,%s,%.1f\n", t, it->second.first.c_str(), cpu - it->second.second);
                    it->second.second = cpu;
                }
            }
            if (!p->NextEntryOffset) break;
        }
        FILETIME i1, k1, u1;
        GetSystemTimes(&i1, &k1, &u1);
        double idle = ft(i1) - ft(i0), all = ft(k1) - ft(k0) + ft(u1) - ft(u0);
        if (all > 0) fprintf(f, "S,%.0f,%.0f\n", t, 100 * (1 - idle / all));
        i0 = i1, k0 = k1, u0 = u1;
        Sleep(interval);
    }
    // who the threads are: name, the module their start address is in
    HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    struct Mod { ULONG_PTR base, size; std::string name; };
    std::vector<Mod> mods;
    if (ph) {
        std::vector<HMODULE> hm(4096);
        DWORD got = 0;
        if (EnumProcessModulesEx(ph, hm.data(), (DWORD)(hm.size() * sizeof(HMODULE)), &got, LIST_MODULES_ALL))
            for (DWORD i = 0; i < got / sizeof(HMODULE) && i < hm.size(); i++) {
                MODULEINFO mi{};
                wchar_t nm[260] = L"";
                GetModuleInformation(ph, hm[i], &mi, sizeof mi);
                GetModuleBaseNameW(ph, hm[i], nm, 260);
                mods.push_back({(ULONG_PTR)mi.lpBaseOfDll, mi.SizeOfImage, narrow(nm)});
            }
    }
    DWORD uiTid = 0;
    EnumWindows(findMain, pid);
    if (g_main) uiTid = GetWindowThreadProcessId(g_main, nullptr);
    fprintf(f, "# threads: N,tid,cpu_ms,max_ms_per_interval,base,min_priority,max_priority,start,name,first_seen_ms,last_seen_ms\n");
    for (auto &[tid, s] : seen) {
        std::string name = tid == uiTid ? "UI" : "", start = "?";
        if (HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
            PWSTR d = nullptr;
            if (SUCCEEDED(GetThreadDescription(th, &d)) && d) { if (*d) name += narrow(d); LocalFree(d); }
            CloseHandle(th);
        }
        if (s.start) {
            char b[300];
            snprintf(b, sizeof b, "%llx", (unsigned long long)s.start);
            start = b;
            for (auto &m : mods)
                if (s.start >= m.base && s.start < m.base + m.size) snprintf(b, sizeof b, "%s+%llx", m.name.c_str(), (unsigned long long)(s.start - m.base)), start = b;
        }
        fprintf(f, "N,%lu,%.0f,%.0f,%ld,%ld,%ld,%s,%s,%.0f,%.0f\n", tid, s.total, s.maxSlice, s.base, s.minPrio, s.maxPrio, start.c_str(), name.c_str(), s.first, s.last);
    }
    fclose(f);
    return 0;
}

// ---- ui: how fast rekordbox's main window answers ----
static int ui(DWORD pid, double seconds, const std::string &out) {
    FILE *f = fopen(out.c_str(), "w");
    if (!f) return 1;
    EnumWindows(findMain, pid);
    fprintf(f, "# window %p; rows: U,time_ms,answer_ms (50 ms and over)\n", (void *)g_main);
    double t0 = nowMs(), worst = 0;
    int n = 0;
    while (nowMs() - t0 < seconds * 1000 && g_main) {
        DWORD_PTR r = 0;
        double a = nowMs();
        SendMessageTimeoutW(g_main, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 20000, &r);
        double ms = nowMs() - a;
        n++;
        worst = std::max(worst, ms);
        if (ms >= 50) fprintf(f, "U,%.0f,%.0f\n", a, ms);
        Sleep(20);
    }
    fprintf(f, "# probes=%d worst_ms=%.0f\n", n, worst);
    fclose(f);
    return 0;
}

// ---- strip: how much of a screen area is lit (a deck's waveform appearing), and its vertical lines (the beat grid) ----
static int strip(int x, int y, int w, int h, double seconds, int interval, bool dump, const std::string &out, const std::string &name) {
    SetProcessDPIAware();
    FILE *f = fopen((out + "/" + name + ".csv").c_str(), "w");
    if (!f) return 1;
    fprintf(f, "# area %d,%d %dx%d; rows: time_ms,lit_pct,line_columns,red_columns,mean\n", x, y, w, h);
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void *px = nullptr;
    HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &px, nullptr, 0);
    SelectObject(mem, bmp);
    double t0 = nowMs();
    int shot = 0;
    std::string last;
    while (nowMs() - t0 < seconds * 1000) {
        double t = nowMs();
        BitBlt(mem, 0, 0, w, h, scr, x, y, SRCCOPY);
        const unsigned char *p = (const unsigned char *)px;
        long lit = 0, sum = 0;
        int white = 0, red = 0;
        for (int cx = 0; cx < w; cx++) {
            int wn = 0, rn = 0;
            for (int cy = 0; cy < h; cy++) {
                const unsigned char *q = p + 4 * (cy * w + cx);
                int b = q[0], g = q[1], r = q[2], mx = std::max(r, std::max(g, b));
                sum += mx;
                if (mx > 70) lit++;
                // a grey or white pixel in the band above the waveform: part of a beat line (they run the lane's full height)
                if (cy >= h / 8 && cy < h * 3 / 8 && std::min(r, std::min(g, b)) > 80 && mx - std::min(r, std::min(g, b)) < 40) wn++;
                if (r > 170 && g < 90 && b < 90) rn++;
            }
            if (wn >= (h * 3 / 8 - h / 8) * 8 / 10) white++;
            if (rn >= 2) red++;
        }
        char line[128];
        snprintf(line, sizeof line, "%.1f,%d,%d,%.0f", 100.0 * lit / (w * h), white, red, (double)sum / (w * h));
        if (last != line) fprintf(f, "%.0f,%s\n", t, line), last = line;
        if (dump && shot < 400) {
            char nm[300];
            snprintf(nm, sizeof nm, "%s/%s%03d.bmp", out.c_str(), name.c_str(), shot++);
            if (FILE *b = fopen(nm, "wb")) {
                BITMAPFILEHEADER fh{0x4d42, (DWORD)(54 + w * h * 4), 0, 0, 54};
                fwrite(&fh, 1, sizeof fh, b);
                fwrite(&bi.bmiHeader, 1, sizeof bi.bmiHeader, b);
                fwrite(px, 1, w * h * 4, b);
                fclose(b);
            }
        }
        Sleep(interval);
    }
    fclose(f);
    return 0;
}

// ---- log: the mods' log lines with the time they were written ----
static int tail(double seconds, const std::string &out) {
    char path[600];
    snprintf(path, sizeof path, "%s\\rekordbox-mods\\rekordbox-mods.log", getenv("LOCALAPPDATA") ? getenv("LOCALAPPDATA") : ".");
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    FILE *f = fopen(out.c_str(), "w");
    if (h == INVALID_HANDLE_VALUE || !f) return 1;
    SetFilePointer(h, 0, nullptr, FILE_END);
    double t0 = nowMs();
    std::string part;
    char buf[8192];
    while (nowMs() - t0 < seconds * 1000) {
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) {
            double t = nowMs();
            part.append(buf, got);
            size_t nl;
            while ((nl = part.find('\n')) != std::string::npos) {
                fprintf(f, "%.0f %s %s\n", t, clock(t).c_str(), part.substr(0, nl).c_str());
                part.erase(0, nl + 1);
            }
            fflush(f);
        }
        Sleep(20);
    }
    fclose(f);
    CloseHandle(h);
    return 0;
}

// ---- input ----
static void click(int x, int y, const std::string &how) {
    SetProcessDPIAware();
    SetCursorPos(x, y);
    Sleep(120);
    DWORD down = how == "right" ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN, up = how == "right" ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    for (int i = 0; i < (how == "double" ? 2 : 1); i++) {
        mouse_event(down, 0, 0, 0, 0);
        Sleep(30);
        mouse_event(up, 0, 0, 0, 0);
        Sleep(60);
    }
}
static void key(const std::string &combo) {
    std::vector<WORD> keys;
    size_t a = 0;
    while (a <= combo.size()) {
        size_t b = combo.find('+', a);
        if (b == std::string::npos) b = combo.size();
        std::string k = combo.substr(a, b - a);
        a = b + 1;
        WORD vk = 0;
        if (k == "ctrl") vk = VK_CONTROL; else if (k == "shift") vk = VK_SHIFT; else if (k == "alt") vk = VK_MENU;
        else if (k == "left") vk = VK_LEFT; else if (k == "right") vk = VK_RIGHT; else if (k == "up") vk = VK_UP; else if (k == "down") vk = VK_DOWN;
        else if (k == "back") vk = VK_BACK; else if (k == "delete") vk = VK_DELETE; else if (k == "home") vk = VK_HOME; else if (k == "end") vk = VK_END;
        else if (k == "space") vk = VK_SPACE; else if (k == "esc") vk = VK_ESCAPE; else if (k == "enter") vk = VK_RETURN; else if (k == "tab") vk = VK_TAB;
        else if (k.size() == 1) vk = (WORD)toupper((unsigned char)k[0]);
        else if (k[0] == 'f') vk = (WORD)(VK_F1 + atoi(k.c_str() + 1) - 1);
        if (vk) keys.push_back(vk);
    }
    auto send = [](WORD vk, bool up) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (vk >= VK_PRIOR && vk <= VK_DOWN ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof in);
        Sleep(25);
    };
    for (WORD k : keys) send(k, false);
    for (size_t i = keys.size(); i--;) send(keys[i], true);
}

static void type(const std::string &text) {  // as typed text ('_' for a space)
    for (char ch : text) {
        INPUT in[2]{};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wScan = in[1].ki.wScan = (WORD)(ch == '_' ? ' ' : ch);
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(30);
    }
}

// ---- run ----
static int run(const std::string &script, const std::string &out) {
    CreateDirectoryA(out.c_str(), nullptr);
    FILE *s = fopen(script.c_str(), "r");
    FILE *log = fopen((out + "/run.log").c_str(), "w");
    if (!s || !log) return 1;
    DWORD pid = findPid(L"rekordbox.exe");
    double t0 = nowMs();
    fprintf(log, "%.0f %s start, rekordbox pid %lu\n", t0, clock(t0).c_str(), pid);
    fflush(log);
    std::vector<std::thread> bg;
    char line[512];
    while (fgets(line, sizeof line, s)) {
        std::vector<std::string> a;
        for (char *t = strtok(line, " \t\r\n"); t; t = strtok(nullptr, " \t\r\n")) a.push_back(t);
        if (a.size() < 2 || a[0][0] == '#') continue;
        double at = atof(a[0].c_str());
        while (nowMs() - t0 < at) Sleep(1);
        const std::string &c = a[1];
        auto arg = [&](size_t i, const char *d = "") { return a.size() > i ? a[i] : std::string(d); };
        double t = nowMs();
        fprintf(log, "%.0f %s %s", t, clock(t).c_str(), c.c_str());
        for (size_t i = 2; i < a.size(); i++) fprintf(log, " %s", a[i].c_str());
        fprintf(log, "\n");
        fflush(log);
        // (the arguments are copied here: the threads outlive this line's vector)
        double secs = atof(arg(2, "30").c_str());
        std::string a3 = arg(3);
        int n[8] = {};
        for (size_t i = 2; i < a.size() && i < 10; i++) n[i - 2] = atoi(a[i].c_str());
        bool dump = arg(9) == "dump";
        std::string sname = arg(8, "strip");
        if (c == "rec") bg.emplace_back([=] { rec(out + "/audio.wav", secs, a3); });
        else if (c == "threads") bg.emplace_back([=] { threads(pid, secs, out + "/threads.csv", n[1] > 0 ? n[1] : 50); });
        else if (c == "log") bg.emplace_back([=] { tail(secs, out + "/mods.log"); });
        else if (c == "ui") bg.emplace_back([=] { ui(pid, secs, out + "/ui.csv"); });
        else if (c == "strip") bg.emplace_back([=] { strip(n[0], n[1], n[2], n[3], n[4], n[5] > 0 ? n[5] : 100, dump, out, sname); });
        else if (c == "click") click(atoi(arg(2).c_str()), atoi(arg(3).c_str()), arg(4, "left"));
        else if (c == "wheel") { SetProcessDPIAware(); SetCursorPos(n[0], n[1]); Sleep(100); mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(n[2] * WHEEL_DELTA), 0); }
        else if (c == "key") for (int i = 0; i < (n[1] > 0 ? n[1] : 1); i++) key(arg(2));
        else if (c == "type") type(arg(2));
        else if (c == "suspend") suspend(pid, atoi(arg(2, "50").c_str()));
        else if (c == "end") break;
        t = nowMs();
        if (c == "click" || c == "key" || c == "suspend") fprintf(log, "%.0f %s %s done\n", t, clock(t).c_str(), c.c_str()), fflush(log);
    }
    for (auto &b : bg) b.join();
    double t = nowMs();
    fprintf(log, "%.0f %s end\n", t, clock(t).c_str());
    fclose(log);
    fclose(s);
    if (FILE *d = fopen((out + "/done").c_str(), "w")) fclose(d);
    return 0;
}

int main(int argc, char **argv) {
    std::vector<std::string> a(argv + 1, argv + argc);
    if (a.empty()) {  // from a scheduled task: the arguments are in a file
        if (FILE *f = fopen("C:\\rb\\glitch.args", "r")) {
            char b[512];
            while (fscanf(f, "%511s", b) == 1) a.push_back(b);
            fclose(f);
        }
    }
    auto arg = [&](size_t i, const char *d = "") { return a.size() > i ? a[i] : std::string(d); };
    if (arg(0) == "rec" && a.size() >= 3) return rec(a[1], atof(a[2].c_str()), arg(3));
    if (arg(0) == "suspend" && a.size() >= 3) return suspend(atoi(a[1].c_str()), atoi(a[2].c_str()));
    if (arg(0) == "threads" && a.size() >= 4) return threads(atoi(a[1].c_str()), atof(a[2].c_str()), a[3], atoi(arg(4, "50").c_str()));
    if (arg(0) == "run" && a.size() >= 3) return run(a[1], a[2]);
    printf("usage: rbglitch rec|suspend|threads|run ... (see tools/win/glitch/rbglitch.cpp)\n");
    return 1;
}
