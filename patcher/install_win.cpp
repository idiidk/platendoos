// platendoos patcher on Windows: installs the platendoos folder (platendoos.exe, rbmods.dll, rbmods\) to
// %LOCALAPPDATA%\Programs\platendoos with Start menu (and desktop) shortcuts and an Apps & features entry.
// rekordbox's own files stay as they are; only the FLX4 mapping, when asked for, is replaced by the extended one
// (with Windows' permission prompt; the original is kept next to it as .original and put back on uninstall).
// The patcher sits in the platendoos folder it installs from and is copied along, as the uninstaller.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <initguid.h>  // defines the PKEY_ below here
#include <propkey.h>
#include "patcher.h"
#include "flx4map.h"
#include "retable.h"
#include "patcher_gen.h"
#include "../core/plat.h"
#include "../core/rbmod.h"
#include <stdarg.h>
#include <filesystem>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace {

const char* kTestedBuild = "6A97B2E4-6573000";  // rekordbox 7.2.19 (core/image_win.cpp)
const char* kCheckedBuild = "6AFC1795-66AA000";  // rekordbox 7.2.20: every mod was run on it with its shipped table
const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\platendoos";

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::string u8(const fs::path& p) { return narrow(p.wstring()); }

fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

fs::path selfPath() {
    wchar_t b[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 4);
    return fs::path(std::wstring(b, n));
}
fs::path payload() { return selfPath().parent_path(); }  // the platendoos folder the patcher sits in
// PLATENDOOS_DEST: a test install elsewhere, which leaves the shortcuts and the Apps & features entry alone
bool testDest() { return _wgetenv(L"PLATENDOOS_DEST") != nullptr; }
fs::path dest() {
    if (const wchar_t* d = _wgetenv(L"PLATENDOOS_DEST")) return d;
    return knownFolder(FOLDERID_LocalAppData) / L"Programs" / L"platendoos";
}
fs::path startShortcut() { return knownFolder(FOLDERID_Programs) / L"platendoos.lnk"; }
fs::path desktopShortcut() { return knownFolder(FOLDERID_Desktop) / L"platendoos.lnk"; }

// Whether folder name a is a newer version than b: runs of digits compare as numbers ("rekordbox 7.2.19" is newer
// than "rekordbox 7.2.9", which as text it is not).
static bool newerName(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (iswdigit(a[i]) && iswdigit(b[j])) {
            unsigned long long x = 0, y = 0;
            while (i < a.size() && iswdigit(a[i])) x = x * 10 + (a[i++] - L'0');
            while (j < b.size() && iswdigit(b[j])) y = y * 10 + (b[j++] - L'0');
            if (x != y) return x > y;
        } else {
            if (a[i] != b[j]) return a[i] > b[j];
            i++, j++;
        }
    }
    return a.size() - i > b.size() - j;
}

// The newest rekordbox.exe in Program Files. PLATENDOOS_SOURCE: a rekordbox.exe elsewhere instead (testing another
// version, e.g. one unpacked from its installer; start it with platendoos.exe --rekordbox <that exe>).
fs::path findRekordbox() {
    fs::path best;
    std::error_code ec;
    if (const wchar_t* s = _wgetenv(L"PLATENDOOS_SOURCE")) return fs::exists(s, ec) ? fs::path(s) : fs::path();
    for (const wchar_t* root : {L"C:\\Program Files\\rekordbox", L"C:\\Program Files\\Pioneer\\rekordbox"})
        for (auto& d : fs::directory_iterator(root, ec)) {
            fs::path exe = d.path() / L"rekordbox.exe";
            if (fs::exists(exe, ec) && (best.empty() || newerName(d.path().filename().wstring(), best.parent_path().filename().wstring()))) best = exe;
        }
    return best;
}

