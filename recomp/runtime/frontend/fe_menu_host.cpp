// Windows side of the overlay menu: turns keyboard and controller input into menu actions, implements the
// services the menu needs (settings, remapping, restart...), and applies settings to the presenter and audio.
//
// Adapted from ReLayer (port/pc/menu_host.c), Copyright (c) 2026 The ReLayer project, MIT License
// (see LICENSE-ReLayer.txt in this folder). Changes for Buriki One: keyboard remapping, region / restart,
// Back + Start opens the menu, restart relaunches the process.
#include "fe_internal.h"
#include "frontend.h"
#include "settings.h"
#include "ui.h"
#include "../hw.h"
#include "../netplay.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <shellapi.h>

int g_test_pulse;
int fe_running_region(void);
[[noreturn]] void fe_quit(void);

static int g_capture = -1;
static float g_capture_t;

// ---------------------------------------------------------------- settings
static int g_applied_display = -1, g_applied_size = -1, g_applied_monitor = -1, g_applied_pacing = -1, g_applied_wide = -1;

static void apply(int save) {
    if (g_settings.display_mode != g_applied_display || g_settings.window_size != g_applied_size ||
        g_settings.monitor != g_applied_monitor || (g_settings.aspect == ASPECT_WIDE) != g_applied_wide) {   // widescreen: a 16:9 window
        video_apply_display(g_settings.display_mode, g_settings.window_size, g_settings.monitor);
        g_applied_display = g_settings.display_mode;
        g_applied_size = g_settings.window_size;
        g_applied_monitor = g_settings.monitor;
        g_applied_wide = g_settings.aspect == ASPECT_WIDE;
    }
    if (g_settings.pacing != g_applied_pacing) { video_set_pacing(g_settings.pacing); g_applied_pacing = g_settings.pacing; }
    if (!netplay_active()) hw_set_unlock_silber(g_settings.unlock_silber != 0);     // from the next player select screen (online: the host's choice, fixed for the session)
    g_aspect_mode = g_settings.aspect;
    g_blit_smooth = g_settings.filter == FILTER_SMOOTH;
    g_blit_scan = g_settings.filter == FILTER_SCANLINES ? 1.0f : 0.0f;
    if (save && !g_no_save) settings_save(&g_settings, fe_path("settings.ini").c_str());
    // the log notes every change of the picture settings, so a report of a display problem shows what was set
    static int logged[6] = {-1, -1, -1, -1, -1, -1};
    int now[6] = {g_settings.display_mode, g_settings.enhanced, g_settings.quality, g_settings.filter,
                  g_settings.aspect, g_settings.pacing};
    if (memcmp(now, logged, sizeof now)) {
        fe_log("settings: display %d, enhanced %d, quality %d, filter %d, shape %d, pacing %d",
               now[0], now[1], now[2], now[3], now[4], now[5]);
        memcpy(logged, now, sizeof now);
    }
}

static void host_settings_changed(void) { apply(1); }
static const char* host_controller_name(int i) { return pad_player_connected(i) ? "Xbox controller" : nullptr; }
static void host_start_capture(int action) { g_capture = action; g_capture_t = 0; }
static int host_capturing(void) { return g_capture; }
static void host_reset_bindings(void) { bind_defaults(); }
static void host_operator_menu(void) { ui_key(UI_CLOSE, UI_DEV_KEYBOARD); g_test_pulse = 8; }

// Restart: start a new copy of the program with the same command line, then quit. (The recompiled game runs
// on this thread's stack from its entry point, so a fresh process is the clean way to power-cycle the board.)
static bool g_log_file;
void fe_note_log_file(bool on) { g_log_file = on; }

