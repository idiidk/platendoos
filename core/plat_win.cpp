// rbm::plat on Windows: a hidden message-only window on rekordbox's main thread carries onMain and the timers
// (created by image_win.cpp's entry hook, outside the loader lock), a small thread pool does the rest.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincred.h>
#include <winioctl.h>
#include <winhttp.h>
#include <io.h>
#include "plat.h"
#include "settings.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// Its settings (core/settings.h): what the settings window and the config file's list show of them.
namespace opt {
const rbm::Setting workerPriority{"worker_priority", 1, {.label = "Priority of background work", .help = "What the mods' analysis and separation threads run at, so the window and the audio stay quick. Automatic: below normal on a PC with four or more cores.", .choices = {{"1", "Automatic"}, {"0", "Normal"}, {"-1", "Below normal"}, {"-2", "Lowest"}}, .advanced = true, .page = "about"}};
}  // namespace opt

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace rbm {  // core.cpp (rbmod.h)
double cfgNum(const char* key, double def);
void logf(const char* fmt, ...);
}

namespace rbm::plat {

// ---- main thread --------------------------------------------------------------------------

static DWORD g_mainTid;
static HWND g_wnd;
static std::mutex g_mainMx;
static std::deque<std::function<void()>> g_mainQ;  // queued before the window exists, or for WM_APP
struct TimerJob { std::function<void()> fn; bool repeat; };
static std::map<UINT_PTR, TimerJob> g_timers;  // main thread only
static UINT_PTR g_nextTimer = 1;
static std::vector<std::pair<double, std::function<void()>>> g_pendingTimers;  // set before the window exists (under g_mainMx)
static std::vector<std::pair<double, std::function<void()>>> g_pendingRepeats;
static const UINT WM_RBM_RUN = WM_APP + 0x5242;

static void drainMain() {
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> l(g_mainMx);
            if (g_mainQ.empty()) return;
            fn = std::move(g_mainQ.front());
            g_mainQ.pop_front();
        }
        fn();
    }
}