// <TimeDateStamp>-<SizeOfImage>, as core/image_win.cpp names the build ("" if it isn't a 64-bit program).
std::string buildId(const fs::path& exe) {
    FILE* f = _wfopen(exe.c_str(), L"rb");
    if (!f) return "";
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    bool ok = fread(&dos, sizeof dos, 1, f) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE && fseek(f, dos.e_lfanew, SEEK_SET) == 0 &&
              fread(&nt, sizeof nt, 1, f) == 1 && nt.Signature == IMAGE_NT_SIGNATURE &&
              nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    fclose(f);
    if (!ok) return "";
    char b[40];
    snprintf(b, sizeof b, "%08lX-%lX", (unsigned long)nt.FileHeader.TimeDateStamp, (unsigned long)nt.OptionalHeader.SizeOfImage);
    return b;
}

std::string fileVersion(const fs::path& exe) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(exe.c_str(), &h);
    std::vector<uint8_t> buf(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!n || !GetFileVersionInfoW(exe.c_str(), 0, n, buf.data()) || !VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) return "";
    char b[48];
    snprintf(b, sizeof b, "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
    return b;
}

bool readFile(const fs::path& p, std::string* out) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    out->clear();
    char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out->append(b, n);
    fclose(f);
    return true;
}
bool writeFile(const fs::path& p, const std::string& s) {
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    return fclose(f) == 0 && ok;
}

// The running rekordbox.exe processes (their paths).
std::vector<std::string> rekordboxRunning() {
    std::vector<std::string> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (!_wcsicmp(e.szExeFile, L"rekordbox.exe")) out.push_back(narrow(e.szExeFile));
    CloseHandle(snap);
    return out;
}

// What is installed, relative to the payload folder: only platendoos's own files (what tools/win/build.sh puts in
// build/win/platendoos), never whatever else is in the folder the patcher was started from (say Downloads).
// The patcher itself is copied separately, as the uninstaller.
std::vector<fs::path> payloadFiles(const fs::path& from, uint64_t* total) {
    static const wchar_t* kFiles[] = {L"platendoos.exe", L"rbmods.dll", L"rbmods.pdb", L"LICENSE", L"NOTICE.md"};  // (.pdb: dev builds only)
    std::vector<fs::path> out;
    std::error_code ec;
    *total = 0;
    for (const wchar_t* f : kFiles)
        if (fs::is_regular_file(from / f, ec)) out.push_back(f), *total += fs::file_size(from / f, ec);
    for (auto& e : fs::recursive_directory_iterator(from / L"rbmods", ec)) {
        if (!e.is_regular_file(ec)) continue;
        out.push_back(fs::relative(e.path(), from, ec));
        *total += e.file_size(ec);
    }
    return out;
}

const wchar_t kAppId[] = L"platendoos.rekordbox";  // rekordbox's window has it too (taskbar icon)

bool makeShortcut(const fs::path& lnk, const fs::path& target, const fs::path& icon, const wchar_t* desc) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(target.parent_path().c_str());
    sl->SetDescription(desc);
    if (!icon.empty()) sl->SetIconLocation(icon.c_str(), 0);
    // the taskbar groups by app id: rekordbox's window gets the same one from the badge mod (mods/badge_view_win.cpp)
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&ps)))) {
        PROPVARIANT pv{};
        pv.vt = VT_LPWSTR;
        pv.pwszVal = (LPWSTR)kAppId;
        ps->SetValue(PKEY_AppUserModel_ID, pv);  // not cleared: the string isn't ours to free
        ps->Commit();
        ps->Release();
    }
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    if (pf) pf->Release();
    sl->Release();
    return ok;
}

