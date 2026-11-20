// rbm::webui on Windows: a top-level tool window owned by rekordbox's main window, hosting a WebView2 (see webui.h).
// rekordbox ships WebView2Loader.dll next to rekordbox.exe; it is loaded at run time (nothing to link against).
#include "webui.h"
#include "plat.h"
#include "rbmod.h"
#include <windows.h>
#include <objbase.h>
#include <map>
#include <vector>
#include <WebView2.h>

namespace rbm::webui {

struct Window {
    std::string id;
    HWND hwnd = nullptr;
    ICoreWebView2Controller* controller = nullptr;
    ICoreWebView2* web = nullptr;
    std::function<void(const std::string&)> onMessage;
    std::string page;
    bool loaded = false;
    std::vector<std::string> queue;  // posted before the page loaded
    double reloadedAt = -1e9;        // after its renderer went (seconds; the patcher links this file without core, so no rbm::now)
};

namespace {

std::map<std::string, Window*>& windows() {
    static std::map<std::string, Window*> m;
    return m;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// A COM callback object around a lambda.
template <class I, class... Args>
struct Callback final : I {
    std::function<HRESULT(Args...)> fn;
    ULONG refs = 1;
    explicit Callback(std::function<HRESULT(Args...)> f) : fn(std::move(f)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(I)) {
            *out = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... a) override { return fn(a...); }
};

ICoreWebView2Environment* g_env = nullptr;
bool g_envPending = false;
std::vector<Window*> g_waiting;  // windows waiting for the environment

// rekordbox's main window: this process's visible top-level window titled "rekordbox" (the largest, if several).
// Not just the largest or the foreground one: while rekordbox starts or reads a playlist, one of its dialogs
// ("Muziekstukken lezen...") can be both, and a tool window it owns goes when the dialog does. nullptr if the
// main window isn't there (the tool window then has no owner).
HWND findMainWindow() {
    struct Best { DWORD pid; HWND hwnd; long area; } best{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            Best* b = (Best*)lp;
            DWORD p = 0;
            GetWindowThreadProcessId(h, &p);
            if (p != b->pid || !IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER)) return TRUE;
            wchar_t title[32] = L"";
            GetWindowTextW(h, title, 32);
            if (wcscmp(title, L"rekordbox")) return TRUE;
            RECT r;
            GetWindowRect(h, &r);
            long area = (r.right - r.left) * (r.bottom - r.top);
            if (area > b->area) b->area = area, b->hwnd = h;
            return TRUE;
        },
        (LPARAM)&best);
    return best.hwnd;
}

void deliver(Window* w, const std::string& json) {
    if (w->web) w->web->PostWebMessageAsJson(wide(json).c_str());
}

void resize(Window* w) {
    if (!w->controller) return;
    RECT r;
    GetClientRect(w->hwnd, &r);
    w->controller->put_Bounds(r);
}

void createController(Window* w) {
    auto done = new Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(
        [w](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
            if (FAILED(hr) || !c) {
                rbm::logf("webui: no web view for %s (0x%08lx)", w->id.c_str(), (unsigned long)hr);
                return S_OK;
            }
            w->controller = c;
            c->AddRef();
            c->get_CoreWebView2(&w->web);
            ICoreWebView2Controller2* c2 = nullptr;
            if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c2))) && c2) {
                COREWEBVIEW2_COLOR bg{255, 30, 30, 30};
                c2->put_DefaultBackgroundColor(bg);
                c2->Release();
            }
            ICoreWebView2Settings* st = nullptr;
            if (SUCCEEDED(w->web->get_Settings(&st)) && st) {
                st->put_AreDefaultContextMenusEnabled(FALSE);
                st->put_IsStatusBarEnabled(FALSE);
                st->put_AreDevToolsEnabled(FALSE);
                st->Release();
            }
            resize(w);
            c->put_IsVisible(TRUE);

            EventRegistrationToken tok;
            w->web->add_WebMessageReceived(
                new Callback<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
                    [w](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                        LPWSTR s = nullptr;
                        if (SUCCEEDED(a->TryGetWebMessageAsString(&s)) && s) {
                            std::string msg = narrow(s);
                            CoTaskMemFree(s);
                            if (w->onMessage) w->onMessage(msg);
                        }
                        return S_OK;
                    }),
                &tok);
            // The page's renderer went (killed or crashed): the page is blank until it is loaded again. Once it is
            // back it asks for the whole state (rbm.ready()), so what was queued for the old page can go.
            w->web->add_ProcessFailed(
                new Callback<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>(
                    [w](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
                        COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                        a->get_ProcessFailedKind(&kind);
                        bool again = GetTickCount64() / 1000.0 - w->reloadedAt < 10;  // a page that takes it down each time isn't reloaded in a loop
                        rbm::logf("webui: %s.html's web view process failed (kind %d)%s", w->page.c_str(), (int)kind,
                                  kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ? "" : again ? ", not reloaded again" : ", reloading");
                        if (kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED || again) return S_OK;
                        w->loaded = false;
                        w->queue.clear();
                        w->reloadedAt = GetTickCount64() / 1000.0;
                        w->web->Reload();
                        return S_OK;
                    }),
                &tok);
            w->web->add_NavigationCompleted(
                new Callback<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
                    [w](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                        if (w->loaded) return S_OK;
                        w->loaded = true;
                        std::vector<std::string> q;
                        q.swap(w->queue);
                        for (const std::string& j : q) deliver(w, j);
                        return S_OK;
                    }),
                &tok);

            ICoreWebView2_3* w3 = nullptr;
            if (SUCCEEDED(w->web->QueryInterface(IID_PPV_ARGS(&w3))) && w3) {
                std::wstring dir = wide(plat::resourcesDir() + "/ui");
                for (wchar_t& ch : dir) if (ch == L'/') ch = L'\\';
                w3->SetVirtualHostNameToFolderMapping(L"platendoos.ui", dir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                w3->Release();
            } else {
                rbm::logf("webui: WebView2 runtime too old for virtual host mapping");
            }
            w->web->Navigate(wide("https://platendoos.ui/" + w->page + ".html").c_str());
            return S_OK;
        });
    g_env->CreateCoreWebView2Controller(w->hwnd, done);
    done->Release();
}

