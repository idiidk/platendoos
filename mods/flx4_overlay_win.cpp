// flx4's toasts and mixer marks on Windows (see flx4_overlay.h), drawn as on the mac (flx4_overlay_mac.mm).
//
// Each is a layered popup window owned by rekordbox's main window: above it (and hidden with it when minimised),
// click-through (WS_EX_TRANSPARENT), never activated, drawn with GDI+ into a premultiplied DIB (per-pixel alpha).
// The rectangles flx4.cpp gives are juce screen coordinates: logical pixels, the window's DPI scale times smaller.
#include "flx4_overlay.h"
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "rbmod.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

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

double scaleOf(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    return dpi ? dpi / 96.0 : 1.0;
}

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

LRESULT CALLBACK overlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, wp, lp);
}

// A layered window and the picture it shows.
struct Layer {
    HWND wnd = nullptr, owner = nullptr;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0, x = 0, y = 0;
    BYTE alpha = 255;

    bool ensure(HWND host) {
        static bool registered;
        if (!registered) {
            ULONG_PTR token;
            Gdiplus::GdiplusStartupInput in;
            Gdiplus::GdiplusStartup(&token, &in, nullptr);
            WNDCLASSW wc{};
            wc.lpfnWndProc = overlayProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"platendoos.flx4";
            RegisterClassW(&wc);
            registered = true;
        }
        if (wnd && (owner != host || !IsWindow(wnd))) destroy();
        if (!wnd) {
            wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"platendoos.flx4", L"",
                                  WS_POPUP, 0, 0, 1, 1, host, nullptr, GetModuleHandleW(nullptr), nullptr);
            owner = host;
        }
        return wnd != nullptr;
    }
    void destroy() {
        if (wnd && IsWindow(wnd)) DestroyWindow(wnd);
        wnd = nullptr;
        freeBitmap();
    }
    void freeBitmap() {
        if (dc) SelectObject(dc, old), DeleteDC(dc);
        if (bmp) DeleteObject(bmp);
        dc = nullptr, bmp = nullptr, bits = nullptr, w = h = 0;
    }
    // A cleared picture of w x h pixels to draw on (premultiplied ARGB, as UpdateLayeredWindow takes it).
    Gdiplus::Bitmap* canvas(int width, int height) {
        if (width != w || height != h) {
            freeBitmap();
            BITMAPINFO bi{};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
            bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp) return nullptr;
            dc = CreateCompatibleDC(nullptr);
            old = (HBITMAP)SelectObject(dc, bmp);
            w = width, h = height;
        }
        memset(bits, 0, (size_t)w * h * 4);
        return new Gdiplus::Bitmap(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
    }
    void show(int px, int py) {
        x = px, y = py;
        POINT dst{x, y}, src{0, 0};
        SIZE size{w, h};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        UpdateLayeredWindow(wnd, nullptr, &dst, &size, dc, &src, 0, &blend, ULW_ALPHA);
        if (!IsWindowVisible(wnd)) ShowWindow(wnd, SW_SHOWNOACTIVATE);
    }
    void setAlpha(BYTE a) {
        alpha = a;
        if (wnd && dc) show(x, y);
    }
    void hide() {
        if (wnd && IsWindowVisible(wnd)) ShowWindow(wnd, SW_HIDE);
    }
};

Gdiplus::Color rgb(double r, double g, double b, double a = 1) {
    return Gdiplus::Color((BYTE)lround(a * 255), (BYTE)lround(r * 255), (BYTE)lround(g * 255), (BYTE)lround(b * 255));
}

void roundedRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = 2 * r;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

Layer g_toast[2];
int g_toastShown[2];  // counts showings, so a toast shown again isn't faded out by the one before

void fade(int side, int shown, int step) {
    if (g_toastShown[side] != shown) return;
    constexpr int kSteps = 10;  // over 0.35 s
    if (step >= kSteps) return g_toast[side].hide();
    g_toast[side].setAlpha((BYTE)(255 * (kSteps - 1 - step) / kSteps));
    rbm::plat::onMainAfter(0.035, [=] { fade(side, shown, step + 1); });
}

flx4ui::MixerMarks g_marks;  // as drawn
Layer g_mixer;
std::vector<int> g_drawn;    // what the overlay shows, to redraw only when it changes