void setString(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

// Runs this program elevated (Windows asks for permission) with args; its exit status (-1: refused / failed).
int runElevated(const std::wstring& args) {
    std::wstring self = selfPath().wstring();
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// platendoos's icon: rekordbox's own (from the installed rekordbox.exe, every size it has) with its colours
// inverted, a light tile with a dark logo, as on macOS (tools/build/invert_icon.swift). Written as an .ico with
// 32-bit DIB entries. rekordbox's artwork never ships with platendoos: this is made on the user's PC.
bool invertedIcon(const fs::path& exe, const fs::path& out) {
    HMODULE m = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return false;
    struct Ctx { HMODULE m; std::vector<std::string> entries; std::vector<int> sizes; } c{m};
    // (RT_GROUP_ICON = 14, RT_ICON = 3: the RT_ macros are the A versions without UNICODE)
    EnumResourceNamesW(m, MAKEINTRESOURCEW(14), [](HMODULE m, LPCWSTR, LPWSTR name, LONG_PTR lp) -> BOOL {
        auto* c = (Ctx*)lp;
        HRSRC r = FindResourceW(m, name, MAKEINTRESOURCEW(14));
        auto* dir = r ? (const uint8_t*)LockResource(LoadResource(m, r)) : nullptr;
        if (!dir) return TRUE;
        int count = *(const uint16_t*)(dir + 4);
        for (int i = 0; i < count; i++) {
            const uint8_t* e = dir + 6 + 14 * i;  // GRPICONDIRENTRY
            int size = e[0] ? e[0] : 256, id = *(const uint16_t*)(e + 12);
            HRSRC ir = FindResourceW(m, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(3));
            if (!ir) continue;
            auto* bits = (BYTE*)LockResource(LoadResource(m, ir));
            DWORD n = SizeofResource(m, ir);
            HICON icon = CreateIconFromResourceEx(bits, n, TRUE, 0x30000, size, size, 0);
            if (!icon) continue;
            // its pixels, 32-bit BGRA bottom-up (as an .ico's DIB keeps them)
            ICONINFO ii{};
            GetIconInfo(icon, &ii);
            BITMAPINFO bi{};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, size, 1, 32, BI_RGB};
            std::vector<uint8_t> px((size_t)size * size * 4);
            HDC dc = GetDC(nullptr);
            bool got = ii.hbmColor && GetDIBits(dc, ii.hbmColor, 0, size, px.data(), &bi, DIB_RGB_COLORS) == size;
            ReleaseDC(nullptr, dc);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            DestroyIcon(icon);
            if (!got) continue;
            bool alpha = false;
            for (size_t k = 3; k < px.size() && !alpha; k += 4) alpha = px[k] != 0;
            for (size_t k = 0; k < px.size(); k += 4) {
                if (!alpha) px[k + 3] = 255;
                for (int ch = 0; ch < 3; ch++) px[k + ch] = (uint8_t)(255 - px[k + ch]);
            }
            BITMAPINFOHEADER h = {sizeof h, size, size * 2, 1, 32, BI_RGB};  // ico DIBs: twice the height (+ mask)
            std::string entry((const char*)&h, sizeof h);
            entry.append((const char*)px.data(), px.size());
            entry.append((size_t)((size + 31) / 32 * 4) * size, '\0');  // the AND mask: all opaque (alpha decides)
            c->entries.push_back(entry);
            c->sizes.push_back(size);
        }
        return FALSE;  // the first icon group is the app's
    }, (LONG_PTR)&c);
    FreeLibrary(m);
    if (c.entries.empty()) return false;
    std::string ico;
    auto put16 = [&](int v) { ico.append((const char*)&v, 2); };
    auto put32 = [&](uint32_t v) { ico.append((const char*)&v, 4); };
    put16(0), put16(1), put16((int)c.entries.size());
    uint32_t off = 6 + 16 * (uint32_t)c.entries.size();
    for (size_t i = 0; i < c.entries.size(); i++) {
        ico += (char)(c.sizes[i] % 256), ico += (char)(c.sizes[i] % 256), ico += '\0', ico += '\0';
        put16(1), put16(32), put32((uint32_t)c.entries[i].size()), put32(off);
        off += (uint32_t)c.entries[i].size();
    }
    for (auto& e : c.entries) ico += e;
    return writeFile(out, ico);
}

// A table for a build platendoos has none for, made from the signatures of a build it has one for
// (rbmods\sigs\*.sig, tools/win/makesigs.py): the best of them. Kept per build, find() runs again after an install.
struct Made { retable::Result r; std::string sig; };
Made makeTable(const fs::path& exe, const std::string& build) {
    static std::map<std::string, Made> cache;
    if (auto it = cache.find(build); it != cache.end()) return it->second;
    Made best;
    std::string bytes;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(payload() / L"rbmods" / L"sigs", ec)) {
        std::string sig;
        if (e.path().extension() != L".sig" || !readFile(e.path(), &sig)) continue;
        if (bytes.empty() && !readFile(exe, &bytes)) break;
        retable::Result r = retable::make((const uint8_t*)bytes.data(), bytes.size(), sig);
        if (r.error.empty() && r.build == build && r.found > best.r.found) best = {r, u8(e.path().filename())};
    }
    return cache[build] = best;
}