void withEnvironment(Window* w) {
    if (g_env) return createController(w);
    g_waiting.push_back(w);
    if (g_envPending) return;
    g_envPending = true;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // fails harmlessly if the thread already has a mode
    HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
    using CreateEnv = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                               ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    auto create = loader ? (CreateEnv)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions") : nullptr;
    if (!create) {
        rbm::logf("webui: WebView2Loader.dll not found");
        g_envPending = false;
        g_waiting.clear();
        return;
    }
    std::wstring data = wide(plat::cacheDir() + "/webview2");
    for (wchar_t& ch : data) if (ch == L'/') ch = L'\\';
    auto done = new Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(
        [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            g_envPending = false;
            std::vector<Window*> ws;
            ws.swap(g_waiting);
            if (FAILED(hr) || !env) {
                rbm::logf("webui: WebView2 environment failed (0x%08lx)", (unsigned long)hr);
                return S_OK;
            }
            g_env = env;
            env->AddRef();
            for (Window* w : ws) createController(w);
            return S_OK;
        });
    HRESULT hr = create(nullptr, data.c_str(), nullptr, done);
    done->Release();
    if (FAILED(hr)) {
        rbm::logf("webui: CreateCoreWebView2EnvironmentWithOptions failed (0x%08lx)", (unsigned long)hr);
        g_envPending = false;
        g_waiting.clear();
    }
}

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Window* w = (Window*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_SIZE:
            if (w) resize(w);
            return 0;
        case WM_CLOSE:   // hide; open() shows it again
            ShowWindow(h, SW_HIDE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void registerClass() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = nullptr;  // the web view paints (WM_ERASEBKGND is ignored)
    wc.lpszClassName = L"PlatendoosToolWindow";
    RegisterClassW(&wc);
}

}  // namespace