static void addTimer(double seconds, std::function<void()> fn, bool repeat) {  // main thread, window exists
    UINT_PTR id = g_nextTimer++;
    g_timers[id] = {std::move(fn), repeat};
    SetTimer(g_wnd, id, (UINT)std::max(1.0, seconds * 1000), nullptr);
}

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_RBM_RUN) { drainMain(); return 0; }
    if (msg == WM_TIMER) {
        auto it = g_timers.find(wp);
        if (it == g_timers.end()) return 0;
        if (!it->second.repeat) {
            KillTimer(h, wp);
            auto fn = std::move(it->second.fn);
            g_timers.erase(it);
            fn();
        } else {
            it->second.fn();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Called once on the main thread before rekordbox's own code runs (image_win.cpp).
void startMainThread() {
    g_mainTid = GetCurrentThreadId();
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"platendoos.main";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    std::vector<std::pair<double, std::function<void()>>> once, rep;
    {
        std::lock_guard<std::mutex> l(g_mainMx);
        once.swap(g_pendingTimers);
        rep.swap(g_pendingRepeats);
    }
    for (auto& [s, fn] : once) addTimer(s, std::move(fn), false);
    for (auto& [s, fn] : rep) addTimer(s, std::move(fn), true);
    PostMessageW(g_wnd, WM_RBM_RUN, 0, 0);
}

void setMainThread(DWORD tid) { g_mainTid = tid; }

bool isMainThread() { return GetCurrentThreadId() == g_mainTid; }

void onMain(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> l(g_mainMx);
        g_mainQ.push_back(std::move(fn));
    }
    if (g_wnd) PostMessageW(g_wnd, WM_RBM_RUN, 0, 0);
}

void onMainAfter(double seconds, std::function<void()> fn) {
    if (!g_wnd) {
        std::lock_guard<std::mutex> l(g_mainMx);
        g_pendingTimers.push_back({seconds, std::move(fn)});
        return;
    }
    if (isMainThread()) return addTimer(seconds, std::move(fn), false);
    auto box = std::make_shared<std::function<void()>>(std::move(fn));
    onMain([seconds, box] { addTimer(seconds, std::move(*box), false); });
}

// (ExitProcess ends every thread before the DLL's statics are destroyed: nothing to stop first)
void onQuit(std::function<void()>) {}
void everyOnMain(double seconds, std::function<void()> fn) {
    if (!g_wnd) {
        std::lock_guard<std::mutex> l(g_mainMx);
        g_pendingRepeats.push_back({seconds, std::move(fn)});
        return;
    }
    if (isMainThread()) return addTimer(seconds, std::move(fn), true);
    auto box = std::make_shared<std::function<void()>>(std::move(fn));
    onMain([seconds, box] { addTimer(seconds, std::move(*box), true); });
}

// ---- workers ------------------------------------------------------------------------------

// Set by lowPriorityThread(): a thread-pool callback must end at the priority it started with, or Windows ends the
// process (STATUS_CALLBACK_RETURNED_PRI_BACK, 0xC0000720), so background() restores it after the task.
static thread_local bool t_lowPriority;

// ---- worker priority --------------------------------------------------------------------
// What the mods compute runs one step below normal priority: every thread they start (workerThreadAttach, called
// for each new thread of the process) and their thread-pool jobs (background()). rekordbox's own threads, the one
// that draws and answers the mouse first of all, then get a core before the mods' work does; the mods' threads
// still use every core nothing else wants. Measured on a 6-core PC (2026-11-19): Beat This!'s preparation of a
// track (resampling, the mel spectrogram: parallelFor on every core for half a second) started at the moment
// the loaded deck was redrawn with its waveforms, and rekordbox's window stopped answering for 0.3-0.7 s (plain
// rekordbox: 0.13-0.15 s at that moment).
// config: worker_priority=-1  0: normal priority, as rekordbox's own threads; -2: two steps below. The default is
//     -1 on a PC with four or more logical cores and 0 on a smaller one: there rekordbox's own analysis can keep
//     every core busy, and a waiting mod thread that holds a lock would hold up whoever needs it.
static ULONG_PTR g_selfBase, g_selfEnd;
typedef LONG(NTAPI* NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);
static NtQueryInformationThreadFn g_queryThread;
static unsigned g_cores;
extern "C" IMAGE_DOS_HEADER __ImageBase;

// worker_priority as a thread priority, read from the config each time (the config is loaded after DllMain).
static int workerPriority() {
    int v = (int)opt::workerPriority.number();
    if (v > 0) v = g_cores >= 4 ? -1 : 0;
    return v >= 0 ? THREAD_PRIORITY_NORMAL : v == -1 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
}

// DllMain, process attach.
void workerPriorityInit() {
    g_cores = std::thread::hardware_concurrency();
    auto* nt = (const IMAGE_NT_HEADERS*)((const char*)&__ImageBase + __ImageBase.e_lfanew);
    g_selfBase = (ULONG_PTR)&__ImageBase, g_selfEnd = g_selfBase + nt->OptionalHeader.SizeOfImage;
    g_queryThread = (NtQueryInformationThreadFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
}

// DllMain, thread attach: on every new thread of the process, before its own code runs. A thread the mods started
// (std::thread: its start routine is the C runtime's, inside this DLL) gets the workers' priority. rekordbox's
// own threads and those of other DLLs start elsewhere and are left alone. (The loader lock is held here: cfgNum
// only looks the key up under the config's own short lock.)
void workerThreadAttach() {
    if (!g_queryThread) return;
    ULONG_PTR start = 0;
    if (g_queryThread(GetCurrentThread(), 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, nullptr) < 0) return;
    if (start < g_selfBase || start >= g_selfEnd) return;
    int p = workerPriority();
    if (p != THREAD_PRIORITY_NORMAL) SetThreadPriority(GetCurrentThread(), p);
}

static void logWorkerPriority() {  // once, with the first job
    static std::once_flag once;
    std::call_once(once, [] {
        int p = workerPriority();
        logf("rbmods: the mods' threads run at %s priority (worker_priority=%d)",
             p == THREAD_PRIORITY_NORMAL ? "normal" : p == THREAD_PRIORITY_BELOW_NORMAL ? "one step below normal" : "two steps below normal",
             p == THREAD_PRIORITY_NORMAL ? 0 : p == THREAD_PRIORITY_BELOW_NORMAL ? -1 : -2);
    });
}

void background(std::function<void()> fn) {
    logWorkerPriority();
    auto box = new std::function<void()>(std::move(fn));
    if (!TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, void* ctx) {
            std::unique_ptr<std::function<void()>> f((std::function<void()>*)ctx);
            // (Windows' own pool thread: lowered for the job only, see t_lowPriority above)
            int p = workerPriority(), old = GetThreadPriority(GetCurrentThread());
            bool lower = p != THREAD_PRIORITY_NORMAL && old == THREAD_PRIORITY_NORMAL && SetThreadPriority(GetCurrentThread(), p);
            (*f)();
            if (t_lowPriority) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END), t_lowPriority = false;
            if (lower) SetThreadPriority(GetCurrentThread(), old);
        }, box, nullptr)) {
        std::thread([box] { std::unique_ptr<std::function<void()>> f(box); (*f)(); }).detach();
    }
}