HWND g_msgWnd;
const UINT WM_RUN = WM_APP + 1;
fs::path g_removeOnExit;  // the installed folder, after an uninstall run from the patcher in it

// The running patcher can't delete itself: on its way out it starts a cmd that removes the folder once the
// patcher's exe is free (tries for up to a minute).
void removeOnExit() {
    if (g_removeOnExit.empty()) return;
    std::wstring dir = quote(g_removeOnExit.wstring());
    std::wstring cmd = L"cmd.exe /d /c for /l %i in (1,1,60) do @(if exist " + dir + L" (rmdir /s /q " + dir +
                       L" 2>nul & if exist " + dir + L" ping -n 2 127.0.0.1 >nul))";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, L"C:\\", &si, &pi))
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
}

}  // namespace

namespace rbm {
void logf(const char* fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    OutputDebugStringA(b);
    OutputDebugStringA("\n");
}
namespace plat {
std::string resourcesDir() { return u8(payload() / L"rbmods"); }
std::string cacheDir() {  // the patcher's web view data
    fs::path p = fs::temp_directory_path() / L"platendoos-patcher";
    std::error_code ec;
    fs::create_directories(p, ec);
    return u8(p);
}
}  // namespace plat
}  // namespace rbm

namespace patcher {

std::string platformName() { return "Windows"; }
std::string configFile() { return u8(knownFolder(FOLDERID_RoamingAppData) / L"rekordbox-mods" / L"config.txt"); }
std::string payloadDir() { return u8(payload()); }
std::string patcherVersion() { return PLATENDOOS_BUILD; }

void onMain(std::function<void()> fn) { PostMessageW(g_msgWnd, WM_RUN, 0, (LPARAM) new std::function<void()>(std::move(fn))); }
void background(std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
    CloseHandle(CreateThread(nullptr, 0, [](void* p) -> DWORD {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* f = (std::function<void()>*)p;
        (*f)();
        delete f;
        CoUninitialize();
        return 0;
    }, f, 0, nullptr));
}
void quit() { PostQuitMessage(0); }

Target find() {
    Target t;
    t.dest = u8(dest());
    std::error_code ec;
    t.installed = fs::exists(dest() / L"platendoos.exe", ec);
    auto add = [&](const char* state, std::string label, std::string detail) { t.checks.push_back({state, label, detail}); };
    auto say = [&](const char* state, std::string text) { t.notes.push_back({state, "", text}); };

    fs::path pay = payload();
    bool payOk = fs::exists(pay / L"platendoos.exe", ec) && fs::exists(pay / L"rbmods.dll", ec) && fs::exists(pay / L"rbmods" / L"ui", ec);
    uint64_t paySize = 0;
    payloadFiles(pay, &paySize);

    fs::path exe = findRekordbox();
    if (exe.empty()) {
        t.problem = "rekordbox isn't installed: there's no rekordbox in C:\\Program Files\\rekordbox.";
        add("fail", "rekordbox", "not found in C:\\Program Files\\rekordbox (install rekordbox 7 from rekordbox.com first)");
        say("fail", "rekordbox isn't installed. Install rekordbox 7 from rekordbox.com, then press Check again.");
        t.mods = modChecks({}, false);
        return t;
    }
    t.found = true;
    t.app = u8(exe);
    t.version = fileVersion(exe);
    add("ok", "rekordbox " + t.version, t.app);

    t.build = buildId(exe);
    if (t.build.empty()) {
        t.problem = "rekordbox.exe couldn't be read.";
        add("fail", "Program", t.problem);
        say("fail", t.problem);
        t.mods = modChecks({}, false);
        return t;
    }
    t.tested = t.build == kTestedBuild;

    // The address table for this build: which hooks, addresses and fields platendoos knows here (core/image_win.cpp).
    fs::path table = pay / L"rbmods" / wide("rekordbox-" + t.build + ".tbl");
    std::string text;
    std::vector<bool> found(gen::kHooks);
    int addrs = 0, offs = 0, hooks = 0;
    bool haveTable = readFile(table, &text);
    Made made;
    if (!haveTable) {  // a newer (or other) rekordbox: look for what platendoos needs in it
        made = makeTable(exe, t.build);
        if (!made.r.table.empty()) text = t.table = made.r.table, haveTable = true;
    }
    if (haveTable) {
        std::map<std::string, int> index;
        for (int i = 0; i < gen::kHooks; i++) index[gen::kHookName[i]] = i;
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            std::string line = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
            p = e == std::string::npos ? text.size() : e + 1;
            if (line.rfind("addr ", 0) == 0) addrs++;
            else if (line.rfind("off ", 0) == 0) offs++;
            else if (line.rfind("hook ", 0) == 0) {
                size_t s = line.find(' ', 5);
                auto it = index.find(line.substr(5, s == std::string::npos ? std::string::npos : s - 5));
                if (it != index.end() && !found[it->second]) found[it->second] = true, hooks++;
            }
        }
    }
    if (t.tested) add("ok", "Build", "the build platendoos was tested with (" + t.build + ")");
    else if (!t.table.empty()) add("warn", "Build", t.build + ": platendoos has no table for it, made one from " + made.sig);
    else if (haveTable && text.find("made by the patcher") != std::string::npos)  // (this is the installed copy)
        add("warn", "Build", t.build + ": not the tested build; its table was made by the patcher");
    else if (haveTable && t.build == kCheckedBuild) add("ok", "Build", "rekordbox 7.2.20: every mod was run on it (" + t.build + ")");
    else if (haveTable) add("warn", "Build", t.build + ": not the tested build, but platendoos has a table for it");
    else add("fail", "Build", t.build + ": platendoos doesn't know this rekordbox build yet");
    if (!haveTable) say("fail", "platendoos doesn't support this rekordbox version (" + t.version + ") yet. Ask for an updated patcher.");
    else if (!t.table.empty())
        say("warn", "platendoos wasn't made for this rekordbox version (" + t.version + "). It found " + std::to_string(made.r.found) +
                        " of the " + std::to_string(made.r.total) + " parts of rekordbox it uses, so some mods may be off (see the list below). An updated patcher may have them all.");
    else if (!t.tested && t.build != kCheckedBuild) say("warn", "platendoos hasn't been tested with this rekordbox version yet.");
    if (haveTable)
        add(t.table.empty() ? "ok" : "warn", "Address table", std::to_string(hooks) + " hook sites, " + std::to_string(addrs) + " addresses, " + std::to_string(offs) + " fields" +
            (t.table.empty() ? "" : " (made here: " + std::to_string(made.r.found) + " of " + std::to_string(made.r.total) + " parts found)"));
    else
        add("fail", "Address table", "none for this build (rbmods\\rekordbox-" + t.build + ".tbl) and no signatures it could be made from: every mod would be off");
    for (size_t i = 0; i < made.r.missing.size() && i < 40; i++) add("warn", "Not found", made.r.missing[i]);
    if (made.r.missing.size() > 40) add("warn", "Not found", "... and " + std::to_string(made.r.missing.size() - 40) + " more");
    t.mods = modChecks(haveTable ? found : std::vector<bool>(), false);