// The marks, in juce coordinates (the graphics is scaled and moved to the window).
void drawMarks(Gdiplus::Graphics& g) {
    using namespace Gdiplus;
    using flx4ui::Rect;
    Color blue = rgb(0.16, 0.47, 0.92), orange = rgb(1, 0.55, 0.1);
    // a knob's angle in degrees for GDI+ (clockwise from 3 o'clock): 7 o'clock at 0, 5 o'clock at 127
    auto angle = [](double v) { return (float)(-135 + 270 * v / 127.0 - 90); };
    FontFamily family(L"Segoe UI");
    for (int d = 0; d < 4; d++) {  // CFX knobs, as rekordbox draws its mixer's knobs
        Rect r = g_marks.cfxKnob[d];
        int val = g_marks.cfxValue[d];
        if (r.w <= 0 || val < 0) continue;
        float cx = r.x + r.w / 2.f, cy = r.y + r.h / 2.f, rad = std::min(r.w, r.h) / 2.f - 2.5f;
        RectF box(cx - rad, cy - rad, 2 * rad, 2 * rad);
        Pen track(rgb(0.23, 0.23, 0.23), 2);
        g.DrawArc(&track, box, angle(0), 270);
        if (abs(val - 64) > 1) {  // from the centre: the effect's amount
            Pen arc(rgb(0.2, 0.45, 0.95), 2);
            g.DrawArc(&arc, box, angle(64), angle(val) - angle(64));
        }
        double a = angle(val) * kPi / 180;
        Pen pointer(Color(255, 255, 255, 255), 1.8f);
        pointer.SetStartCap(LineCapRound), pointer.SetEndCap(LineCapRound);
        g.DrawLine(&pointer, (float)(cx + cos(a) * rad * 0.55), (float)(cy + sin(a) * rad * 0.55), (float)(cx + cos(a) * (rad - 0.5)),
                   (float)(cy + sin(a) * (rad - 0.5)));
        Rect lr = g_marks.cfxLabel[d];
        Font font(&family, 8.5f, FontStyleBold, UnitPixel);
        StringFormat center;
        center.SetAlignment(StringAlignmentCenter);
        center.SetLineAlignment(StringAlignmentCenter);
        SolidBrush white(Color(255, 255, 255, 255));
        g.DrawString(L"CFX", -1, &font, RectF((float)lr.x, lr.y - 1.5f, (float)lr.w, lr.h + 3.f), &center, &white);
    }
    for (int side = 0; side < 2; side++) {
        Rect col = g_marks.column[side];
        if (col.w > 0) {
            GraphicsPath p;
            roundedRect(p, col.x - 3.f, col.y - 3.f, col.w + 6.f, col.h + 6.f, 5);
            Pen pen(blue, 1.5f);
            g.DrawPath(&pen, &p);
        }
        // where the FLX4's knobs and fader are, for the controls still to pick up
        for (int c = 0; c < flx4ui::kControls; c++) {
            int phys = g_marks.phys[side][c];
            if (phys < 0) continue;
            Rect r = g_marks.control[side][c];
            SolidBrush fill(orange);
            if (c == flx4ui::kFader) {
                float y = r.y + r.h - 4 - (r.h - 8) * phys / 127.f;
                g.FillRectangle(&fill, r.x - 3.f, y - 1, r.w + 6.f, 2.f);
            } else {  // a rotary knob: 7 to 5 o'clock
                double a = (-135 + 270 * phys / 127.0) * kPi / 180, rad = std::min(r.w, r.h) / 2.0;
                double dx = sin(a), dy = -cos(a);
                float cx = r.x + r.w / 2.f, cy = r.y + r.h / 2.f;
                Pen pen(orange, 2);
                pen.SetStartCap(LineCapRound), pen.SetEndCap(LineCapRound);
                g.DrawLine(&pen, (float)(cx + dx * rad * 0.3), (float)(cy + dy * rad * 0.3), (float)(cx + dx * rad), (float)(cy + dy * rad));
                g.FillEllipse(&fill, (float)(cx + dx * rad - 2.5), (float)(cy + dy * rad - 2.5), 5.f, 5.f);
            }
        }
    }
}

}  // namespace

