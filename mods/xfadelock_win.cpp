// xfadelock's button on Windows: a child window of rekordbox's main window drawn with GDI+ like the mac view
// (xfadelock_mac.mm), with a standard tooltip. JUCE's window clips its children, so the button stays on top of it.
// mods/xfadelock.cpp puts it next to the crossfader and says what a click does.
#include "xfadelock_view.h"
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cmath>
#include <string>

using namespace xfadelockview;

namespace {

int (*g_onClick)();
HWND g_button, g_tip, g_host;
std::wstring g_tipText;
int g_state = -1;
ULONG_PTR g_gdiplus;

std::wstring wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// rekordbox's main window: its largest visible top-level window titled "rekordbox" (as badge_view_win.cpp).
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

void roundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = 2 * r;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

// The mac view's drawing (in its points, here JUCE's logical pixels, scaled to the window's): lit in the blue of
// rekordbox's lit assign buttons when locked; else dark grey like the unlit ones, with an open lock (orange while
// the controller's crossfader has yet to pick up).
void paint(HDC dc, int pw, int ph) {
    using namespace Gdiplus;
    double k = g_host ? GetDpiForWindow(g_host) / 96.0 : 1.0;
    Bitmap bmp(pw, ph, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.Clear(Color(255, 30, 30, 30));  // (outside the window's rounded region: not shown)
    g.ScaleTransform((REAL)k, (REAL)k);
    float w = (float)(pw / k), h = (float)(ph / k);
    bool on = g_state == kLocked;
    {
        GraphicsPath bg;
        roundRect(bg, 0, 0, w, h, 2.5f);
        SolidBrush fill(on ? Color(255, 41, 120, 235) : Color(255, 43, 43, 43));
        g.FillPath(&fill, &bg);
    }
    Color glyph = on ? Color(255, 255, 255, 255) : g_state == kPickup ? Color(255, 255, 140, 26) : Color(255, 184, 184, 184);
    float cx = w / 2, top = h / 2 - 4.6f;  // a 7 x 9.5 lock, centred
    {
        GraphicsPath body;
        roundRect(body, cx - 3.5f, top + 4.5f, 7, 5, 1);
        SolidBrush b(glyph);
        g.FillPath(&b, &body);
    }
    float lift = on ? 0 : 1.3f;  // open: the shackle up, its right leg out of the body
    GraphicsPath shackle;
    shackle.AddLine(cx - 2, top + 4.6f, cx - 2, top + 2.4f - lift);
    shackle.AddBezier(cx - 2, top + 2.4f - lift, cx - 2, top - 0.2f - lift, cx + 2, top - 0.2f - lift, cx + 2, top + 2.4f - lift);
    shackle.AddLine(cx + 2, top + 2.4f - lift, cx + 2, on ? top + 4.6f : top + 3.2f - lift);
    Pen pen(glyph, 1.3f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    g.DrawPath(&pen, &shackle);
    Graphics out(dc);
    out.DrawImage(&bmp, 0, 0);
}

LRESULT CALLBACK buttonProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
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
        case WM_LBUTTONDOWN:  // (as the mac view's mouseDown)
            if (g_onClick) g_state = g_onClick(), InvalidateRect(h, nullptr, FALSE);
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
        wc.lpfnWndProc = buttonProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"platendoos.xfadelock";
        RegisterClassW(&wc);
        registered = true;
    }
    if (g_button) DestroyWindow(g_button), g_button = nullptr;  // (its tooltip goes with it)
    g_button = CreateWindowExW(0, L"platendoos.xfadelock", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 14, 14, host, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                            g_button, nullptr, GetModuleHandleW(nullptr), nullptr);
    TTTOOLINFOW ti{sizeof ti};
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_button;
    ti.uId = (UINT_PTR)g_button;
    ti.lpszText = (LPWSTR)g_tipText.c_str();
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 400);
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    g_host = host;
}

}  // namespace

namespace xfadelockview {

bool hostReady() { return mainWindow() != nullptr; }

void setOnClick(int (*fn)()) { g_onClick = fn; }

void update(const Rect* r, int st, const char* tip) {
    HWND host = r ? mainWindow() : nullptr;
    if (!host) {
        if (g_button) ShowWindow(g_button, SW_HIDE);
        return;
    }
    std::wstring t = wide(tip ? tip : "");
    if (!g_button || g_host != host || !IsWindow(g_button)) g_tipText = t, create(host);
    else if (t != g_tipText) {
        g_tipText = t;
        TTTOOLINFOW ti{sizeof ti};
        ti.hwnd = g_button;
        ti.uId = (UINT_PTR)g_button;
        ti.lpszText = (LPWSTR)g_tipText.c_str();
        SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
    // JUCE's logical pixels -> the window's: its DPI scale.
    double k = GetDpiForWindow(host) / 96.0;
    POINT p{(LONG)lround(r->x * k), (LONG)lround(r->y * k)};
    ScreenToClient(host, &p);
    int w = (int)lround(r->w * k), h = (int)lround(r->h * k);
    RECT cur;
    GetWindowRect(g_button, &cur);
    POINT tl{cur.left, cur.top};
    ScreenToClient(host, &tl);
    if (tl.x != p.x || tl.y != p.y || cur.right - cur.left != w || cur.bottom - cur.top != h) {
        SetWindowPos(g_button, HWND_TOP, p.x, p.y, w, h, SWP_NOACTIVATE);
        int d = (int)lround(5 * k);  // the rounded corners (radius 2.5)
        SetWindowRgn(g_button, CreateRoundRectRgn(0, 0, w + 1, h + 1, d, d), TRUE);
    }
    if (st != g_state) g_state = st, InvalidateRect(g_button, nullptr, FALSE);
    if (!IsWindowVisible(g_button)) ShowWindow(g_button, SW_SHOWNOACTIVATE);
}

}  // namespace xfadelockview