    if (!payOk) add("fail", "Mods", "the patcher's files are missing next to it (" + u8(pay) + ")"),
                say("fail", "The patcher is incomplete. Unzip the whole platendoos folder and run the patcher from there.");
    else add("ok", "Mods", "platendoos.exe, rbmods.dll, models and windows (" + humanSize((double)paySize) + ")");

    ULARGE_INTEGER freeBytes{};
    fs::path root = dest().root_path();
    GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr);
    uint64_t need = paySize + (100ull << 20);
    bool lowDisk = freeBytes.QuadPart && freeBytes.QuadPart < need;
    if (lowDisk) {
        add("fail", "Disk space", humanSize((double)freeBytes.QuadPart) + " free, needs about " + humanSize((double)need));
        say("fail", "Not enough disk space: about " + humanSize((double)need) + " is needed, " + humanSize((double)freeBytes.QuadPart) + " is free.");
    }
    else add("ok", "Disk space", humanSize((double)freeBytes.QuadPart) + " free");

    bool running = !rekordboxRunning().empty();
    if (running) add("fail", "rekordbox", "is running: close it, then Check again"), say("fail", "rekordbox is open. Close it, then press Check again.");
    if (t.installed) add("ok", "platendoos", "installed in " + t.dest + ": it will be updated");
    bool same = fs::equivalent(pay, dest(), ec);
    if (same) {
        add("ok", "platendoos", "this is the installed copy of the patcher (Uninstall removes it)");
        say("ok", "This is the installed copy of the patcher: it can uninstall platendoos. To update, run the new patcher.");
    }

    if (!payOk) t.problem = "The patcher is incomplete.";
    else if (!haveTable) t.problem = "This rekordbox version isn't supported yet.";
    else if (lowDisk) t.problem = "Not enough disk space.";
    else if (running) t.problem = "rekordbox is open.";
    else if (same) t.problem = "";
    t.supported = t.problem.empty() && !same;
    return t;
}

