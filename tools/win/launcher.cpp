// platendoos.exe: starts rekordbox with the mods. rekordbox's own files are never changed: rekordbox.exe is
// started suspended and loads rbmods.dll (next to this launcher) on its main thread before any of its own code
// runs (an APC, which runs once the loader has set the process up). Arguments are passed on to rekordbox.
//   platendoos.exe [--rekordbox <path to rekordbox.exe>] [rekordbox's arguments]
// Without --rekordbox: the newest C:\Program Files\rekordbox\rekordbox *\rekordbox.exe.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")

namespace fs = std::filesystem;

static void fail(const std::wstring& what) {
    MessageBoxW(nullptr, what.c_str(), L"platendoos", MB_ICONERROR);
    ExitProcess(1);
}

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

static fs::path findRekordbox() {
    fs::path best;
    std::error_code ec;
    for (const wchar_t* root : {L"C:\\Program Files\\rekordbox", L"C:\\Program Files\\Pioneer\\rekordbox"}) {
        for (auto& d : fs::directory_iterator(root, ec)) {
            fs::path exe = d.path() / L"rekordbox.exe";
            if (fs::exists(exe, ec) && (best.empty() || newerName(d.path().filename().wstring(), best.parent_path().filename().wstring()))) best = exe;
        }
    }
    return best;
}

// One argument in the form CommandLineToArgvW (and the C runtime) takes apart again: backslashes in front of a quote,
// and at the end, are doubled ("C:\My Music\" would otherwise swallow its closing quote).
static std::wstring quote(const std::wstring& a) {
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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
    fs::path here = fs::path(self).parent_path();
    fs::path dll = here / L"rbmods.dll";
    fs::path exe;
    std::wstring args;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--rekordbox") && i + 1 < argc) exe = argv[++i];
        else args += L" " + quote(argv[i]);
    }
    if (exe.empty()) exe = findRekordbox();
    if (exe.empty()) fail(L"rekordbox wasn't found in C:\\Program Files\\rekordbox. Start platendoos.exe with --rekordbox <path to rekordbox.exe>.");
    if (!fs::exists(dll)) fail(L"rbmods.dll is missing next to platendoos.exe.");

    std::wstring cmd = quote(exe.wstring()) + args;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring dir = exe.parent_path().wstring();
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi))
        fail(L"rekordbox couldn't be started (error " + std::to_wstring(GetLastError()) + L").");

    std::wstring path = dll.wstring();
    size_t bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    auto loadLibrary = (PAPCFUNC)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!remote || !WriteProcessMemory(pi.hProcess, remote, path.c_str(), bytes, nullptr) ||
        !QueueUserAPC(loadLibrary, pi.hThread, (ULONG_PTR)remote)) {
        TerminateProcess(pi.hProcess, 1);
        fail(L"The mods couldn't be loaded into rekordbox (error " + std::to_wstring(GetLastError()) + L").");
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
