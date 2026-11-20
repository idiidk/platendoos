// badge's chip on Windows: a child window of rekordbox's main window, drawn with GDI+ like the mac chip
// (badge_chip.h), with a standard tooltip. JUCE's window clips its children, so the chip stays on top of it.
#include "badge_view.h"
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <propsys.h>
#include <initguid.h>  // defines the PKEY_ below here
#include <propkey.h>
#include "badge_chip_png.h"
#include <string>
#include "rbmod.h"

namespace {

void (*g_onClick)();
HWND g_chip, g_tip, g_host;
std::wstring g_tipText;
bool g_pending, g_pulsing;  // pulsing: the chip's timer runs (the tile pulses orange while a restart is pending)
ULONG_PTR g_gdiplus;

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// rekordbox's main window: its largest visible top-level window titled "rekordbox".
HWND mainWindow() {
    struct Find { HWND best; LONG area; } f{nullptr, 0};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            DWORD pid;
            GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
            wchar_t t[32];
            GetWindowTextW(h, t, 32);
            if (wcscmp(t, L"rekordbox")) return TRUE;
            RECT r;
            GetWindowRect(h, &r);
            Find* f = (Find*)lp;
            LONG a = (r.right - r.left) * (r.bottom - r.top);
            if (a > f->area) f->best = h, f->area = a;
            return TRUE;
        },
        (LPARAM)&f);
    return f.best;
}

// The chip: badge_chip.h's drawing, rendered on the Mac at Windows' display scales by the build (tools/win/build.sh,
// badge_chip_png.h), so it looks the same on both. The rendering for the window's height (the next larger one,
// scaled down), on rekordbox's black bar behind the rounded corners.
Gdiplus::Bitmap* chipBitmap(int h) {
    static Gdiplus::Bitmap* decoded[sizeof kChipPngs / sizeof *kChipPngs];
    int n = (int)(sizeof kChipPngs / sizeof *kChipPngs), pick = n - 1;
    for (int i = 0; i < n; i++)
        if (kChipPngs[i].height >= h) { pick = i; break; }
    if (!decoded[pick]) {
        IStream* st = SHCreateMemStream(kChipPngs[pick].bytes, kChipPngs[pick].size);
        if (!st) return nullptr;
        decoded[pick] = Gdiplus::Bitmap::FromStream(st);
        st->Release();
    }
    return decoded[pick];
}

// The pending pulse's orange (0..1) at phase ph (0..1 of 2.4 s), as badge_view_mac.mm's keyframes: blue, a quick
// eased fade to orange, orange, a quick fade back (a slow crossfade would sit in the muddy grey between the two).
double pulse(double ph) {
    auto ease = [](double x) { return x * x * (3 - 2 * x); };
    if (ph < 0.38) return 0;
    if (ph < 0.5) return ease((ph - 0.38) / 0.12);
    if (ph < 0.88) return 1;
    return 1 - ease((ph - 0.88) / 0.12);
}

void paint(HDC dc, int w, int h) {
    using namespace Gdiplus;
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.Clear(Color(255, 0, 0, 0));
    Bitmap* chip = chipBitmap(h);
    if (chip) {
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(chip, Rect(0, 0, w, h));
    }
    if (g_pending && chip) {
        // A restart is pending: the icon's tile pulses orange, as on macOS. The tile is drawn again through a colour
        // matrix that takes the chip's blue (0.16, 0.47, 0.92) to orange (1, 0.58, 0.1) and keeps white and grey
        // (each output's weights sum to 1; green's weight is 0), faded in by the pulse.
        REAL a = (REAL)pulse((GetTickCount64() % 2400) / 2400.0);
        ColorMatrix cm = {{{-0.1053f, 0.4474f, 1.0789f, 0, 0},
                           {0, 0, 0, 0, 0},
                           {1.1053f, 0.5526f, -0.0789f, 0, 0},
                           {0, 0, 0, a, 0},
                           {0, 0, 0, 0, 1}}};
        ImageAttributes attrs;
        attrs.SetColorMatrix(&cm);
        INT ch = (INT)chip->GetHeight();
        g.DrawImage(chip, Rect(0, 0, h, h), 0, 0, ch, ch, UnitPixel, &attrs);
    }
    Graphics out(dc);
    out.DrawImage(&bmp, 0, 0);
}