Window* open(const char* id, const char* page, const char* title, int width, int height,
             std::function<void(const std::string&)> onMessage) {
    auto it = windows().find(id);
    if (it != windows().end() && !IsWindow(it->second->hwnd)) {  // destroyed with its owner: make a new one
        windows().erase(it);
        it = windows().end();
    }
    if (it != windows().end()) {
        Window* w = it->second;
        w->onMessage = std::move(onMessage);
        ShowWindow(w->hwnd, IsIconic(w->hwnd) ? SW_RESTORE : SW_SHOW);
        // Above the other tool windows (SetForegroundWindow alone may not raise it among rekordbox's owned windows).
        SetWindowPos(w->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(w->hwnd);
        return w;
    }
    registerClass();
    Window* w = new Window;
    w->id = id;
    w->page = page;
    w->onMessage = std::move(onMessage);

    HWND owner = findMainWindow();
    UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (!dpi) dpi = 96;
    RECT r{0, 0, MulDiv(width, dpi, 96), MulDiv(height, dpi, 96)};
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&r, style, FALSE, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top, x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT o;
    if (owner && GetWindowRect(owner, &o)) {
        x = o.left + ((o.right - o.left) - ww) / 2;
        y = o.top + ((o.bottom - o.top) - wh) / 2;
        if (x < o.left) x = o.left;
        if (y < o.top) y = o.top;
    }
    w->hwnd = CreateWindowExW(0, L"PlatendoosToolWindow", wide(title).c_str(), style, x, y, ww, wh, owner, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!w->hwnd) {
        rbm::logf("webui: CreateWindow failed (%lu)", GetLastError());
        delete w;
        return nullptr;
    }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    // a dark title bar (Windows 10 1809+/11), looked up at run time
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto set = (SetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            BOOL on = TRUE;
            set(w->hwnd, 20, &on, sizeof on);
        }
    }
    // the platendoos icon (resource 1 of the module this code is in: rbmods.dll, or the patcher), at this DPI
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&open, &self);
    UINT wdpi = GetDpiForWindow(w->hwnd);
    for (auto [which, metric] : {std::pair{ICON_SMALL, SM_CXSMICON}, std::pair{ICON_BIG, SM_CXICON}}) {
        int px = GetSystemMetricsForDpi(metric, wdpi ? wdpi : 96);
        if (HICON icon = (HICON)LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, 0))
            SendMessageW(w->hwnd, WM_SETICON, which, (LPARAM)icon);
    }
    windows()[id] = w;
    ShowWindow(w->hwnd, SW_SHOW);
    SetForegroundWindow(w->hwnd);
    withEnvironment(w);
    RECT wr{};
    GetWindowRect(w->hwnd, &wr);
    wchar_t ot[128] = L"";
    if (owner) GetWindowTextW(owner, ot, 128);
    rbm::logf("webui: %s opened at %ld,%ld %ldx%ld (owner '%s', visible %d)", id, wr.left, wr.top, wr.right - wr.left,
              wr.bottom - wr.top, narrow(ot).c_str(), IsWindowVisible(w->hwnd) ? 1 : 0);
    return w;
}

void post(Window* w, const std::string& json) {
    if (!w) return;
    if (w->loaded) return deliver(w, json);
    // A page that never loads (no WebView2) mustn't keep every post of the session: the newest are enough, a page
    // asks for the whole state anyway once it is up (rbm.ready()).
    if (w->queue.size() >= 32) w->queue.erase(w->queue.begin());
    w->queue.push_back(json);
}

bool visible(Window* w) { return w && IsWindowVisible(w->hwnd); }

// Windows has no cheap occlusion query; minimised is the case that matters.
bool onScreen(Window* w) { return visible(w) && !IsIconic(w->hwnd); }

void close(Window* w) {
    if (w) ShowWindow(w->hwnd, SW_HIDE);
}

}  // namespace rbm::webui
