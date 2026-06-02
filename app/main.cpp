// Horizon CloudBeats — desktop shell.
//
// Two completely separate layers, by design:
//
//   1. The "waiting" screen is drawn with NATIVE Win32/GDI (logo + title +
//      spinner). It uses no web engine and no sockets, so while the game is
//      closed there is *nothing* that could ever touch port 8420.
//
//   2. The WebView2 control (the Edge/Chromium engine) is created *only* once
//      Forza Horizon 6 is detected running. The in-game radio server lives in
//      version.dll and binds port 8420 when the game launches; the WebView2 is
//      then just a client that navigates to http://localhost:8420. When the
//      game closes we fully tear the WebView2 down again and go back to the
//      native screen.
//
// This guarantees the port is opened by the DLL (when the game starts), never
// by this app: the browser engine simply does not exist before the game is up.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wrl.h>
#include "WebView2.h"

#include "resource.h"

#include <algorithm>
using std::min;          // GDI+ headers reference unqualified min/max and we
using std::max;          // build with NOMINMAX, so pull them into global scope.
#include <gdiplus.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Microsoft::WRL;

namespace {

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2>           g_webview;
HWND                            g_hwnd  = nullptr;
HICON                           g_logo  = nullptr;   // large icon for the native screen

constexpr wchar_t kClassName[] = L"HorizonCloudBeatsShell";
constexpr wchar_t kTitle[]     = L"Horizon CloudBeats";

constexpr UINT_PTR kSpinTimer  = 1;     // animates the native spinner
constexpr UINT     kSpinPeriod = 16;    // ms (~60fps, buttery smooth)

// Small in-memory page shown *only* while the game is running but the radio
// server hasn't answered yet (the brief warm-up after launch). It never appears
// before the game, so it can't be the thing that opens the port.
const wchar_t* kConnectingHtml = LR"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  :root{color-scheme:dark;} *{box-sizing:border-box;}
  html,body{height:100%;margin:0;}
  body{font-family:'Segoe UI',system-ui,sans-serif;background:#080808;color:#e8e8e8;
       display:flex;align-items:center;justify-content:center;text-align:center;
       -webkit-user-select:none;user-select:none;}
  .wrap{display:flex;flex-direction:column;align-items:center;gap:22px;padding:32px;}
  h1{font-size:22px;font-weight:600;letter-spacing:.06em;margin:0;color:#fff;}
  p{font-size:15px;color:#9a9a9a;margin:0;max-width:420px;line-height:1.6;}
  .pill{display:inline-flex;align-items:center;gap:10px;padding:11px 18px;border-radius:999px;
        background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.1);font-size:13px;color:#bbb;}
  .spin{width:16px;height:16px;border-radius:50%;border:2px solid rgba(255,255,255,.18);
        border-top-color:#fff;animation:spin .8s linear infinite;}
  @keyframes spin{to{transform:rotate(360deg);}}
</style></head>
<body><div class="wrap">
  <h1>HORIZON CLOUDBEATS</h1>
  <p>Game detected — connecting to the in-game radio…</p>
  <div class="pill"><span class="spin"></span><span>Connecting…</span></div>
</div></body></html>
)HTML";

// --- Game detection ---------------------------------------------------------
// Pure process enumeration (Toolhelp32). No sockets, no network.
bool game_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize  = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name = pe.szExeFile;
            for (auto& c : name) c = (wchar_t)towlower(c);
            if (name.find(L"forzahorizon") != std::wstring::npos) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Confirm the in-game radio server (version.dll) is actually answering on
// localhost:8420 -- i.e. the DLL, not us, has opened the site. This is a pure
// *client* connection (connect + GET): it uses an ephemeral outbound port and
// never creates a listening socket, so the app still cannot open port 8420.
// We also require a real "HTTP/" reply, so a stale socket that merely accepts
// connections without serving anything is correctly treated as "not ready".
bool dashboard_ready() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD tmo = 400;   // ms; keep the watch loop responsive
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));

    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(8420);
    a.sin_addr.s_addr = htonl(0x7F000001);   // 127.0.0.1

