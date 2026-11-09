// Host controls: keyboard keys and Xbox controllers (XInput) -> the arcade's per-player controls, with
// remappable bindings saved to controls.ini next to the exe.
//
// Controller handling adapted from ReLayer (port/pc/pad_map.c, pad_xinput.c), Copyright (c) 2026 The ReLayer
// project, MIT License (see LICENSE-ReLayer.txt in this folder). Changes for Buriki One: four buttons, a
// keyboard binding next to each controller binding, one mapping per player.
#include "fe_internal.h"
#include "frontend.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------- XInput
struct XPad { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XState { DWORD dwPacketNumber; XPad Gamepad; };
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XState*);

struct PadRaw { int connected; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly; };

static XInputGetStateFn g_get;
static PadRaw g_slot[4];
static int g_retry[4];          // frames until we look at an empty slot again (polling those is slow)
static bool g_pad_tried;

static const uint16_t k_xbits[SRC_COUNT] = {
    0, 0x0001, 0x0002, 0x0004, 0x0008,    // none, d-pad up down left right
    0x0010, 0x0020, 0x0040, 0x0080,       // start, back, ls, rs
    0x0100, 0x0200, 0x1000, 0x2000, 0x4000, 0x8000,   // lb rb a b x y
    0, 0,                                 // lt rt (analogue)
};

static const struct { const char* name; int src; } k_pad_names[] = {
    {"none", SRC_NONE}, {"dup", SRC_DUP}, {"ddown", SRC_DDOWN}, {"dleft", SRC_DLEFT}, {"dright", SRC_DRIGHT},
    {"start", SRC_START}, {"back", SRC_BACK}, {"view", SRC_BACK}, {"menu", SRC_START},
    {"ls", SRC_LS}, {"rs", SRC_RS}, {"lb", SRC_LB}, {"rb", SRC_RB},
    {"a", SRC_A}, {"b", SRC_B}, {"x", SRC_X}, {"y", SRC_Y}, {"lt", SRC_LT}, {"rt", SRC_RT},
};

void pad_init(void) {
    if (g_pad_tried) return;
    g_pad_tried = true;
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
        if (HMODULE h = LoadLibraryA(dll)) {
            g_get = (XInputGetStateFn)(void*)GetProcAddress(h, "XInputGetState");
            if (g_get) { fe_log("[input] gamepad support via %s", dll); return; }
        }
    }
    fe_log("[input] no XInput found; keyboard only");
}

void pad_poll(void) {
    pad_init();
    if (!g_get) return;
    for (int i = 0; i < 4; i++) {
        if (!g_slot[i].connected && g_retry[i] > 0) { g_retry[i]--; continue; }
        XState st = {};
        int was = g_slot[i].connected;
        if (g_get((DWORD)i, &st) == ERROR_SUCCESS) {
            g_slot[i].connected = 1;
            g_slot[i].buttons = st.Gamepad.wButtons;
            g_slot[i].lt = st.Gamepad.bLeftTrigger;
            g_slot[i].rt = st.Gamepad.bRightTrigger;
            g_slot[i].lx = st.Gamepad.sThumbLX;
            g_slot[i].ly = st.Gamepad.sThumbLY;
        } else {
            memset(&g_slot[i], 0, sizeof g_slot[i]);
            g_retry[i] = 120;     // look again in about two seconds
        }
        if (was != g_slot[i].connected)
            fe_log("[input] XInput slot %d %s", i, g_slot[i].connected ? "connected" : "disconnected");
    }
}

static int src_held(int src, const PadRaw* p) {
    if (src <= SRC_NONE || src >= SRC_COUNT) return 0;
    if (src == SRC_LT) return p->lt > 30;
    if (src == SRC_RT) return p->rt > 30;
    return (p->buttons & k_xbits[src]) != 0;
}

static uint32_t held_sources(const PadRaw* p) {
    uint32_t m = 0;
    if (!p->connected) return 0;
    for (int s = 1; s < SRC_COUNT; s++) if (src_held(s, p)) m |= 1u << s;
    return m;
}