void parallelFor(size_t n, const std::function<void(size_t)>& fn) {
    if (n == 0) return;
    size_t threads = std::min<size_t>(n, std::max(1u, std::thread::hardware_concurrency()));
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i; (i = next++) < n;) fn(i);
    };
    // The helpers are threads of ours (workerThreadAttach lowers them); the calling thread does its share at the
    // same priority when it is one of rekordbox's analysis threads, not when it is the main thread.
    int p = workerPriority(), old = GetThreadPriority(GetCurrentThread());
    bool lower = threads > 1 && p != THREAD_PRIORITY_NORMAL && old == THREAD_PRIORITY_NORMAL && !t_lowPriority && !isMainThread() &&
                 SetThreadPriority(GetCurrentThread(), p);
    std::vector<std::thread> ts;
    for (size_t t = 1; t < threads; t++) ts.emplace_back(work);
    work();
    for (auto& t : ts) t.join();
    if (lower) SetThreadPriority(GetCurrentThread(), old);
}

struct Queue::Impl {
    std::mutex mx;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    std::thread th;
    bool quit = false;
    DWORD tid = 0;
};
Queue::Queue(const char*) : impl(new Impl) {
    Impl* p = impl.get();
    p->th = std::thread([p] {
        p->tid = GetCurrentThreadId();
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> l(p->mx);
                p->cv.wait(l, [p] { return p->quit || !p->q.empty(); });
                if (p->q.empty()) return;
                fn = std::move(p->q.front());
                p->q.pop_front();
            }
            fn();
        }
    });
}
Queue::~Queue() {
    {
        std::lock_guard<std::mutex> l(impl->mx);
        impl->quit = true;
    }
    impl->cv.notify_all();
    if (impl->th.joinable()) impl->th.join();
}
void Queue::async(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> l(impl->mx);
        impl->q.push_back(std::move(fn));
    }
    impl->cv.notify_one();
}
void Queue::sync(const std::function<void()>& fn) {
    if (GetCurrentThreadId() == impl->tid) return fn();
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    async([&] {
        fn();
        std::lock_guard<std::mutex> l(m);
        done = true;
        cv.notify_one();
    });
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return done; });
}

// ---- time ---------------------------------------------------------------------------------

double now() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return (double)(u.QuadPart - 126227808000000000ULL) / 1e7;  // seconds since 2001-01-01, like CFAbsoluteTime
}
double monotonic() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}
double idleSeconds() {
    LASTINPUTINFO li{sizeof li};
    if (!GetLastInputInfo(&li)) return 0;
    return (GetTickCount() - li.dwTime) / 1000.0;
}

// ---- files --------------------------------------------------------------------------------

static std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = narrow(p);
    CoTaskMemFree(p);
    for (char& c : r) if (c == '\\') c = '/';
    return r;
}
static std::string ensureDir(std::string d) {
    std::error_code ec;
    std::filesystem::create_directories(fsPath(d), ec);
    return d;
}