namespace flx4ui {

bool windowAvailable() { return mainWindow() != nullptr; }

static flx4ui::Rect g_decks[4];  // where the decks are (setMixerMarks), for the toasts

void toast(int side, const std::string& titleText, const std::string& detailText, int deck) {
    using namespace Gdiplus;
    HWND host = mainWindow();
    if (!host || IsIconic(host) || side < 0 || side > 1) return;
    Layer& t = g_toast[side];
    if (!t.ensure(host)) return;
    double s = scaleOf(host);
    float lh = detailText.empty() ? 56.f : detailText.find('\n') != std::string::npos ? 100.f : 84.f;
    int w = (int)lround(300 * s), h = (int)lround(lh * s);
    Bitmap* bmp = t.canvas(w, h);
    if (!bmp) return;
    {
        Graphics g(bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.ScaleTransform((REAL)s, (REAL)s);
        float lw = 300;
        GraphicsPath box;
        roundedRect(box, 0, 0, lw, lh, 8);
        SolidBrush bg(rgb(0.08, 0.08, 0.08, 0.92));
        g.FillPath(&bg, &box);
        GraphicsPath edge;
        roundedRect(edge, 1, 1, lw - 2, lh - 2, 7);
        Pen pen(rgb(0.16, 0.47, 0.92), 2);
        g.DrawPath(&pen, &edge);
        FontFamily family(L"Segoe UI");
        StringFormat center;
        center.SetAlignment(StringAlignmentCenter);
        Font big(&family, 24, FontStyleBold, UnitPixel), little(&family, 12, FontStyleRegular, UnitPixel);
        SolidBrush white(Color(255, 255, 255, 255)), grey(rgb(0.75, 0.75, 0.75));
        std::wstring title = wide(titleText), detail = wide(detailText);
        g.DrawString(title.c_str(), -1, &big, RectF(8, 10, lw - 16, 34), &center, &white);
        if (!detail.empty()) g.DrawString(detail.c_str(), -1, &little, RectF(8, 46, lw - 16, lh - 52), &center, &grey);
    }
    delete bmp;
    // Over the side's half of the decks (above rekordbox's browser, which takes about the lower 40%).
    RECT cr;
    GetClientRect(host, &cr);
    POINT o{0, 0};
    ClientToScreen(host, &o);
    int x = o.x + (int)lround(cr.right * (side ? 0.75 : 0.25)) - w / 2;
    int y = o.y + (int)lround(cr.bottom * 0.6 * 0.6) - h / 2;
    if (deck >= 0 && deck < 4 && g_decks[deck].w > 0) {  // over that deck (juce's coordinates are logical: times the scale)
        flx4ui::Rect d = g_decks[deck];
        x = (int)lround((d.x + d.w / 2.0) * s) - w / 2, y = (int)lround((d.y + d.h / 2.0) * s) - h / 2;
    }
    t.alpha = 255;
    t.show(x, y);
    int shown = ++g_toastShown[side];
    rbm::plat::onMainAfter(1.4, [side, shown] { fade(side, shown, 0); });
}

void setMixerMarks(const MixerMarks& m) {
    for (int d = 0; d < 4; d++) g_decks[d] = m.deck[d];
    HWND host = mainWindow();
    bool on = host && m.shown && !IsIconic(host);
    std::vector<int> drawn{on};
    Rect area{};
    auto unite = [&](Rect r) {
        if (r.w <= 0 || r.h <= 0) return;
        if (area.w <= 0) return void(area = r);
        int x = std::min(area.x, r.x), y = std::min(area.y, r.y);
        area = {x, y, std::max(area.x + area.w, r.x + r.w) - x, std::max(area.y + area.h, r.y + r.h) - y};
    };
    for (int side = 0; side < 2 && on; side++) {
        Rect col = m.column[side];
        drawn.insert(drawn.end(), {side, col.x, col.y, col.w, col.h});
        unite(col);
        for (int c = 0; c < kControls; c++) {
            Rect cr = m.control[side][c];
            drawn.insert(drawn.end(), {m.phys[side][c], cr.x, cr.y, cr.w, cr.h});
            unite(cr);
        }
    }
    for (int d = 0; d < 4 && on; d++) {
        Rect k = m.cfxKnob[d], l = m.cfxLabel[d];
        drawn.insert(drawn.end(), {m.cfxValue[d], k.x, k.y, k.w, k.h, l.x, l.y, l.w, l.h});
        unite(k), unite(l);
    }
    if (!on || area.w <= 0) {
        g_mixer.hide();
        g_drawn.clear();
        return;
    }
    if (!g_mixer.ensure(host)) return;
    double s = scaleOf(host);
    drawn.push_back((int)lround(s * 100));
    if (drawn == g_drawn && IsWindowVisible(g_mixer.wnd)) return;
    g_drawn = drawn;
    g_marks = m;
    const int margin = 12;
    int x0 = area.x - margin, y0 = area.y - margin;
    int w = (int)lround((area.w + 2 * margin) * s), h = (int)lround((area.h + 2 * margin) * s);
    Gdiplus::Bitmap* bmp = g_mixer.canvas(w, h);
    if (!bmp) return;
    {
        Gdiplus::Graphics g(bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.ScaleTransform((Gdiplus::REAL)s, (Gdiplus::REAL)s);
        g.TranslateTransform((Gdiplus::REAL)-x0, (Gdiplus::REAL)-y0);
        drawMarks(g);
    }
    delete bmp;
    g_mixer.alpha = 255;
    g_mixer.show((int)lround(x0 * s), (int)lround(y0 * s));
}

}  // namespace flx4ui