    bool ok = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        const char* req = "GET / HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        send(s, req, static_cast<int>(std::strlen(req)), 0);
        char buf[16]{};
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        ok = (n >= 5 && std::strncmp(buf, "HTTP/", 5) == 0);
    }
    closesocket(s);
    return ok;
}

// --- State ------------------------------------------------------------------
enum class View { Waiting, Dashboard };

constexpr UINT WM_PROBE_RESULT = WM_APP + 1;   // wParam = 1 (game up) / 0 (down)

View              g_desired   = View::Waiting;  // what we want to show (UI thread)
bool              g_creating  = false;          // a WebView2 creation is in flight
std::atomic<bool> g_connected{false};           // dashboard navigation succeeded (read by watcher)
float             g_spin      = 0.f;            // native spinner angle (degrees)
RECT              g_link_rect{};                // clickable "localhost:8420" hit box
std::atomic<bool> g_expect_dashboard{false};    // last nav targeted the dashboard
std::atomic<bool> g_running{true};
std::thread       g_watch_thread;

constexpr wchar_t kDashboardUrl[] = L"http://localhost:8420";

void resize_webview() {
    if (!g_controller) return;
    RECT r{};
    GetClientRect(g_hwnd, &r);
    g_controller->put_Bounds(r);
}

void navigate_dashboard() {
    if (!g_webview) return;
    g_expect_dashboard.store(true, std::memory_order_release);
    g_webview->Navigate(L"http://localhost:8420");
}
void navigate_connecting() {
    if (!g_webview) return;
    g_expect_dashboard.store(false, std::memory_order_release);
    g_webview->NavigateToString(kConnectingHtml);
}

// Tear the WebView2 down completely so the Chromium engine (and anything it
// might hold) goes away while we're back to waiting.
void destroy_webview() {
    if (g_controller) g_controller->Close();
    g_webview.Reset();
    g_controller.Reset();
    g_connected.store(false, std::memory_order_release);
    g_creating  = false;
}

void create_webview(HWND hwnd);   // fwd

// Reconcile actual state with desired state. Always runs on the UI thread.
void reconcile() {
    if (g_desired == View::Dashboard) {
        KillTimer(g_hwnd, kSpinTimer);                  // no native spinner needed
        if (!g_controller && !g_creating) {
            g_creating = true;
            create_webview(g_hwnd);                     // navigates once ready
        } else if (g_controller && !g_connected.load(std::memory_order_acquire)) {
            navigate_dashboard();                       // retry until the server answers
        }
    } else { // Waiting
        if (g_controller || g_creating) destroy_webview();
        SetTimer(g_hwnd, kSpinTimer, kSpinPeriod, nullptr);
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

// --- Dark title bar (active and inactive same colour) -----------------------
constexpr COLORREF kCaptionColor = 0x001A1A1A;   // dark gray, ~#1A1A1A

void apply_caption_color(HWND hwnd) {
    // DWMWA_CAPTION_COLOR (35) — Windows 11 build 22000+
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/,
                          &kCaptionColor, sizeof(kCaptionColor));
    // DWMWA_BORDER_COLOR (34) — keep the border matching
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/,
                          &kCaptionColor, sizeof(kCaptionColor));
}

// --- Native waiting screen (GDI / GDI+) ------------------------------------

