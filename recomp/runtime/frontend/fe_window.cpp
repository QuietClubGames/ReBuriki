// Win32 window: shows the 512x448 picture through Direct3D 11 (fe_d3d11.cpp), or GDI as a fallback; window /
// borderless / exclusive full screen; frame pacing (the arcade's 59.43 Hz, or vsync); the pause loop that
// keeps the menu running while emulated time stands still; the --menu-test screenshot option.
//
// Adapted from ReLayer (port/pc/video_win32.c and parts of main.c), Copyright (c) 2026 The ReLayer project,
// MIT License (see LICENSE-ReLayer.txt in this folder).
#include "fe_internal.h"
#include "frontend.h"
#include "settings.h"
#include "ui.h"
#include "../hw.h"
#include "../audio_out.h"
#include <mmsystem.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static HWND g_wnd;
static bool g_closing;
static int g_hz, g_use_d3d;
static FeOptions g_opt;
static LARGE_INTEGER g_freq;
static std::atomic<uint64_t> g_activity;
static std::atomic<bool> g_in_sizemove;
int g_aspect_mode;
float g_ui_fps = 59.43f;
bool g_no_save;

// ---------------------------------------------------------------- small helpers
void fe_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, fmt[0] == '[' ? "" : "[frontend] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

std::string fe_path(const char* name) {
    static std::string dir;
    if (dir.empty()) {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, exe, sizeof exe);
        while (n > 0 && exe[n - 1] != '\\' && exe[n - 1] != '/') n--;
        dir.assign(exe, n);
    }
    return dir + name;
}

uint64_t fe_activity(void) {
    // while the menu is open or the window is being moved, the game is meant to stand still
    if (g_in_sizemove || ui_menu_wants_pause()) return GetTickCount64();
    return g_activity;
}

bool fe_window_active(void) { return g_wnd != nullptr; }
bool video_focused(void) { return g_wnd && GetForegroundWindow() == g_wnd; }

static int display_hz() {
    DEVMODEA dm = {};
    dm.dmSize = sizeof dm;
    return EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 0;
}

// ---------------------------------------------------------------- settings / start-up
static std::string g_running_bios = "brom1.bin";

void fe_init(const FeOptions& opt) {
    g_opt = opt;
    g_no_save = opt.no_save || opt.menu_test;
    settings_defaults(&g_settings);
    settings_load(&g_settings, fe_path("settings.ini").c_str());
    fe_controls_load();
}

int fe_override_setting(const char* kv) { return settings_set(&g_settings, kv); }

const char* fe_bios_file(void) { return settings_region_bios(g_settings.region); }
void fe_note_running_bios(const char* file) { g_running_bios = file ? file : "brom1.bin"; }
int fe_running_region(void) {
    for (int r = 0; r < REGION_COUNT; r++) if (g_running_bios == settings_region_bios(r)) return r;
    return -1;
}

int fe_volume(void) { return g_settings.mute ? 0 : g_settings.volume; }

// ---------------------------------------------------------------- monitors
#define MAX_MONS 8
static HMONITOR g_mons[MAX_MONS];
static char g_mon_names[MAX_MONS][64];
static int g_nmons;

static BOOL CALLBACK mon_cb(HMONITOR m, HDC, LPRECT r, LPARAM) {
    if (g_nmons < MAX_MONS) {
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoA(m, &mi);
        snprintf(g_mon_names[g_nmons], sizeof g_mon_names[0], "Display %d  %ld\xC3\x97%ld%s", g_nmons + 1,
                 r->right - r->left, r->bottom - r->top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? "  (main)" : "");
        g_mons[g_nmons++] = m;
    }
    return TRUE;
}
static void list_monitors() { g_nmons = 0; EnumDisplayMonitors(nullptr, nullptr, mon_cb, 0); }
int video_monitor_count(void) { if (!g_nmons) list_monitors(); return g_nmons; }
const char* video_monitor_name(int i) { if (!g_nmons) list_monitors(); return i >= 0 && i < g_nmons ? g_mon_names[i] : "?"; }