// The extended FLX4 mapping in place of rekordbox's (run elevated: --write-mapping <csv>); rekordbox's own stays
// as <csv>.original, which is always the one transformed (a second run doesn't extend the extended one).
int writeMapping(const std::wstring& csv) {
    fs::path p(csv), orig = p;
    orig += L".original";
    std::error_code ec;
    if (!fs::exists(orig, ec) && !fs::copy_file(p, orig, ec)) return 2;
    std::string src, why;
    if (!readFile(orig, &src)) return 3;
    std::string m = flx4map::make(src, &why);
    if (m.empty()) return 4;
    return writeFile(p, m) ? 0 : 5;
}
int restoreMapping(const std::wstring& csv) {
    fs::path p(csv), orig = p;
    orig += L".original";
    std::error_code ec;
    if (!fs::exists(orig, ec)) return 0;
    fs::copy_file(orig, p, fs::copy_options::overwrite_existing, ec);
    if (ec) return 2;
    fs::remove(orig, ec);
    return 0;
}

std::string install(const Target& t, const Options& o, const Step& step) {
    std::error_code ec;
    fs::path from = payload(), to = dest();
    if (!rekordboxRunning().empty()) return "rekordbox is running: close it first.";

    // the files, by size for the progress bar
    uint64_t total = 0, done = 0;
    std::vector<fs::path> files = payloadFiles(from, &total);
    step("Copying platendoos to " + t.dest, 0.02);
    fs::create_directories(to, ec);
    // old files in rbmods\ that this version doesn't have (tables of other builds stay useful: kept)
    for (auto& e : fs::recursive_directory_iterator(to / L"rbmods" / L"ui", ec))
        if (e.is_regular_file(ec) && !fs::exists(from / fs::relative(e.path(), to, ec), ec)) fs::remove(e.path(), ec);
    double last = 0;
    for (const fs::path& rel : files) {
        fs::path s = from / rel, d = to / rel;
        fs::create_directories(d.parent_path(), ec);
        uint64_t size = fs::file_size(s, ec);
        // unchanged big files (the models) aren't copied again
        if (!(fs::exists(d, ec) && fs::file_size(d, ec) == size && size > (16u << 20) &&
              fs::last_write_time(d, ec) == fs::last_write_time(s, ec))) {
            if (!fs::copy_file(s, d, fs::copy_options::overwrite_existing, ec))
                return "Copying " + u8(rel) + " failed: " + ec.message() + (ec.value() == ERROR_SHARING_VIOLATION ? " (is rekordbox running?)" : "");
        }
        done += size;
        double p = 0.02 + 0.7 * (total ? (double)done / total : 1);
        if (p - last > 0.02) step("", p), last = p;
    }
    if (!t.table.empty()) {  // the table made for this rekordbox (find())
        step("Writing the address table made for this rekordbox version", 0.72);
        if (!writeFile(to / L"rbmods" / wide("rekordbox-" + t.build + ".tbl"), t.table)) return "The address table couldn't be written to " + t.dest;
    }
    // the patcher too, as the uninstaller
    fs::copy_file(selfPath(), to / L"platendoos patcher.exe", fs::copy_options::overwrite_existing, ec);
    step("  " + std::to_string(files.size()) + " files, " + humanSize((double)total), 0.72);

    step("Giving it its own icon (rekordbox's, inverted)", 0.73);
    fs::path icon = to / L"platendoos.ico";
    if (!invertedIcon(fs::path(wide(t.app)), icon)) {
        step("  kept rekordbox's icon", 0.74);
        icon = fs::path(wide(t.app));
    }

    if (testDest()) {
        step("Test install (PLATENDOOS_DEST): no shortcuts or Apps & features entry", 0.75);
    } else {
        step("Adding platendoos to the Start menu", 0.75);
        if (!makeShortcut(startShortcut(), to / L"platendoos.exe", icon, L"rekordbox with the platendoos mods"))
            step("  the Start menu shortcut couldn't be made", 0.76);
        if (o.desktop && !makeShortcut(desktopShortcut(), to / L"platendoos.exe", icon, L"rekordbox with the platendoos mods"))
            step("  the desktop shortcut couldn't be made", 0.77);
        if (!o.desktop) fs::remove(desktopShortcut(), ec);

        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            setString(k, L"DisplayName", L"platendoos");
            setString(k, L"DisplayVersion", wide(PLATENDOOS_BUILD));
            setString(k, L"Publisher", L"platendoos");
            setString(k, L"InstallLocation", to.wstring());
            setString(k, L"DisplayIcon", icon.wstring());
            setString(k, L"UninstallString", quote((to / L"platendoos patcher.exe").wstring()));
            DWORD one = 1;
            RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof one);
            RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof one);
            RegCloseKey(k);
        }
    }

    fs::path csv = fs::path(wide(t.app)).parent_path() / L"MidiMappings" / L"DDJ-FLX4.midi.csv", orig = csv;
    orig += L".original";
    if (!o.flx4) {  // unticked: an extended mapping from an earlier install goes (it is in rekordbox's own folder)
        if (fs::exists(orig, ec)) {
            step("Putting rekordbox's own DDJ-FLX4 mapping back: Windows asks for permission", 0.8);
            int r = runElevated(L"--restore-mapping " + quote(csv.wstring()));
            if (r == 0) step("  done", 0.9);
            else step(r < 0 ? "  not restored: no permission (Install again to retry)" : "  not restored (" + std::to_string(r) + ")", 0.9);
        }
    } else {
        std::string cur, why;
        if (!readFile(csv, &cur)) {
            step("  no DDJ-FLX4 mapping in rekordbox: skipped", 0.85);
        } else {
            std::string base;
            std::string want = flx4map::make(readFile(orig, &base) ? base : cur, &why);
            if (want.empty()) step("  FLX4 mapping skipped: " + why, 0.85);
            else if (want == cur) step("The DDJ-FLX4 mapping is already the extended one", 0.85);
            else {
                step("Adding the DDJ-FLX4 mapping (four decks, stems): Windows asks for permission", 0.8);
                int r = runElevated(L"--write-mapping " + quote(csv.wstring()));
                if (r == 0) step("  done (rekordbox's own is kept as DDJ-FLX4.midi.csv.original)", 0.9);
                else step(r < 0 ? "  skipped: no permission (Install again to retry)" : "  the mapping couldn't be written (" + std::to_string(r) + ")", 0.9);
            }
        }
    }
    step("Done: start platendoos from the Start menu", 1);
    return "";
}