LRESULT CALLBACK chipProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            paint(dc, r.right, r.bottom);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;  // the pending pulse
        case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE;
        case WM_LBUTTONUP:
            if (g_onClick) g_onClick();
            return 0;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // a click doesn't take the focus from rekordbox
    }
    return DefWindowProcW(h, m, wp, lp);
}

void create(HWND host) {
    static bool registered;
    if (!registered) {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&g_gdiplus, &in, nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = chipProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"platendoos.badge";
        RegisterClassW(&wc);
        registered = true;
    }
    if (g_chip) DestroyWindow(g_chip), g_chip = nullptr;  // (its tooltip and timer go with it)
    g_pulsing = false;
    g_chip = CreateWindowExW(0, L"platendoos.badge", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 90, 16, host, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                            g_chip, nullptr, GetModuleHandleW(nullptr), nullptr);
    TTTOOLINFOW ti{sizeof ti};
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_chip;
    ti.uId = (UINT_PTR)g_chip;
    ti.lpszText = (LPWSTR)g_tipText.c_str();
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);  // (the tip has line breaks, which a tooltip honours only with a width)
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    g_host = host;
}

}  // namespace

namespace badgeview {

// rekordbox's window gets platendoos's icon, rekordbox's own inverted (as on macOS), which the patcher made from
// the installed rekordbox.exe next to platendoos.exe (patcher/install_win.cpp): in the taskbar and Alt+Tab it
// tells the modded rekordbox from the plain one. Without the file rekordbox keeps its own.
//
// Setting it from outside (WM_SETICON, the class icon) didn't stick: rekordbox's window code answers or re-sets
// the icon itself. So its window procedure is wrapped: asked for its icon it answers ours, and an icon rekordbox
// sets is swapped for ours on the way in.
namespace {
HICON g_iconBig, g_iconSmall;
WNDPROC g_hostProc;
HWND g_iconHost;
std::wstring g_dir;  // platendoos's folder (rbmods.dll's)

LRESULT CALLBACK iconProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_GETICON && g_iconBig) return (LRESULT)(wp == ICON_BIG ? g_iconBig : g_iconSmall);
    if (m == WM_SETICON && g_iconBig) lp = (LPARAM)(wp == ICON_BIG ? g_iconBig : g_iconSmall);
    return CallWindowProcW(g_hostProc, h, m, wp, lp);
}

// The taskbar button doesn't take the window's icon: Windows 11 groups windows by app (AppUserModelID, by default
// the exe's) and draws the app's icon. So the window gets its own app id, with platendoos as the program to relaunch
// and its icon; the patcher's Start menu shortcut carries the same id (patcher/install_win.cpp kAppId).
bool setString(IPropertyStore* ps, const PROPERTYKEY& k, const std::wstring& v) {
    PROPVARIANT pv{};
    pv.vt = VT_LPWSTR;
    pv.pwszVal = (LPWSTR)CoTaskMemAlloc((v.size() + 1) * sizeof(wchar_t));
    if (!pv.pwszVal) return false;
    memcpy(pv.pwszVal, v.c_str(), (v.size() + 1) * sizeof(wchar_t));
    bool ok = SUCCEEDED(ps->SetValue(k, pv));
    PropVariantClear(&pv);
    return ok;
}

bool setAppId(HWND h, const std::wstring& dir) {
    IPropertyStore* ps = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) return false;
    // relaunch info first: the shell reads it when the id is set
    bool ok = setString(ps, PKEY_AppUserModel_RelaunchCommand, L"\"" + dir + L"platendoos.exe\"") &&
              setString(ps, PKEY_AppUserModel_RelaunchIconResource, dir + L"platendoos.ico,0") &&
              setString(ps, PKEY_AppUserModel_RelaunchDisplayNameResource, L"platendoos") &&
              setString(ps, PKEY_AppUserModel_ID, L"platendoos.rekordbox");
    ps->Release();
    return ok;
}

void describe(HWND h, const char* what) {
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    rbm::logf("badge: %s window %p class %ls caption %d appwindow %d owner %p", what, (void*)h, cls, (st & WS_CAPTION) == WS_CAPTION,
              (ex & WS_EX_APPWINDOW) != 0, (void*)GetWindow(h, GW_OWNER));
}
}  // namespace