// ---------------------------------------------------------------- display mode
static const int k_sizes[][2] = {{640, 480}, {960, 720}, {1195, 896}, {1280, 960}, {1600, 1200}, {1792, 1344}};
static const int k_nsizes = (int)(sizeof k_sizes / sizeof k_sizes[0]);
static bool g_reclaim_exclusive;
static int g_mode = -1;              // DISPLAY_*
static bool g_fullscreen;
static RECT g_window_rect;
static bool g_have_window_rect;
static int g_pacing = PACING_ARCADE;

static void place_window(int w, int h, HMONITOR mon, bool keep_pos) {
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoA(mon, &mi);
    int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    if (ww > aw || wh > ah) {               // too big for this monitor: shrink, keeping the shape
        float k = fminf((float)aw / (float)ww, (float)ah / (float)wh);
        ww = (int)((float)ww * k); wh = (int)((float)wh * k);
    }
    int x = mi.rcWork.left + (aw - ww) / 2, y = mi.rcWork.top + (ah - wh) / 2;
    if (keep_pos) { x = g_window_rect.left; y = g_window_rect.top; }
    SetWindowLongA(g_wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPos(g_wnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void video_apply_display(int mode, int window_size, int monitor) {
    if (!g_wnd) return;
    if (window_size < 0 || window_size >= k_nsizes) window_size = 2;
    if (!g_nmons) list_monitors();
    HMONITOR mon = (monitor >= 1 && monitor <= g_nmons) ? g_mons[monitor - 1] : MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST);
    if (g_mode == DISPLAY_EXCLUSIVE && mode != DISPLAY_EXCLUSIVE && g_use_d3d) d3d_set_exclusive(0, nullptr);
    if (mode == DISPLAY_WINDOW) {
        if (g_mode == DISPLAY_WINDOW) { GetWindowRect(g_wnd, &g_window_rect); g_have_window_rect = true; }
        int w = k_sizes[window_size][0], h = k_sizes[window_size][1];
        place_window(w, h, mon, g_mode == DISPLAY_WINDOW && g_have_window_rect && monitor == 0);
        g_fullscreen = false;
        fe_log("video: window %dx%d", w, h);
    } else {
        if (g_mode == DISPLAY_WINDOW) { GetWindowRect(g_wnd, &g_window_rect); g_have_window_rect = true; }
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoA(mon, &mi);
        SetWindowLongA(g_wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = true;
        if (mode == DISPLAY_EXCLUSIVE) SetForegroundWindow(g_wnd);   // DXGI only grants exclusive mode to the focused window
        if (mode == DISPLAY_EXCLUSIVE && !(g_use_d3d && d3d_set_exclusive(1, mon))) {
            fe_log("video: exclusive full screen unavailable, using borderless");
            mode = DISPLAY_BORDERLESS;
        }
        fe_log("video: %s full screen %ldx%ld", mode == DISPLAY_EXCLUSIVE ? "exclusive" : "borderless",
               mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
    g_mode = mode;
}

// Vsync only makes sense when the display runs at (a multiple of) ~60 Hz; otherwise the timer sets the pace.
void video_set_pacing(int pacing) {
    g_pacing = pacing;
    int interval = 0;
    if (pacing == PACING_VSYNC && g_opt.throttle)
        for (int k = 1; k <= 4; k++)
            if (g_hz >= 59 * k && g_hz <= 61 * k) interval = k;
    if (g_use_d3d) d3d_set_sync(interval);
    static int logged = -1;
    if (logged != interval) {
        fe_log("video: pacing %s", interval ? "by the display's vsync" : g_opt.throttle ? "to the arcade's 59.43 Hz" : "off (as fast as possible)");
        logged = interval;
    }
}

// ---------------------------------------------------------------- key queue (menu / hotkeys)
static int g_keyq[32];
static int g_keyq_n;
static void push_key(int vk) { if (g_keyq_n < 32) g_keyq[g_keyq_n++] = vk; }
int video_pop_key(void) {
    if (!g_keyq_n) return 0;
    int vk = g_keyq[0];
    memmove(g_keyq, g_keyq + 1, sizeof(int) * (size_t)(--g_keyq_n));
    return vk;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    g_activity++;
    switch (m) {
    case WM_CLOSE: g_closing = true; return 0;
    case WM_ACTIVATEAPP:
        // DXGI leaves exclusive full screen when another program takes focus; take the monitor back on return
        if (w && g_mode == DISPLAY_EXCLUSIVE) g_reclaim_exclusive = true;
        break;
    case WM_ENTERSIZEMOVE: g_in_sizemove = true; break;
    case WM_EXITSIZEMOVE: g_in_sizemove = false; break;
    case WM_KEYDOWN: push_key((int)w | ((l & (1 << 30)) ? KEY_REPEAT : 0)); return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) {            // Alt+Enter
            if (!(l & (1 << 30))) push_key(VK_F11);         // ignore key repeat
            return 0;
        }
        if (w == VK_F10 || w == VK_MENU) { push_key((int)w | ((l & (1 << 30)) ? KEY_REPEAT : 0)); return 0; }
        break;
    case WM_SYSCHAR:
        if (w == VK_RETURN) return 0;                        // no "ding" for Alt+Enter
        break;
    case WM_SETCURSOR:
        if (g_fullscreen && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; }   // hide the mouse
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

void fe_window_open(void) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "BurikiRecomp";
    RegisterClassA(&wc);
    int ws = g_settings.window_size >= 0 && g_settings.window_size < k_nsizes ? g_settings.window_size : 2;
    RECT r = {0, 0, k_sizes[ws][0], k_sizes[ws][1]};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("BurikiRecomp", "Buriki One", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);              // 1 ms Sleep granularity (default is ~15.6 ms)
    if (!g_wnd) { fe_log("video: couldn't create the window"); exit(1); }
    g_hz = getenv("BURIKI_FORCE_HZ") ? atoi(getenv("BURIKI_FORCE_HZ")) : display_hz();
    g_use_d3d = getenv("BURIKI_FORCE_GDI") ? 0 : d3d_init(g_wnd, g_hz, g_settings.gpu);
    fe_log("video: display %d Hz, drawing with %s", g_hz, g_use_d3d ? "Direct3D 11" : "GDI");
    pad_init();
    menu_host_init();                // applies the settings (full screen, filter, pacing...)
    if (g_opt.scale > 0 && g_settings.display_mode == DISPLAY_WINDOW)      // --scale N for this run
        place_window(448 * g_opt.scale * 4 / 3, 448 * g_opt.scale, MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), false);
    if (!g_use_d3d) fe_log("video: the menu needs Direct3D 11; without it Esc does nothing and closing the window quits");
}

int video_menu_drawable(void) { return g_use_d3d && !ui_d3d_failed(); }

// ---------------------------------------------------------------- drawing
static void present_gdi(const uint32_t* rgb, int w, int h) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    RECT cr;
    GetClientRect(g_wnd, &cr);
    int px, py, pw, ph;
    picture_rect(cr.right, cr.bottom, &px, &py, &pw, &ph);
    HDC dc = GetDC(g_wnd);
    if (px > 0) { PatBlt(dc, 0, 0, px, cr.bottom, BLACKNESS); PatBlt(dc, px + pw, 0, cr.right - px - pw, cr.bottom, BLACKNESS); }
    if (py > 0) { PatBlt(dc, 0, 0, cr.right, py, BLACKNESS); PatBlt(dc, 0, py + ph, cr.right, cr.bottom - py - ph, BLACKNESS); }
    SetStretchBltMode(dc, g_settings.filter == FILTER_SMOOTH ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, px, py, pw, ph, 0, 0, w, h, rgb, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_wnd, dc);
}

static void present(const uint32_t* rgb, int w, int h) {
    if (g_reclaim_exclusive) {
        g_reclaim_exclusive = false;
        if (g_mode == DISPLAY_EXCLUSIVE && g_use_d3d) d3d_set_exclusive(1, MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST));
    }
    if (g_use_d3d) {
        if (d3d_present(rgb, w, h)) return;
        // device lost (driver update or crash, graphics card switched...): start Direct3D again. GDI is the last
        // resort: it can't draw the menu.
        static int restarts;
        d3d_shutdown();
        if (restarts < 5 && d3d_init(g_wnd, g_hz, g_settings.gpu)) {
            restarts++;
            fe_log("video: Direct3D failed; started it again");
            video_set_pacing(g_pacing);
            if (g_mode == DISPLAY_EXCLUSIVE) g_reclaim_exclusive = true;
            return;
        }
        fe_log("video: Direct3D failed for good, drawing with GDI (no menu)");
        g_use_d3d = 0;
    }
    present_gdi(rgb, w, h);
}

// Wait for the next tick of a fixed schedule (no drift). Needs timeBeginPeriod(1) for Sleep(1) to be ~1 ms.
static void timer_pace() {
    const double frame = (double)g_freq.QuadPart * 0.016826519;      // HNG64: ~59.43 Hz (MAME tcram_w)
    static double next;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (next == 0 || (double)now.QuadPart - next > 4 * frame) next = (double)now.QuadPart;   // fell far behind: resync
    next += frame;
    for (;;) {
        QueryPerformanceCounter(&now);
        double left = next - (double)now.QuadPart;
        if (left <= 0) break;
        if (left > (double)g_freq.QuadPart / 500.0) Sleep(1);   // > 2 ms to go
    }
}

static void wait_frame() {
    if (!g_opt.throttle) return;
    static LARGE_INTEGER last;
    LARGE_INTEGER now;
    if (g_use_d3d && d3d_vsync()) {
        // Present() already waited for the display. Safety net: if it returned early (window minimised or
        // covered, so nothing is shown), use the timer so the game doesn't run fast.
        QueryPerformanceCounter(&now);
        double ms = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)g_freq.QuadPart : 16.7;
        if (ms < 12.0) timer_pace();
    } else {
        timer_pace();
    }
    QueryPerformanceCounter(&last);
}

[[noreturn]] void fe_quit(void) {
    fprintf(stderr, "[video] window closed\n");
    hw_report();
    fflush(stderr);
    if (g_use_d3d) d3d_shutdown();       // leave exclusive full screen cleanly
    exit(0);                             // atexit: the save memory is written
}

// ---------------------------------------------------------------- --menu-test
static int g_test_state;
static int g_test_n;
static void menu_test_tick() {
    if (!g_opt.menu_test || g_test_state == 2) return;
    static char shot[MAX_PATH];
    if (g_test_state == 0) {
        if (hw_frame_number() < g_opt.menu_test_at) return;
        char page[32] = {0};
        snprintf(page, sizeof page, "%s", g_opt.menu_test);
        int dev = UI_DEV_KEYBOARD;
        if (char* c = strchr(page, ':')) { *c = 0; if (!strcmp(c + 1, "pad")) dev = UI_DEV_PAD; }
        static const char* names[] = {"display", "audio", "controls", "game", "system"};
        int idx = -1;
        for (int i = 0; i < 5; i++) if (!strcmp(page, names[i])) idx = i;
        bool capture = !strcmp(page, "capture");
        if (capture) idx = UI_PAGE_CONTROLS;
        if (!strcmp(page, "pause")) idx = UI_PAGE_SYSTEM;
        if (idx >= 0) {
            ui_menu_set_page(idx);
            ui_menu_toggle(dev);
            if (capture) { ui_key(UI_DOWN, dev); ui_key(UI_OK, dev); }   // "Player" -> "Button 1", start remapping
        } else if (!strcmp(page, "toast")) {
            ui_toast("Controller 1 connected");
        }
        if (g_opt.menu_shot) snprintf(shot, sizeof shot, "%s", g_opt.menu_shot);
        else snprintf(shot, sizeof shot, "%s", fe_path((std::string("menu_test_") + page + ".bmp").c_str()).c_str());
        fe_log("menu test: %s at frame %u", g_opt.menu_test, hw_frame_number());
        g_test_state = 1;
        g_test_n = 0;
        return;
    }
    ++g_test_n;
    if (!strcmp(g_opt.menu_test, "pause")) {
        // pause test: keep the menu open for 4 s of real time (longer than the watchdog's 3 s), save a picture,
        // close it and let the run carry on (its RAM fingerprint must match a run without the pause)
        static ULONGLONG t0;
        if (g_test_n == 1) t0 = GetTickCount64();
        if (g_test_n == 45) d3d_request_backbuffer_save(shot);
        if (g_test_n > 45 && GetTickCount64() - t0 >= 4000) {
            fe_log("menu test: closing the menu at frame %u after %llu ms", hw_frame_number(), GetTickCount64() - t0);
            ui_key(UI_CLOSE, UI_DEV_KEYBOARD);
            g_test_state = 2;
        }
        return;
    }
    if (g_test_n == 45) d3d_request_backbuffer_save(shot);
    if (g_test_n == 46) {
        if (!g_use_d3d) fe_log("menu test: no Direct3D, nothing saved");
        fe_log("menu test done");
        fflush(stderr);
        if (g_use_d3d) d3d_shutdown();
        exit(0);
    }
}

// ---------------------------------------------------------------- --test-keys
// "N:key,N:key,...": press keys (esc tab enter up down left right q e f9 f11, or a hex VK code) at displayed
// frame N, through the same queue the window's key presses go to. Displayed frames keep counting while the
// menu has the game paused.
static uint32_t g_shown;
static void test_keys_tick() {
    if (!g_opt.test_keys) return;
    static const struct { const char* n; int vk; } names[] = {
        {"esc", VK_ESCAPE}, {"tab", VK_TAB}, {"enter", VK_RETURN}, {"up", VK_UP}, {"down", VK_DOWN},
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"q", 'Q'}, {"e", 'E'}, {"f9", VK_F9}, {"f11", VK_F11}};
    const char* p = g_opt.test_keys;
    while (*p) {
        unsigned n = 0;
        char key[16] = {0};
        if (sscanf(p, "%u:%15[^,]", &n, key) == 2 && n == g_shown) {
            int vk = (int)strtol(key, nullptr, 16);
            for (auto& k : names) if (!strcmp(key, k.n)) vk = k.vk;
            if (vk > 0) { push_key(vk); fe_log("test key %s at displayed frame %u", key, n); }
        }
        const char* c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
    }
}