[[noreturn]] static void relaunch(wchar_t* cmd) {
    if (!g_no_save) settings_save(&g_settings, fe_path("settings.ini").c_str());
    fe_nvram_save();
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    fe_log("restarting");
    fflush(stderr);
    d3d_shutdown();                  // give up exclusive full screen before the new copy wants it
    // Let go of the log file first: the new copy keeps this session's log as buriki.old.log and starts its own,
    // which it can't do while this copy still has the file open.
    if (g_log_file) freopen("NUL", "w", stderr);
    if (CreateProcessW(exe, cmd, nullptr, nullptr, g_log_file ? FALSE : TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        exit(0);
    }
    fe_log("restart failed (%lu)", GetLastError());
    exit(1);
}
static void host_restart(void) { relaunch(_wcsdup(GetCommandLineW())); }

// One command-line argument, quoted the way Windows programs read it back.
static void append_arg(std::wstring& cmd, const wchar_t* a) {
    if (!cmd.empty()) cmd += L' ';
    if (*a && !wcspbrk(a, L" \t\"")) { cmd += a; return; }
    cmd += L'"';
    for (const wchar_t* p = a;; p++) {
        size_t slashes = 0;
        while (*p == L'\\') { p++; slashes++; }
        if (!*p) { cmd.append(slashes * 2, L'\\'); break; }
        if (*p == L'"') cmd.append(slashes * 2 + 1, L'\\');
        else cmd.append(slashes, L'\\');
        cmd += *p;
    }
    cmd += L'"';
}

// The same command line with the online-play options taken out and `netplay_args` (plain ASCII) put in.
[[noreturn]] void fe_relaunch(const char* netplay_args) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring cmd;
    for (int i = 0; argv && i < argc; i++) {
        if (i > 0 && (!wcscmp(argv[i], L"--netplay-host") || !wcscmp(argv[i], L"--netplay-join") || !wcscmp(argv[i], L"--netplay-delay"))) { i++; continue; }
        append_arg(cmd, argv[i]);
    }
    if (netplay_args && *netplay_args) {
        cmd += L' ';
        for (const char* c = netplay_args; *c; c++) cmd += (wchar_t)(unsigned char)*c;
    }
    relaunch(_wcsdup(cmd.c_str()));
}

// ---------------------------------------------------------------- online play
static int host_online_active(void) { return netplay_active(); }
static const char* host_online_status(void) { return netplay_active() ? netplay_status() : "Not connected"; }
static int host_online_ping(void) { return netplay_ping(); }
static const char* host_local_address(void) {
    static char s[96];
    snprintf(s, sizeof s, "%s    port %d", netplay_local_address(), g_settings.online_port);
    return s;
}
static void host_online_host(void) {
    char a[96];
    snprintf(a, sizeof a, "--netplay-host %d --netplay-delay %d", g_settings.online_port, g_settings.online_delay);
    fe_relaunch(a);
}
static void host_online_join(void) {
    char a[160];
    if (strchr(g_settings.online_address, ':')) snprintf(a, sizeof a, "--netplay-join %s", g_settings.online_address);
    else snprintf(a, sizeof a, "--netplay-join %s:%d", g_settings.online_address, g_settings.online_port);
    fe_relaunch(a);
}
static void host_online_leave(void) { fe_relaunch(""); }