std::filesystem::path fsPath(const std::string& s) { return std::filesystem::path(wide(s)); }
std::string utf8(const std::filesystem::path& p) {
    std::string s = narrow(p.wstring());
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}
std::string homeDir() { return knownFolder(FOLDERID_Profile); }
std::string configDir() { return ensureDir(knownFolder(FOLDERID_RoamingAppData) + "/rekordbox-mods"); }
std::string logFile() { return ensureDir(knownFolder(FOLDERID_LocalAppData) + "/rekordbox-mods") + "/rekordbox-mods.log"; }
std::string cacheDir() { return ensureDir(knownFolder(FOLDERID_LocalAppData) + "/rekordbox-mods/cache"); }
std::string resourcesDir() {  // next to rbmods.dll
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&resourcesDir, &self);
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(self, buf, (DWORD)std::size(buf));
    std::string p = utf8(std::filesystem::path(std::wstring(buf, n)).parent_path());
    return p + "/rbmods";
}
FILE* openFile(const std::string& s, const char* mode) { return _wfopen(wide(s).c_str(), wide(mode).c_str()); }
bool readFile(const std::string& s, std::string* out) {
    FILE* f = openFile(s, "rb");
    if (!f) return false;
    out->clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    bool ok = !ferror(f);  // (a drive unplugged mid-read: not the whole file)
    fclose(f);
    return ok;
}
bool writeFileAtomic(const std::string& s, const void* data, size_t n, bool flush) {
    std::string tmp = s + ".tmp" + std::to_string(GetCurrentProcessId());
    FILE* f = openFile(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    if (flush && ok) ok = fflush(f) == 0 && FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f)));  // (the C runtime's buffer, then Windows')
    ok = fclose(f) == 0 && ok;
    if (ok) ok = MoveFileExW(wide(tmp).c_str(), wide(s).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(wide(tmp).c_str());
    return ok;
}
uint64_t diskFree(const std::string& s) {
    ULARGE_INTEGER avail;
    std::wstring w = wide(s);
    return GetDiskFreeSpaceExW(w.c_str(), &avail, nullptr, nullptr) ? avail.QuadPart : 0;
}
bool externalVolume(const std::string& s) {
    std::wstring w = wide(s);
    for (wchar_t& c : w) if (c == L'/') c = L'\\';
    // the nearest folder that exists (a cache folder that is still to be made)
    while (w.size() > 3 && GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) {
        size_t cut = w.find_last_of(L'\\', w.size() - 2);
        if (cut == std::wstring::npos) break;
        w.resize(cut + 1);
    }
    wchar_t root[MAX_PATH + 1];  // "E:\\", "\\\\server\\share\\" or the folder a volume is mounted in
    if (!GetVolumePathNameW(w.c_str(), root, MAX_PATH)) return false;
    UINT type = GetDriveTypeW(root);
    if (type == DRIVE_REMOVABLE || type == DRIVE_REMOTE || type == DRIVE_CDROM) return true;
    if (type != DRIVE_FIXED) return false;
    // An external disk is "fixed" too: ask what it is connected with. (Opening the volume for no access needs no rights.)
    wchar_t volume[64];  // "\\\\?\\Volume{guid}\\"
    if (!GetVolumeNameForVolumeMountPointW(root, volume, 64)) return false;
    size_t n = wcslen(volume);
    if (n && volume[n - 1] == L'\\') volume[n - 1] = 0;  // the volume itself, not its root folder
    HANDLE h = CreateFileW(volume, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    union { STORAGE_DEVICE_DESCRIPTOR d; char bytes[1024]; } out{};
    DWORD got = 0;
    bool ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &out, sizeof out, &got, nullptr) != 0;
    CloseHandle(h);
    if (!ok || got < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawPropertiesLength)) return false;
    switch (out.d.BusType) {
        case BusTypeUsb: case BusType1394: case BusTypeSd: case BusTypeMmc: return true;
        default: return out.d.RemovableMedia != 0;
    }
}
void openPath(const std::string& s) { ShellExecuteW(nullptr, L"open", wide(s).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void openTextFile(const std::string& s) {
    std::wstring w = wide(s);
    for (wchar_t& c : w) if (c == L'/') c = L'\\';
    std::wstring args = L"\"" + w + L"\"";
    ShellExecuteW(nullptr, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}
// (Run on the test PC, 2026-11-26: rekordbox closed and came back with its mods after about 40 s.)
bool restartApp() {
    // platendoos.exe is next to rbmods.dll (resourcesDir() is the rbmods folder there); it gets the rekordbox.exe that
    // runs now, so a rekordbox started from another place (--rekordbox) comes back as the same one.
    std::wstring launcher = (std::filesystem::path(wide(resourcesDir())).parent_path() / L"platendoos.exe").wstring();
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    if (!n || n >= std::size(exe) || GetFileAttributesW(launcher.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    auto quoted = [](std::wstring s) {  // inside a PowerShell '...' string
        for (size_t i = 0; (i = s.find(L'\'', i)) != std::wstring::npos; i += 2) s.insert(i, 1, L'\'');
        return L"'" + s + L"'";
    };
    // A hidden PowerShell waits for this process to end (a minute at most: rekordbox may ask something first), and
    // starts platendoos again only when it has.
    std::wstring pid = std::to_wstring(GetCurrentProcessId());
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command \"Wait-Process -Id " + pid +
                       L" -Timeout 60 -ErrorAction SilentlyContinue; if (-not (Get-Process -Id " + pid +
                       L" -ErrorAction SilentlyContinue)) { Start-Process -FilePath " + quoted(launcher) +
                       L" -ArgumentList '--rekordbox', ([char]34 + " + quoted(std::wstring(exe, n)) + L" + [char]34) }\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // As the window's close button: rekordbox's own quit. Its main window is its largest visible one titled "rekordbox".
    struct Find { HWND best; LONG area; } f{nullptr, 0};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            DWORD p;
            GetWindowThreadProcessId(h, &p);
            wchar_t t[32];
            if (p != GetCurrentProcessId() || !IsWindowVisible(h) || !GetWindowTextW(h, t, 32) || wcscmp(t, L"rekordbox")) return TRUE;
            RECT r;
            GetWindowRect(h, &r);
            Find* f = (Find*)lp;
            LONG a = (r.right - r.left) * (r.bottom - r.top);
            if (a > f->area) f->best = h, f->area = a;
            return TRUE;
        },
        (LPARAM)&f);
    if (!f.best) return false;  // (the helper then waits its minute and starts nothing: rekordbox is still there)
    PostMessageW(f.best, WM_CLOSE, 0, 0);
    return true;
}
// rekordbox.exe's file version (7.2.19.x -> "7.2.19").
std::string appVersion() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD n = GetFileVersionInfoSizeW(exe, nullptr);
    std::vector<char> buf(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!n || !GetFileVersionInfoW(exe, 0, n, buf.data()) || !VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi)
        return "?";
    char v[48];
    snprintf(v, sizeof v, "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
    return v;
}
std::string nfc(const std::string& s) {
    std::wstring w = wide(s);
    int n = NormalizeString(NormalizationC, w.c_str(), (int)w.size(), nullptr, 0);
    if (n <= 0) return s;
    std::wstring out(n, L'\0');
    n = NormalizeString(NormalizationC, w.c_str(), (int)w.size(), out.data(), n);
    if (n <= 0) return s;
    out.resize(n);
    return narrow(out);
}
int httpPostJson(const std::string& url, const std::string& bearer, const std::string& json, std::string* body, double timeout) {
    std::wstring u = wide(url);
    URL_COMPONENTS c{sizeof c};
    wchar_t host[256], path[2048];
    c.lpszHostName = host, c.dwHostNameLength = 256;
    c.lpszUrlPath = path, c.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(u.c_str(), 0, 0, &c)) return 0;
    static HINTERNET session = WinHttpOpen(L"platendoos", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    int ms = (int)(timeout * 1000), status = 0;
    HINTERNET conn = WinHttpConnect(session, host, c.nPort, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              c.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    if (req) {
        WinHttpSetTimeouts(req, ms, ms, ms, ms);
        std::wstring headers = L"Content-Type: application/json\r\nX-Title: platendoos\r\n";
        if (!bearer.empty()) headers += L"Authorization: Bearer " + wide(bearer) + L"\r\n";
        if (WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L, (LPVOID)json.data(), (DWORD)json.size(), (DWORD)json.size(), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, len = sizeof code;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                                WINHTTP_NO_HEADER_INDEX);
            std::string data;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
                size_t at = data.size();
                data.resize(at + avail);
                if (!WinHttpReadData(req, data.data() + at, avail, &got)) { data.resize(at); break; }
                data.resize(at + got);
            }
            status = (int)code;
            if (body) *body = std::move(data);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    return status;
}

bool secretSet(const std::string& name, const std::string& value) {
    std::wstring target = L"platendoos/" + wide(name);
    if (value.empty()) return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = target.data();
    c.CredentialBlobSize = (DWORD)value.size();
    c.CredentialBlob = (LPBYTE)value.data();
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    wchar_t user[] = L"platendoos";
    c.UserName = user;
    return CredWriteW(&c, 0) != 0;
}
std::string secretGet(const std::string& name) {
    std::wstring target = L"platendoos/" + wide(name);
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &c)) return "";
    std::string v((const char*)c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return v;
}

int httpGet(const std::string& url, std::string* body, double timeout) {
    std::wstring u = wide(url);
    URL_COMPONENTS c{sizeof c};
    wchar_t host[256], path[2048];
    c.lpszHostName = host, c.dwHostNameLength = 256;
    c.lpszUrlPath = path, c.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(u.c_str(), 0, 0, &c)) return 0;
    static HINTERNET session = WinHttpOpen(L"platendoos", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    int ms = (int)(timeout * 1000);
    HINTERNET conn = WinHttpConnect(session, host, c.nPort, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              c.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    int status = 0;
    if (req) {
        WinHttpSetTimeouts(req, ms, ms, ms, ms);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, len = sizeof code;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                                WINHTTP_NO_HEADER_INDEX);
            std::string data;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
                size_t at = data.size();
                data.resize(at + avail);
                if (!WinHttpReadData(req, data.data() + at, avail, &got)) { data.resize(at); break; }
                data.resize(at + got);
            }
            status = (int)code;
            if (body) *body = std::move(data);
        }
        WinHttpCloseHandle(req);
    }
    if (conn) WinHttpCloseHandle(conn);
    return status;
}
void revealPath(const std::string& s) {
    std::wstring w = wide(s);
    for (wchar_t& c : w) if (c == L'/') c = L'\\';
    std::wstring args = L"/select,\"" + w + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static double fileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return (double)(u.QuadPart - 116444736000000000ULL) / 1e7;
}
static FILETIME unixToFileTime(double t) {
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)(t * 1e7) + 116444736000000000ULL;
    return {u.LowPart, u.HighPart};
}