// The first connected controller is player 1 and the second is player 2, whatever XInput slots they use.
static const PadRaw* player_pad(int player) {
    int n = 0;
    for (auto& s : g_slot) if (s.connected) { if (n == player) return &s; n++; }
    return nullptr;
}
int pad_player_connected(int player) { return player_pad(player) != nullptr; }
uint32_t pad_held_all(void) { uint32_t m = 0; for (auto& s : g_slot) m |= held_sources(&s); return m; }
int pad_stick_dir(void) {
    int d = 0;
    for (auto& s : g_slot) {
        if (!s.connected) continue;
        if (s.ly > 20000) d |= 1;
        if (s.ly < -20000) d |= 2;
        if (s.lx < -20000) d |= 4;
        if (s.lx > 20000) d |= 8;
    }
    return d;
}

const char* pad_src_name(int src) {
    static const char* n[SRC_COUNT] = {
        "\xE2\x80\x94", "D-UP", "D-DOWN", "D-LEFT", "D-RIGHT", "MENU", "VIEW", "LS", "RS",
        "LB", "RB", "A", "B", "X", "Y", "LT", "RT",
    };
    return src >= 0 && src < SRC_COUNT ? n[src] : "?";
}

// ---------------------------------------------------------------- keyboard key names
static const struct { int vk; const char* name; const char* shown; } k_keys[] = {
    {VK_UP, "up", "\xE2\x86\x91"}, {VK_DOWN, "down", "\xE2\x86\x93"}, {VK_LEFT, "left", "\xE2\x86\x90"},
    {VK_RIGHT, "right", "\xE2\x86\x92"}, {VK_SPACE, "space", "Space"}, {VK_RETURN, "enter", "Enter"},
    {VK_SHIFT, "shift", "Shift"}, {VK_CONTROL, "ctrl", "Ctrl"}, {VK_MENU, "alt", "Alt"},
    {VK_LSHIFT, "lshift", "L Shift"}, {VK_RSHIFT, "rshift", "R Shift"}, {VK_LCONTROL, "lctrl", "L Ctrl"},
    {VK_RCONTROL, "rctrl", "R Ctrl"}, {VK_BACK, "backspace", "Backspace"}, {VK_INSERT, "insert", "Ins"},
    {VK_DELETE, "delete", "Del"}, {VK_HOME, "home", "Home"}, {VK_END, "end", "End"}, {VK_PRIOR, "pageup", "PgUp"},
    {VK_NEXT, "pagedown", "PgDn"}, {VK_OEM_1, "semicolon", ";"}, {VK_OEM_2, "slash", "/"}, {VK_OEM_3, "backquote", "`"},
    {VK_OEM_4, "lbracket", "["}, {VK_OEM_5, "backslash", "\\"}, {VK_OEM_6, "rbracket", "]"}, {VK_OEM_7, "quote", "'"},
    {VK_OEM_COMMA, "comma", ","}, {VK_OEM_PERIOD, "period", "."}, {VK_OEM_MINUS, "minus", "-"}, {VK_OEM_PLUS, "equals", "="},
    {VK_NUMPAD0, "num0", "Num 0"}, {VK_NUMPAD1, "num1", "Num 1"}, {VK_NUMPAD2, "num2", "Num 2"}, {VK_NUMPAD3, "num3", "Num 3"},
    {VK_NUMPAD4, "num4", "Num 4"}, {VK_NUMPAD5, "num5", "Num 5"}, {VK_NUMPAD6, "num6", "Num 6"}, {VK_NUMPAD7, "num7", "Num 7"},
    {VK_NUMPAD8, "num8", "Num 8"}, {VK_NUMPAD9, "num9", "Num 9"}, {VK_ADD, "numplus", "Num +"}, {VK_SUBTRACT, "numminus", "Num -"},
    {VK_MULTIPLY, "numstar", "Num *"}, {VK_DIVIDE, "numslash", "Num /"}, {VK_DECIMAL, "numdot", "Num ."},
    {VK_F1, "f1", "F1"}, {VK_F3, "f3", "F3"}, {VK_F4, "f4", "F4"}, {VK_F5, "f5", "F5"}, {VK_F6, "f6", "F6"},
    {VK_F7, "f7", "F7"}, {VK_F8, "f8", "F8"}, {VK_F10, "f10", "F10"}, {VK_F12, "f12", "F12"},
};

static const char* key_name(int vk, bool shown) {
    static char buf[8][16];
    static int n;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        char* b = buf[n++ & 7];
        b[0] = (char)vk; b[1] = 0;
        return b;
    }
    for (auto& k : k_keys) if (k.vk == vk) return shown ? k.shown : k.name;
    if (vk == 0) return shown ? "\xE2\x80\x94" : "none";
    char* b = buf[n++ & 7];
    snprintf(b, 16, "vk%02X", vk);
    return b;
}