// Ctrl+V while typing an address
static void paste_text(void) {
    if (!OpenClipboard(nullptr)) return;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char* t = (const char*)GlobalLock(h)) {
            for (int n = 0; *t && n < 80; t++, n++) {
                const char c = *t;
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-') ui_text_input(c);
                else if (c >= 'A' && c <= 'Z') ui_text_input(c + 32);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}
static void host_quit(void) { fe_quit(); }
static int host_monitor_count(void) { return video_monitor_count(); }
static const char* host_monitor_name(int i) { return video_monitor_name(i); }
static int host_gpu_count(void) { return d3d_gpu_count(); }
static const char* host_gpu_name(int i) { return d3d_gpu_name(i); }
static const char* host_save_info(void) { return fe_nvram_path() ? "nvram.bin" : "Not kept this run"; }

static const UiHost k_host = {
    host_settings_changed, host_controller_name, bind_pad_name, bind_key_name, host_start_capture, host_capturing,
    host_reset_bindings, bind_deadzone_percent, bind_set_deadzone_percent, host_operator_menu, host_restart, host_quit,
    host_monitor_count, host_monitor_name, host_gpu_count, host_gpu_name, fe_running_region, host_save_info,
    host_online_active, host_online_status, host_local_address, host_online_host, host_online_join, host_online_leave,
    host_online_ping,
};

void menu_host_init(void) {
    ui_init(&k_host);
    apply(0);
}

// ---------------------------------------------------------------- input
static uint32_t g_prev_held;
static int g_prev_dir;
static float g_dir_timer;
static bool g_combo_latched;
static int g_prev_connected[2];

// Open or close the menu. Without Direct3D it can't be drawn, and an invisible menu would pause the game
// for no visible reason (it looks like a freeze), so then it doesn't open.
static void menu_toggle(int dev) {
    if (!ui_menu_visible() && !video_menu_drawable()) {
        static bool told;
        if (!told) { told = true; fe_log("menu: can't be shown without Direct3D, not opened"); }
        return;
    }
    ui_menu_toggle(dev);
}

// Alt+Enter / F11: window <-> the full-screen mode last used (borderless unless exclusive was chosen)
static int g_last_full = DISPLAY_BORDERLESS;
static void toggle_fullscreen(void) {
    if (g_settings.display_mode == DISPLAY_WINDOW) g_settings.display_mode = g_last_full;
    else { g_last_full = g_settings.display_mode; g_settings.display_mode = DISPLAY_WINDOW; }
    apply(1);
    ui_toast(g_settings.display_mode == DISPLAY_WINDOW ? "Window" :
             g_settings.display_mode == DISPLAY_EXCLUSIVE ? "Exclusive full screen" : "Borderless full screen");
}

static void keyboard(void) {
    int k;
    while ((k = video_pop_key()) != 0) {
        int vk = k & 0xFFFF;
        bool repeat = (k & KEY_REPEAT) != 0;
        if (vk == VK_F11) { if (!repeat) toggle_fullscreen(); continue; }
        if (vk == VK_F9 && !repeat) {
            g_settings.enhanced = !g_settings.enhanced;
            apply(1);
            ui_toast(g_settings.enhanced ? "Enhanced graphics" : "Original arcade graphics");
            continue;
        }
        if (g_capture >= 0) {                         // remapping: Esc cancels, the next other key is taken
            if (repeat) continue;
            if (vk == VK_ESCAPE) { g_capture = -1; ui_toast("Remapping cancelled"); continue; }
            char b[64];
            if (bind_set_key(g_capture, vk)) {
                snprintf(b, sizeof b, "Assigned to %s", bind_key_name(g_capture));
                g_capture = -1;
            } else {
                snprintf(b, sizeof b, "That key is kept for the menu and hotkeys");
            }
            ui_toast(b);
            continue;
        }
        if (!ui_menu_visible() || !ui_menu_wants_pause()) {
            if ((vk == VK_ESCAPE || vk == VK_TAB) && !repeat) menu_toggle(UI_DEV_KEYBOARD);
            continue;
        }
        if (ui_text_editing()) {                      // typing an address (Online page)
            const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
            if (vk == VK_RETURN) { if (!repeat) ui_text_input(13); }
            else if (vk == VK_ESCAPE) { if (!repeat) ui_text_input(27); }
            else if (vk == VK_BACK) ui_text_input(8);
            else if (ctrl && vk == 'V') { if (!repeat) paste_text(); }
            else if (ctrl) {}
            else if (vk >= '0' && vk <= '9' && !shift) ui_text_input(vk);
            else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ui_text_input('0' + (vk - VK_NUMPAD0));
            else if (vk >= 'A' && vk <= 'Z') ui_text_input(vk + 32);
            else if (vk == VK_OEM_PERIOD || vk == VK_DECIMAL) ui_text_input('.');
            else if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) ui_text_input('-');
            else if (vk == VK_OEM_1 && shift) ui_text_input(':');
            continue;
        }
        switch (vk) {
        case VK_UP: ui_key(UI_UP, UI_DEV_KEYBOARD); break;
        case VK_DOWN: ui_key(UI_DOWN, UI_DEV_KEYBOARD); break;
        case VK_LEFT: ui_key(UI_LEFT, UI_DEV_KEYBOARD); break;
        case VK_RIGHT: ui_key(UI_RIGHT, UI_DEV_KEYBOARD); break;
        case VK_RETURN: case VK_SPACE: if (!repeat) ui_key(UI_OK, UI_DEV_KEYBOARD); break;
        case VK_ESCAPE: case VK_BACK: if (!repeat) ui_key(UI_BACK, UI_DEV_KEYBOARD); break;
        case VK_TAB: if (!repeat) ui_key(UI_CLOSE, UI_DEV_KEYBOARD); break;
        case 'Q': case VK_PRIOR: ui_key(UI_PREV_TAB, UI_DEV_KEYBOARD); break;
        case 'E': case VK_NEXT: ui_key(UI_NEXT_TAB, UI_DEV_KEYBOARD); break;
        }
    }
}