bool fileInfo(const std::string& s, FileInfo* out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(wide(s).c_str(), GetFileExInfoStandard, &a)) return false;
    if (!out) return true;  // (only whether it exists)
    out->size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    out->mtime = fileTimeToUnix(a.ftLastWriteTime);
    out->isDir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

bool setModified(const std::string& s, double mtime) {
    HANDLE h = CreateFileW(wide(s).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME ft = unixToFileTime(mtime);
    bool ok = SetFileTime(h, nullptr, &ft, &ft) != 0;
    CloseHandle(h);
    return ok;
}

// Shared for reading, writing and deleting: rekordbox reads audio files the mods are still writing.
bool File::open(const std::string& s, bool create, bool readOnly) {
    close();
    HANDLE h = CreateFileW(wide(s).c_str(), readOnly ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    h_ = (intptr_t)h;
    return true;
}
static bool rw(intptr_t h, uint64_t offset, void* data, size_t n, bool write) {
    if (h == -1) return false;
    char* p = (char*)data;
    while (n) {
        OVERLAPPED o{};
        o.Offset = (DWORD)offset;
        o.OffsetHigh = (DWORD)(offset >> 32);
        DWORD chunk = (DWORD)std::min<size_t>(n, 1u << 30), done = 0;
        BOOL ok = write ? WriteFile((HANDLE)h, p, chunk, &done, &o) : ReadFile((HANDLE)h, p, chunk, &done, &o);
        if (!ok || done != chunk) return false;
        p += done, offset += done, n -= done;
    }
    return true;
}
bool File::readAt(uint64_t offset, void* data, size_t n) { return rw(h_, offset, data, n, false); }
bool File::writeAt(uint64_t offset, const void* data, size_t n) { return rw(h_, offset, (void*)data, n, true); }
uint64_t File::size() {
    LARGE_INTEGER n;
    return h_ != -1 && GetFileSizeEx((HANDLE)h_, &n) ? (uint64_t)n.QuadPart : 0;
}
bool File::resize(uint64_t n) {
    FILE_END_OF_FILE_INFO e;
    e.EndOfFile.QuadPart = (LONGLONG)n;
    return h_ != -1 && SetFileInformationByHandle((HANDLE)h_, FileEndOfFileInfo, &e, sizeof e);
}
bool File::sync() { return h_ != -1 && FlushFileBuffers((HANDLE)h_); }
void File::close() {
    if (h_ != -1) CloseHandle((HANDLE)h_);
    h_ = -1;
}

bool copyFile(const std::string& from, const std::string& to) {
    // ReFS block cloning would need more; a plain copy (attributes and times come along)
    return CopyFileExW(wide(from).c_str(), wide(to).c_str(), nullptr, nullptr, nullptr, COPY_FILE_FAIL_IF_EXISTS) != 0;
}

// One argument in the form CommandLineToArgvW (and the C runtime) takes apart again.
static std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (size_t i = 0;; i++) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') i++, slashes++;
        if (i == a.size()) { q.append(slashes * 2, L'\\'); break; }
        if (a[i] == L'"') q.append(slashes * 2 + 1, L'\\');
        else q.append(slashes, L'\\');
        q += a[i];
    }
    return q + L"\"";
}