// ---------------------------------------------------------------- one displayed frame
static void one_frame(const uint32_t* rgb, int w, int h) {
    g_activity++;
    g_shown++;
    test_keys_tick();
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    if (g_closing) fe_quit();
    // real time since the last frame, for the menu's animations
    static LARGE_INTEGER prev;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = prev.QuadPart ? (float)((double)(now.QuadPart - prev.QuadPart) / (double)g_freq.QuadPart) : 1.0f / 60;
    if (dt > 0.1f) dt = 0.1f;
    prev = now;
    pad_poll();
    menu_host_frame(dt);
    present(rgb, w, h);
    wait_frame();
    // frame rate shown by the overlay counter (smoothed)
    static LARGE_INTEGER last_fps;
    QueryPerformanceCounter(&now);
    if (last_fps.QuadPart) {
        double ms = (double)(now.QuadPart - last_fps.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
        if (ms > 0) g_ui_fps = g_ui_fps * 0.95f + (float)(1000.0 / ms) * 0.05f;
    }
    last_fps = now;
    menu_test_tick();
}

void fe_present_frame(const uint32_t* rgb, int w, int h) {
    one_frame(rgb, w, h);
    fe_nvram_tick();
    if (ui_menu_wants_pause()) {
        // The pause: the game stays inside this vblank, so no emulated time passes. The frozen frame keeps
        // being shown under the menu, and the sound card is paused rather than left to run dry.
        audio_out_pause(true);
        while (ui_menu_wants_pause()) one_frame(rgb, w, h);
        audio_out_pause(false);
    }
}