static int key_from_name(const char* s) {
    char low[32];
    size_t i = 0;
    for (; s[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)s[i]);
    low[i] = 0;
    if (i == 1 && (isalpha((unsigned char)low[0]) || isdigit((unsigned char)low[0]))) return toupper((unsigned char)low[0]);
    if (!strcmp(low, "none")) return 0;
    for (auto& k : k_keys) if (!strcmp(low, k.name)) return k.vk;
    unsigned v;
    if (sscanf(low, "vk%x", &v) == 1 && v > 0 && v < 256) return (int)v;
    return -1;
}

// Keys the front end keeps for itself: the menu, full screen, the Test / Service buttons.
static bool key_reserved(int vk) {
    return vk == VK_ESCAPE || vk == VK_TAB || vk == VK_F11 || vk == VK_F9 || vk == VK_F2 || vk == '9' ||
           vk == VK_LWIN || vk == VK_RWIN;
}

// ---------------------------------------------------------------- bindings
enum { MAP_B1, MAP_B2, MAP_B3, MAP_B4, MAP_START, MAP_COIN, MAP_COUNT };
struct Bindings {
    int key[2][FB_COUNT];
    uint8_t pad[2][MAP_COUNT];
    float deadzone;            // left stick, 0..1 of full deflection
};
static Bindings g_b;
static const char* k_fb_names[FB_COUNT] = {"up", "down", "left", "right", "b1", "b2", "b3", "b4", "start", "coin"};

static void defaults(Bindings* b) {
    static const int p1[FB_COUNT] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X', 'C', 'V', '1', '5'};
    static const int p2[FB_COUNT] = {'R', 'F', 'D', 'G', 'A', 'S', 'Q', 'W', '2', '6'};
    memcpy(b->key[0], p1, sizeof p1);
    memcpy(b->key[1], p2, sizeof p2);
    // Face buttons in arcade order: X Y on top = buttons 1 2, A B below = buttons 3 4.
    static const uint8_t pad[MAP_COUNT] = {SRC_X, SRC_Y, SRC_A, SRC_B, SRC_START, SRC_BACK};
    memcpy(b->pad[0], pad, sizeof pad);
    memcpy(b->pad[1], pad, sizeof pad);
    b->deadzone = 0.35f;
}

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// controls.ini:  p1_b1_key = Z   p1_b1_pad = X   ...   stick_deadzone = 0.35
static void load(Bindings* b, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* h = strchr(line, '#');
        if (h) *h = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line), *val = trim(eq + 1);
        if (!strcmp(key, "stick_deadzone")) {
            float dz = 0;
            if (sscanf(val, "%f", &dz) == 1 && dz > 0.05f && dz < 0.95f) b->deadzone = dz;
            continue;
        }
        int pl = 0;
        char what[16], dev[8];
        if (sscanf(key, "p%d_%15[a-z0-9]_%7s", &pl, what, dev) != 3 || pl < 1 || pl > 2) continue;
        int t = -1;
        for (int k = 0; k < FB_COUNT; k++) if (!strcmp(what, k_fb_names[k])) t = k;
        if (t < 0) continue;
        if (!strcmp(dev, "key")) {
            int vk = key_from_name(val);
            if (vk >= 0 && !key_reserved(vk)) b->key[pl - 1][t] = vk;
            else fe_log("[input] %s: unknown key '%s'", path, val);
        } else if (!strcmp(dev, "pad") && t >= FB_B1) {
            int s = -1;
            char low[16];
            size_t i = 0;
            for (; val[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)val[i]);
            low[i] = 0;
            for (auto& n : k_pad_names) if (!strcmp(low, n.name)) s = n.src;
            if (s >= 0) b->pad[pl - 1][t - FB_B1] = (uint8_t)s;
            else fe_log("[input] %s: unknown controller button '%s'", path, val);
        }
    }
    fclose(f);
}