int run(const std::vector<std::string>& argv, const std::string& cwd, const std::string& input, std::string* out, double timeout) {
    if (out) out->clear();
    if (argv.empty()) return -1;
    std::wstring exe = wide(argv[0]), cmd;
    for (wchar_t& c : exe)
        if (c == L'/') c = L'\\';
    for (size_t i = 0; i < argv.size(); i++) cmd += (i ? L" " : L"") + quoteArg(i ? wide(argv[i]) : exe);
    size_t dot = exe.rfind(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : exe.substr(dot);
    for (wchar_t& c : ext) c = towlower(c);
    bool script = ext == L".cmd" || ext == L".bat";
    if (script) {  // a batch file is run by cmd.exe
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        exe = std::wstring(sys) + L"\\cmd.exe";
        cmd = L"cmd.exe /d /s /c \"" + cmd + L"\"";
    }
    // One start at a time: a child inherits every inheritable handle there is, so while this call's pipe ends are
    // inheritable no other run() may start its program (that one would hold this pipe open: the output here would
    // not end until the other program did).
    static std::mutex startLock;
    std::unique_lock<std::mutex> starting(startLock);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0)) return -1;
    if (!CreatePipe(&outR, &outW, &sa, 0)) { CloseHandle(inR), CloseHandle(inW); return -1; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si, si.dwFlags = STARTF_USESTDHANDLES, si.hStdInput = inR, si.hStdOutput = outW, si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::wstring dir = wide(cwd);
    // In a job of its own, so that the timeout ends what it started too (a batch file's program holds the pipe).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    if (ok) {
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
    }
    CloseHandle(inR), CloseHandle(outW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    starting.unlock();
    if (!ok) { CloseHandle(inW), CloseHandle(outR); if (job) CloseHandle(job); return -1; }
    std::thread writer([inW, input] {  // (its own thread: the program may not read until it has written)
        size_t at = 0;
        while (at < input.size()) {
            DWORD n = 0;
            if (!WriteFile(inW, input.data() + at, (DWORD)std::min<size_t>(input.size() - at, 1 << 16), &n, nullptr) || !n) break;
            at += n;
        }
        CloseHandle(inW);
    });
    std::atomic<bool> done{false}, killed{false};
    std::thread watchdog([&] {  // ReadFile below has no timeout of its own
        double end = monotonic() + timeout;
        while (!done && monotonic() < end) Sleep(100);
        if (!done) { killed = true; if (job) TerminateJobObject(job, 1); TerminateProcess(pi.hProcess, 1); }
    });
    char buf[65536];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(outR, buf, sizeof buf, &n, nullptr) || !n) break;
        if (out) out->append(buf, n);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    done = true;
    watchdog.join(), writer.join();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(outR), CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    if (job) CloseHandle(job);
    return killed ? -1 : (int)code;
}

void lowPriorityThread(bool low) {
    if (low && !t_lowPriority && SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN)) t_lowPriority = true;
    if (!low && t_lowPriority && SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END)) t_lowPriority = false;
}

// ---- process ------------------------------------------------------------------------------

bool patchMemory(void* addr, const void* bytes, size_t len) {
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, bytes, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return true;
}

void enterCriticalSection(void* cs) { EnterCriticalSection((CRITICAL_SECTION*)cs); }
void leaveCriticalSection(void* cs) { LeaveCriticalSection((CRITICAL_SECTION*)cs); }

std::string sha256Hex(const void* data, size_t n) {
    uint8_t d[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data, (ULONG)n, d, 32) != 0) return "";
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    return hex;
}

void* loadLibraryFunction(const std::string& path, const char* name) {
    HMODULE h = LoadLibraryW(wide(path).c_str());
    return h ? (void*)GetProcAddress(h, name) : nullptr;
}

}  // namespace rbm::plat