HFONT make_font(int px, int weight, bool underline) {
    return CreateFontW(px, 0, 0, 0, weight, FALSE, underline ? TRUE : FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

// Smooth, anti-aliased ring spinner: a faint full ring plus a bright rotating
// arc with round caps -- mirrors the dashboard's CSS spinner.
void draw_spinner(HDC dc, int cx, int cy) {
    using namespace Gdiplus;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    const REAL R = 15.f, pw = 2.f;
    REAL x = cx - R, y = cy - R, d = R * 2.f;

    Pen track(Color(38, 255, 255, 255), pw);          // faint full ring
    g.DrawEllipse(&track, x, y, d, d);

    Pen arc(Color(255, 255, 255, 255), pw);           // bright rotating arc
    arc.SetStartCap(LineCapRound);
    arc.SetEndCap(LineCapRound);
    g.DrawArc(&arc, x, y, d, d, g_spin, 80.f);
}

// One run of text that is either normal or bold.
struct TextRun { std::wstring text; bool bold; };

// Lay out the runs word-by-word, wrap to maxW, draw centred at cx from topY.
// Returns the Y just below the last line.
int draw_runs_centered(HDC dc, const std::vector<TextRun>& runs,
                       HFONT fn, HFONT fb, COLORREF color,
                       int cx, int topY, int maxW, int lineH) {
    struct Word { std::wstring t; bool bold; int w; };
    std::vector<Word> words;
    for (const auto& r : runs) {
        size_t i = 0;
        while (i < r.text.size()) {
            size_t j = r.text.find(L' ', i);
            if (j == std::wstring::npos) j = r.text.size();
            if (j > i) {
                Word wd{r.text.substr(i, j - i), r.bold, 0};
                HGDIOBJ f = SelectObject(dc, wd.bold ? fb : fn);
                SIZE sz{};
                GetTextExtentPoint32W(dc, wd.t.c_str(), (int)wd.t.size(), &sz);
                wd.w = sz.cx;
                SelectObject(dc, f);
                words.push_back(std::move(wd));
            }
            i = j + 1;
        }
    }
    HGDIOBJ f0 = SelectObject(dc, fn);
    SIZE spz{}; GetTextExtentPoint32W(dc, L" ", 1, &spz);
    SelectObject(dc, f0);
    int spaceW = spz.cx;

    std::vector<std::pair<int, int>> lines;   // [begin,end) word indices
    int start = 0, curW = 0;
    for (int i = 0; i < (int)words.size(); ++i) {
        int add = (i > start ? spaceW : 0) + words[i].w;
        if (i > start && curW + add > maxW) {
            lines.push_back({start, i});
            start = i; curW = words[i].w;
        } else {
            curW += add;
        }
    }
    if (start < (int)words.size()) lines.push_back({start, (int)words.size()});

    SetTextColor(dc, color);
    int y = topY;
    for (auto [b, e] : lines) {
        int total = 0;
        for (int i = b; i < e; ++i) total += words[i].w + (i > b ? spaceW : 0);
        int x = cx - total / 2;
        for (int i = b; i < e; ++i) {
            HGDIOBJ f = SelectObject(dc, words[i].bold ? fb : fn);
            TextOutW(dc, x, y, words[i].t.c_str(), (int)words[i].t.size());
            SelectObject(dc, f);
            x += words[i].w + spaceW;
        }
        y += lineH;
    }
    return y;
}

// Draw the "You can also open <url> in any browser." hint with a clickable,
// underlined URL. Records the URL's hit box in g_link_rect.
void draw_hint_link(HDC dc, int cx, int y, HFONT fn, HFONT fl, int lineH) {
    const wchar_t* pre  = L"You can also open ";
    const wchar_t* post = L" in any browser.";
    SIZE sp{}, su{}, so{};
    HGDIOBJ f = SelectObject(dc, fn);
    GetTextExtentPoint32W(dc, pre,  lstrlenW(pre),  &sp);
    GetTextExtentPoint32W(dc, post, lstrlenW(post), &so);
    SelectObject(dc, fl);
    GetTextExtentPoint32W(dc, kDashboardUrl, lstrlenW(kDashboardUrl), &su);
    SelectObject(dc, f);

    int total = sp.cx + su.cx + so.cx;
    int x = cx - total / 2;

    SetTextColor(dc, RGB(96, 96, 96));
    f = SelectObject(dc, fn);
    TextOutW(dc, x, y, pre, lstrlenW(pre));
    SelectObject(dc, f);

    int ux = x + sp.cx;
    SetTextColor(dc, RGB(230, 230, 230));
    f = SelectObject(dc, fl);
    TextOutW(dc, ux, y, kDashboardUrl, lstrlenW(kDashboardUrl));
    SelectObject(dc, f);
    g_link_rect = RECT{ux, y, ux + su.cx, y + lineH};

    SetTextColor(dc, RGB(96, 96, 96));
    f = SelectObject(dc, fn);
    TextOutW(dc, ux + su.cx, y, post, lstrlenW(post));
    SelectObject(dc, f);
}

void paint_waiting(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;

    // Double-buffer to avoid flicker on the animated spinner.
    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ ob  = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(RGB(8, 8, 8));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    SetBkMode(mem, TRANSPARENT);
    int cx = w / 2, cy = h / 2;

    // Logo only (no title) -- 20% larger than before (124 -> 149).
    if (g_logo) {
        const int ls = 180;
        DrawIconEx(mem, cx - ls / 2, cy - 195, g_logo, ls, ls, 0, nullptr, DI_NORMAL);
    }

    // Subtitle with selected words in bold.
    HFONT subN = make_font(-16, FW_NORMAL, false);
    HFONT subB = make_font(-16, FW_BOLD,   false);
    std::vector<TextRun> runs = {
        {L"Start ",                                false},
        {L"Forza Horizon 6",                       true },
        {L" to begin. This window connects ",      false},
        {L"automatically",                         true },
        {L" as soon as the in-game radio is ready.", false},
    };
    int afterSub = draw_runs_centered(mem, runs, subN, subB, RGB(170, 170, 170),
                                      cx, cy + 18, 470, 24);
    DeleteObject(subN);
    DeleteObject(subB);

    // Spinner.
    int spinY = afterSub + 32;
    draw_spinner(mem, cx, spinY);

    // Hint with clickable link.
    HFONT hintN = make_font(-13, FW_NORMAL, false);
    HFONT hintL = make_font(-13, FW_NORMAL, true);
    draw_hint_link(mem, cx, spinY + 28, hintN, hintL, 18);
    DeleteObject(hintN);
    DeleteObject(hintL);

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// --- Window proc ------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, kSpinTimer, kSpinPeriod, nullptr);
            apply_caption_color(hwnd);
            return 0;
        case WM_NCACTIVATE:
            // Let DefWindowProc paint, then reapply our dark caption so it
            // doesn't revert to the system colour when the window loses focus.
            DefWindowProc(hwnd, msg, wp, lp);
            apply_caption_color(hwnd);
            return TRUE;
        case WM_ERASEBKGND:
            return 1;                               // we paint the whole client ourselves
        case WM_PAINT:
            if (!g_controller) { paint_waiting(hwnd); return 0; }
            break;                                  // webview covers the client
        case WM_TIMER:
            if (wp == kSpinTimer && !g_controller) {
                g_spin += 6.f;
                if (g_spin >= 360.f) g_spin -= 360.f;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (!g_controller && LOWORD(lp) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (PtInRect(&g_link_rect, pt)) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONUP:
            if (!g_controller) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (PtInRect(&g_link_rect, pt))
                    ShellExecuteW(nullptr, L"open", kDashboardUrl, nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        case WM_SIZE:
            resize_webview();
            return 0;
        case WM_PROBE_RESULT:
            g_desired = (wp == 1u) ? View::Dashboard : View::Waiting;
            reconcile();
            return 0;
        case WM_DESTROY:
            g_running.store(false, std::memory_order_release);
            KillTimer(hwnd, kSpinTimer);
            destroy_webview();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Background watcher: polls for the game and posts the result to the UI thread.
void start_watch_thread(HWND hwnd) {
    g_watch_thread = std::thread([hwnd] {
        while (g_running.load(std::memory_order_acquire)) {
            // Decide whether the dashboard should be shown.
            //  - Game closed            -> no (and we never touch the network).
            //  - Game open, dashboard up -> stay; do NOT re-probe. The single-
            //    threaded DLL server is briefly unresponsive while it serves a
            //    request, and a failed probe must not tear the webview down and
            //    bounce back to the loading screen. Transient server hiccups are
            //    handled by the dashboard's own JS once it's loaded.
            //  - Game open, not yet shown -> probe HTTP once to confirm the DLL
            //    has actually opened the site, then connect.
            bool up;
            if (!game_running()) {
                up = false;
            } else if (g_connected.load(std::memory_order_acquire)) {
                up = true;                       // already connected: PID alone is enough
            } else {
                up = dashboard_ready();          // connect phase: confirm the server
            }
            if (g_running.load(std::memory_order_acquire))
                PostMessage(hwnd, WM_PROBE_RESULT, up ? 1u : 0u, 0);
            for (int i = 0; i < 12 && g_running.load(std::memory_order_acquire); ++i)
                Sleep(100);   // ~1.2 s, responsive to shutdown
        }
    });
}

void create_webview(HWND hwnd) {
    wchar_t local[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::wstring data_folder =
        (n && n < MAX_PATH) ? std::wstring(local) + L"\\HorizonCloudBeats" : L"";

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data_folder.empty() ? nullptr : data_folder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
                if (!env) { g_creating = false; return S_OK; }
                env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT, ICoreWebView2Controller* ctrl) -> HRESULT {
                            g_creating = false;
                            if (!ctrl) return S_OK;
                            g_controller = ctrl;

                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(g_controller.As(&c2)))
                                c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 8, 8, 8});

                            g_controller->get_CoreWebView2(&g_webview);
                            if (g_webview) {
                                ICoreWebView2Settings* s = nullptr;
                                if (SUCCEEDED(g_webview->get_Settings(&s)) && s) {
                                    s->put_AreDefaultContextMenusEnabled(FALSE);
                                    s->put_IsStatusBarEnabled(FALSE);
                                }
                                EventRegistrationToken tok{};
                                g_webview->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [](ICoreWebView2*,
                                           ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                            BOOL ok = FALSE;
                                            if (args) args->get_IsSuccess(&ok);
                                            if (g_expect_dashboard.load(std::memory_order_acquire)) {
                                                if (ok) {
                                                    g_connected.store(true, std::memory_order_release);   // dashboard live
                                                } else {
                                                    g_connected.store(false, std::memory_order_release);
                                                    navigate_connecting();    // show shell; retry next tick
                                                }
                                            }
                                            return S_OK;
                                        })
                                        .Get(),
                                    &tok);
                            }
                            resize_webview();

                            // The game may have closed during the (async) creation.
                            if (g_desired == View::Dashboard) navigate_dashboard();
                            else                               destroy_webview();
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);   // for the client-side dashboard probe

    Gdiplus::GdiplusStartupInput gsi{};
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr);

    HICON icon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    // Larger crisp icon for the native waiting screen (falls back to default).
    g_logo = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                               256, 256, LR_DEFAULTCOLOR);
    if (!g_logo) g_logo = icon;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kClassName;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(8, 8, 8));
    wc.hIcon         = icon;
    wc.hIconSm       = icon;
    RegisterClassExW(&wc);

    const int W = 1120, H = 800;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    g_hwnd = CreateWindowExW(0, kClassName, kTitle, WS_OVERLAPPEDWINDOW,
                             sx, sy, W, H, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    // Start watching only now. No WebView2 is created until the game appears.
    start_watch_thread(g_hwnd);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_running.store(false, std::memory_order_release);
    if (g_watch_thread.joinable()) g_watch_thread.join();
    WSACleanup();
    Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();
    return 0;
}