static void save(const Bindings* b, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Buriki One controls (also editable in the in-game menu: Controls).\n"
               "# Keys: A-Z 0-9 up down left right space enter shift ctrl alt num0-num9 f1 ... or vkXX (hex)\n"
               "# Controller buttons: A B X Y LB RB LT RT START BACK LS RS DUP DDOWN DLEFT DRIGHT NONE\n"
               "# Fixed: Esc / Tab menu, F2 Test, 9 Service, F11 or Alt+Enter full screen.\n\n");
    for (int pl = 0; pl < 2; pl++) {
        for (int t = 0; t < FB_COUNT; t++) {
            fprintf(f, "p%d_%s_key = %s\n", pl + 1, k_fb_names[t], key_name(b->key[pl][t], false));
            if (t >= FB_B1) {
                int src = b->pad[pl][t - FB_B1];
                const char* n = "none";
                for (auto& k : k_pad_names) if (k.src == src) { n = k.name; break; }
                fprintf(f, "p%d_%s_pad = %s\n", pl + 1, k_fb_names[t], n);
            }
        }
        fprintf(f, "\n");
    }
    fprintf(f, "stick_deadzone = %.2f\n", b->deadzone);
    fclose(f);
}

void bind_defaults(void) { float dz = g_b.deadzone; defaults(&g_b); g_b.deadzone = dz; bind_save(); }
void bind_save(void) { if (!g_no_save) save(&g_b, fe_path("controls.ini").c_str()); }
int bind_deadzone_percent(void) { return (int)lroundf(g_b.deadzone * 100); }
void bind_set_deadzone_percent(int p) { g_b.deadzone = (float)p / 100.0f; bind_save(); }
static int action_fb(int action) { return FB_B1 + action % 6; }
const char* bind_pad_name(int action) { return pad_src_name(g_b.pad[action / 6 & 1][action % 6]); }
const char* bind_key_name(int action) { return key_name(g_b.key[action / 6 & 1][action_fb(action)], true); }
void bind_set_pad(int action, int src) { g_b.pad[action / 6 & 1][action % 6] = (uint8_t)src; bind_save(); }
int bind_set_key(int action, int vk) {
    if (key_reserved(vk) || vk <= 0 || vk > 255) return 0;
    g_b.key[action / 6 & 1][action_fb(action)] = vk;
    bind_save();
    return 1;
}

static bool key(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool keys_bound_held(void) {
    if (!video_focused()) return false;
    for (auto& pl : g_b.key) for (int vk : pl) if (key(vk)) return true;
    return false;
}

void fe_controls_load(void) {
    defaults(&g_b);
    load(&g_b, fe_path("controls.ini").c_str());
}

// ---------------------------------------------------------------- what the game sees
static bool our_process_focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static void pad_bits(const PadRaw* p, int pl, bool* b) {
    if (!p) return;
    b[FB_UP] |= (p->buttons & 0x0001) != 0;
    b[FB_DOWN] |= (p->buttons & 0x0002) != 0;
    b[FB_LEFT] |= (p->buttons & 0x0004) != 0;
    b[FB_RIGHT] |= (p->buttons & 0x0008) != 0;
    // left stick: 8 sectors of 45 degrees, so diagonals are as easy as straight directions
    float x = p->lx / 32767.0f, y = p->ly / 32767.0f;
    if (sqrtf(x * x + y * y) > g_b.deadzone) {
        float a = atan2f(y, x) * (180.0f / 3.14159265f);   // 0 = right, 90 = up
        int sector = (int)floorf((a + 22.5f) / 45.0f);
        sector = ((sector % 8) + 8) % 8;
        static const int8_t dx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        if (dx[sector] > 0) b[FB_RIGHT] = true;
        if (dx[sector] < 0) b[FB_LEFT] = true;
        if (dy[sector] > 0) b[FB_UP] = true;
        if (dy[sector] < 0) b[FB_DOWN] = true;
    }
    for (int t = 0; t < MAP_COUNT; t++) if (src_held(g_b.pad[pl][t], p)) b[FB_B1 + t] = true;
}

void fe_read_host(bool p[2][10], bool* service, bool* test) {
    if (!fe_window_active()) pad_poll();       // headless: nobody else polls the controllers
    *service = *test = false;
    bool blocked = fe_window_active() && menu_host_input_blocked();
    if (!blocked) {
        bool focused = fe_window_active() ? video_focused() : our_process_focused();
        if (focused) {
            for (int pl = 0; pl < 2; pl++)
                for (int i = 0; i < FB_COUNT; i++) p[pl][i] = key(g_b.key[pl][i]);
            *service = key('9');
            *test = key(VK_F2);
        }
        for (int pl = 0; pl < 2; pl++) pad_bits(player_pad(pl), pl, p[pl]);
    }
    if (g_test_pulse > 0) { g_test_pulse--; *test = true; }
}