std::string uninstall(const Target& t, const Step& step) {
    std::error_code ec;
    if (!rekordboxRunning().empty()) return "rekordbox is running: close it first.";
    fs::path to = dest();
    if (!t.app.empty()) {
        fs::path csv = fs::path(wide(t.app)).parent_path() / L"MidiMappings" / L"DDJ-FLX4.midi.csv", orig = csv;
        orig += L".original";
        if (fs::exists(orig, ec)) {
            step("Putting rekordbox's own DDJ-FLX4 mapping back: Windows asks for permission", 0.2);
            if (runElevated(L"--restore-mapping " + quote(csv.wstring())) != 0) step("  not restored (no permission)", 0.3);
        }
    }
    if (!testDest()) {
        step("Removing the shortcuts", 0.4);
        fs::remove(startShortcut(), ec);
        fs::remove(desktopShortcut(), ec);
        RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    }
    step("Removing " + t.dest, 0.6);
    // Everything in the folder goes: only if it is a platendoos folder (PLATENDOOS_DEST may name anything).
    if (!fs::exists(to / L"platendoos.exe", ec) || !fs::exists(to / L"rbmods.dll", ec))
        return t.dest + " is not a platendoos folder (no platendoos.exe and rbmods.dll in it): nothing removed.";
    bool self = fs::equivalent(payload(), to, ec);
    for (auto& e : fs::directory_iterator(to, ec)) {
        if (self && fs::equivalent(e.path(), selfPath(), ec)) continue;
        fs::remove_all(e.path(), ec);
        if (ec) return "Removing " + u8(e.path()) + " failed: " + ec.message();
    }
    if (self) {  // the rest goes once the patcher has quit (removeOnExit)
        g_removeOnExit = to;
    } else {
        fs::remove(to, ec);
    }
    step("Removed. Your rekordbox library, settings and the mods' caches are untouched.", 1);
    return "";
}