void setAppIcon(HWND host) {
    static bool tried;
    if (!tried) {
        tried = true;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&setAppIcon, &self);
        wchar_t path[MAX_PATH * 4];
        DWORD n = GetModuleFileNameW(self, path, MAX_PATH * 4);
        std::wstring dir(path, n);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        g_dir = dir;
        std::wstring ico = dir + L"platendoos.ico";
        UINT dpi = GetDpiForWindow(host);
        HICON big = (HICON)LoadImageW(nullptr, ico.c_str(), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CXICON, dpi), LR_LOADFROMFILE);
        HICON sm = (HICON)LoadImageW(nullptr, ico.c_str(), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CXSMICON, dpi), LR_LOADFROMFILE);
        rbm::logf("badge: window icon %ls: %s", ico.c_str(), big && sm ? "loaded" : "not loaded");
        if (big && sm) g_iconBig = big, g_iconSmall = sm;
        HWND root = GetAncestor(host, GA_ROOTOWNER);
        describe(host, "main");
        if (root != host) describe(root, "root owner");
    }
    if (!g_iconBig || host == g_iconHost) return;
    // A new main window (or the first): wrap it, then hand it the icon so the taskbar and title bar redraw.
    if (g_iconHost && IsWindow(g_iconHost) && g_hostProc) SetWindowLongPtrW(g_iconHost, GWLP_WNDPROC, (LONG_PTR)g_hostProc);
    g_iconHost = host;
    g_hostProc = (WNDPROC)SetWindowLongPtrW(host, GWLP_WNDPROC, (LONG_PTR)iconProc);
    SetClassLongPtrW(host, GCLP_HICON, (LONG_PTR)g_iconBig);
    SetClassLongPtrW(host, GCLP_HICONSM, (LONG_PTR)g_iconSmall);
    DefWindowProcW(host, WM_SETICON, ICON_BIG, (LPARAM)g_iconBig);
    DefWindowProcW(host, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);
    // Also the root owner: when rekordbox's window is owned, the taskbar button is the owner's.
    HWND root = GetAncestor(host, GA_ROOTOWNER);
    if (root && root != host) {
        SendMessageW(root, WM_SETICON, ICON_BIG, (LPARAM)g_iconBig);
        SendMessageW(root, WM_SETICON, ICON_SMALL, (LPARAM)g_iconSmall);
    }
    rbm::logf("badge: taskbar app id %s", setAppId(host, g_dir) ? "set" : "not set");
    rbm::logf("badge: window icon set, the window reports %s", (HICON)SendMessageW(host, WM_GETICON, ICON_BIG, 0) == g_iconBig ? "ours" : "another");
}

void setOnClick(void (*fn)()) { g_onClick = fn; }

void update(bool on, bool hidden, const Bar* bar, const std::string& tip, bool pending) {
    if (!on || !bar) {
        if (g_chip) ShowWindow(g_chip, SW_HIDE);
        return;
    }
    HWND host = mainWindow();
    if (!host) return;
    std::wstring t = wide(tip);
    setAppIcon(host);
    if (!g_chip || g_host != host || !IsWindow(g_chip)) g_tipText = t, create(host);
    else if (t != g_tipText) {
        g_tipText = t;
        TTTOOLINFOW ti{sizeof ti};
        ti.hwnd = g_chip;
        ti.uId = (UINT_PTR)g_chip;
        ti.lpszText = (LPWSTR)g_tipText.c_str();
        SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
    // JUCE's logical pixels -> the window's: its DPI scale. Right of the "rekordbox" logo (it ends ~112 px in), on
    // the bar's centre line, 90 x 16.
    double k = GetDpiForWindow(host) / 96.0;
    POINT p{(LONG)((bar->x + 120) * k), (LONG)((bar->y + (bar->h - 16) / 2.0) * k)};
    ScreenToClient(host, &p);
    int w = (int)(90 * k + 0.5), h = (int)(16 * k + 0.5);
    RECT cur;
    GetWindowRect(g_chip, &cur);
    POINT tl{cur.left, cur.top};
    ScreenToClient(host, &tl);
    if (tl.x != p.x || tl.y != p.y || cur.right - cur.left != w || cur.bottom - cur.top != h)
        SetWindowPos(g_chip, HWND_TOP, p.x, p.y, w, h, SWP_NOACTIVATE);
    if (pending != g_pending) g_pending = pending, InvalidateRect(g_chip, nullptr, FALSE);
    if (g_pending != g_pulsing) {  // ~25 redraws a second of the 90 x 16 chip while pending, none otherwise
        if (g_pending) SetTimer(g_chip, 1, 40, nullptr);
        else KillTimer(g_chip, 1);
        g_pulsing = g_pending;
    }
    ShowWindow(g_chip, hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
}

}  // namespace badgeview