static void controller(float dt) {
    uint32_t held = pad_held_all();
    uint32_t pressed = held & ~g_prev_held;
    const uint32_t back_bit = 1u << SRC_BACK, start_bit = 1u << SRC_START;

    // hot-plug notices
    for (int i = 0; i < 2; i++) {
        int c = pad_player_connected(i);
        if (c != g_prev_connected[i]) {
            char b[64];
            snprintf(b, sizeof b, "Controller %d %s", i + 1, c ? "connected" : "disconnected");
            ui_toast(b);
            g_prev_connected[i] = c;
        }
    }

    // remapping: the next newly pressed button (not the one that started it)
    if (g_capture >= 0) {
        g_capture_t += dt;
        uint32_t p = pressed & ~((1u << SRC_DUP) | (1u << SRC_DDOWN) | (1u << SRC_DLEFT) | (1u << SRC_DRIGHT));
        if (p) {
            int src = 0;
            while (!(p & (1u << src))) src++;
            bind_set_pad(g_capture, src);
            char b[64];
            snprintf(b, sizeof b, "Assigned to %s", pad_src_name(src));
            ui_toast(b);
            g_capture = -1;
            g_combo_latched = true;               // don't treat this press as anything else
        } else if (g_capture_t > 6.0f) {
            g_capture = -1;
            ui_toast("Remapping cancelled");
        }
        g_prev_held = held;
        return;
    }

    // Back + Start together opens or closes the menu
    if ((held & back_bit) && (held & start_bit) && (pressed & (back_bit | start_bit)) && !g_combo_latched) {
        menu_toggle(UI_DEV_PAD);
        g_combo_latched = true;
    }
    if (!(held & (back_bit | start_bit))) g_combo_latched = false;

    if (ui_menu_visible() && ui_menu_wants_pause() && !g_combo_latched) {
        if (pressed & (1u << SRC_A)) ui_key(UI_OK, UI_DEV_PAD);
        if (pressed & (1u << SRC_B)) ui_key(UI_BACK, UI_DEV_PAD);
        if (pressed & (1u << SRC_LB)) ui_key(UI_PREV_TAB, UI_DEV_PAD);
        if (pressed & (1u << SRC_RB)) ui_key(UI_NEXT_TAB, UI_DEV_PAD);
        if ((pressed & start_bit) && !(held & back_bit)) ui_key(UI_CLOSE, UI_DEV_PAD);
        // directions from the D-pad or the stick, with key repeat
        int dir = pad_stick_dir();
        if (held & (1u << SRC_DUP)) dir |= 1;
        if (held & (1u << SRC_DDOWN)) dir |= 2;
        if (held & (1u << SRC_DLEFT)) dir |= 4;
        if (held & (1u << SRC_DRIGHT)) dir |= 8;
        int fire = 0;
        if (dir != g_prev_dir) { fire = dir != 0; g_dir_timer = 0.38f; }
        else if (dir) { g_dir_timer -= dt; if (g_dir_timer <= 0) { fire = 1; g_dir_timer = 0.085f; } }
        if (fire) {
            if (dir & 1) ui_key(UI_UP, UI_DEV_PAD);
            else if (dir & 2) ui_key(UI_DOWN, UI_DEV_PAD);
            else if (dir & 4) ui_key(UI_LEFT, UI_DEV_PAD);
            else if (dir & 8) ui_key(UI_RIGHT, UI_DEV_PAD);
        }
        g_prev_dir = dir;
    } else {
        g_prev_dir = 0;
    }
    g_prev_held = held;
}

void menu_host_frame(float dt) {
    keyboard();
    controller(dt);
    ui_update(dt);
    float b = ui_backdrop();
    g_blit_dim = b;
    g_blit_blur = b;
}

// While the menu is open, and just after it closes (until the button or key that closed it is let go),
// keep controls away from the game.
int menu_host_input_blocked(void) {
    // Only briefly: a controller that always reports something held (a resting trigger, a fight stick's
    // switch, a second device) would otherwise leave the game ignoring every control after the menu.
    static bool blocked;
    static int frames;
    if (ui_menu_wants_pause()) { blocked = true; frames = 0; return 1; }
    if (!blocked) return 0;
    uint32_t held = pad_held_all();
    if (!held && !keys_bound_held()) blocked = false;
    else if (++frames > 30) {
        fe_log("menu: controls still held %08X after closing the menu; controls back anyway", (unsigned)held);
        blocked = false;
    }
    return blocked;
}

void menu_host_cancel_capture(void) { g_capture = -1; }
int menu_host_capturing(void) { return g_capture >= 0; }
void menu_host_apply(void) { apply(1); }