void launch(const Target& t) {
    std::wstring exe = (fs::path(wide(t.dest)) / L"platendoos.exe").wstring();
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, fs::path(exe).parent_path().c_str(), SW_SHOWNORMAL);
}

void reveal(const std::string& path) {
    std::wstring p = fs::path(wide(path)).make_preferred().wstring();
    ShellExecuteW(nullptr, L"open", L"explorer.exe", p.c_str(), nullptr, SW_SHOWNORMAL);
}

}  // namespace patcher

// ---- the app ----------------------------------------------------------------------------------------------

namespace {

LRESULT CALLBACK msgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_RUN) {
        auto* f = (std::function<void()>*)lp;
        (*f)();
        delete f;
        return 0;
    }
    if (m == WM_TIMER) {
        if (!patcher::windowOpen() && !patcher::busy()) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// WebView2Loader.dll comes with rekordbox; the WebView2 runtime with Windows 11 (and most Windows 10).
bool webViewAvailable(std::wstring* why) {
    fs::path rb = findRekordbox();
    if (!rb.empty()) SetDllDirectoryW(rb.parent_path().c_str());
    HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    if (!loader) return *why = L"WebView2Loader.dll wasn't found (it comes with rekordbox: install rekordbox first).", false;
    using GetVersion = HRESULT(STDAPICALLTYPE*)(PCWSTR, LPWSTR*);
    auto get = (GetVersion)GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString");
    LPWSTR v = nullptr;
    if (!get || FAILED(get(nullptr, &v)) || !v)
        return *why = L"The Microsoft Edge WebView2 Runtime isn't installed. Get it from developer.microsoft.com/microsoft-edge/webview2 and start the patcher again.", false;
    CoTaskMemFree(v);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && !wcscmp(argv[1], L"--write-mapping")) return patcher::writeMapping(argv[2]);
    if (argc >= 3 && !wcscmp(argv[1], L"--restore-mapping")) return patcher::restoreMapping(argv[2]);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc{};
    wc.lpfnWndProc = msgProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"PlatendoosPatcherMsg";
    RegisterClassW(&wc);
    g_msgWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);

    if (argc >= 2 && !wcsncmp(argv[1], L"--", 2)) {  // the command line mode: output to the console it was started from
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        bool piped = out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
        if (!piped && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        int status = patcher::cli(narrow(argv[1]));
        removeOnExit();
        return status;
    }

    std::wstring why;
    if (!webViewAvailable(&why)) {
        MessageBoxW(nullptr, why.c_str(), L"platendoos patcher", MB_ICONERROR);
        return 1;
    }
    patcher::start();
    SetTimer(g_msgWnd, 1, 500, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    removeOnExit();
    return 0;
}
