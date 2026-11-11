// Host controls: keyboard keys and Xbox controllers (XInput) -> the arcade's per-player controls, with
// remappable bindings saved to controls.ini next to the exe.
//
// Controller handling adapted from ReLayer (port/pc/pad_map.c, pad_xinput.c), Copyright (c) 2026 The ReLayer
// project, MIT License (see LICENSE-ReLayer.txt in this folder). Changes for Buriki One: four buttons, a
// keyboard binding next to each controller binding, one mapping per player, several controller buttons per
// control, a choice of which stick is the arcade stick.
#include "fe_internal.h"
#include "frontend.h"
#include "../hw.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------- XInput
struct XPad { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XState { DWORD dwPacketNumber; XPad Gamepad; };
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XState*);

struct PadRaw { int connected; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };

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
            g_slot[i].rx = st.Gamepad.sThumbRX;
            g_slot[i].ry = st.Gamepad.sThumbRY;
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
// Each of a player's controls (FB_*) has one keyboard key and any number of controller buttons (a set of
// SRC_* bits; the menu allows up to MAX_PAD_PER). The same key or button may be on several controls. The
// sticks are separate: `stick` says which of them works as the arcade stick.
enum { STICK_LEFT, STICK_RIGHT, STICK_EITHER };
enum { MAX_PAD_PER = 3 };
struct Bindings {
    int key[2][FB_COUNT];
    uint32_t pad[2][FB_COUNT];
    int stick[2];              // STICK_*
    float deadzone;            // sticks, 0..1 of full deflection
};
static Bindings g_b;
static const char* k_fb_names[FB_COUNT] = {"up", "down", "left", "right", "b1", "b2", "b3", "b4", "start", "coin",
                                           "select", "other"};

static void defaults(Bindings* b) {
    // (keyboard: the same two keys step in a fight and confirm on the other screens, as on the cabinet)
    static const int p1[FB_COUNT] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X', 'C', 'V', '1', '5', 'Z', 'X'};
    static const int p2[FB_COUNT] = {'R', 'F', 'D', 'G', 'A', 'S', 'Q', 'W', '2', '6', 'A', 'S'};
    memcpy(b->key[0], p1, sizeof p1);
    memcpy(b->key[1], p2, sizeof p2);
    // The game only uses two buttons, which it calls L and R. In a fight they step left and right: the
    // left and right triggers and bumpers. On the game's other screens they confirm: A and B. X and Y are
    // the board's spare buttons 3 and 4. The D-pad and either stick attack.
#define S(x) (1u << (x))
    static const uint32_t pad[FB_COUNT] = {
        S(SRC_DUP), S(SRC_DDOWN), S(SRC_DLEFT), S(SRC_DRIGHT),
        S(SRC_LT) | S(SRC_LB), S(SRC_RT) | S(SRC_RB), S(SRC_X), S(SRC_Y),
        S(SRC_START), S(SRC_BACK), S(SRC_A), S(SRC_B)};
#undef S
    memcpy(b->pad[0], pad, sizeof pad);
    memcpy(b->pad[1], pad, sizeof pad);
    b->stick[0] = b->stick[1] = STICK_EITHER;
    b->deadzone = 0.35f;
}

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static const char* const k_stick_names[] = {"left", "right", "either"};

// controls.ini:  p1_b1_key = Z   p1_b1_pad = LT LB A   p1_attack_stick = either   ...   stick_deadzone = 0.35
static void load(Bindings* b, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    bool seen_stick[2] = {false, false}, seen_select[2] = {false, false};
    bool seen_key[2][FB_COUNT] = {};
    const Bindings fresh = *b;
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
        if (!strcmp(what, "attack") && !strcmp(dev, "stick")) {
            for (int i = 0; i < 3; i++) if (!_stricmp(val, k_stick_names[i])) { b->stick[pl - 1] = i; seen_stick[pl - 1] = true; }
            continue;
        }
        int t = -1;
        for (int k = 0; k < FB_COUNT; k++) if (!strcmp(what, k_fb_names[k])) t = k;
        if (t < 0) continue;
        if (t == FB_SELECT) seen_select[pl - 1] = true;
        if (!strcmp(dev, "key")) {
            int vk = key_from_name(val);
            if (vk >= 0 && !key_reserved(vk)) { b->key[pl - 1][t] = vk; seen_key[pl - 1][t] = true; }
            else fe_log("[input] %s: unknown key '%s'", path, val);
        } else if (!strcmp(dev, "pad")) {
            // a list of controller buttons: "LT LB A" (or "none")
            uint32_t mask = 0;
            bool ok = true;
            for (char* tok = strtok(val, " ,\t"); tok; tok = strtok(nullptr, " ,\t")) {
                int s = -1;
                for (auto& n : k_pad_names) if (!_stricmp(tok, n.name)) s = n.src;
                if (s < 0) { ok = false; fe_log("[input] %s: unknown controller button '%s'", path, tok); }
                else if (s != SRC_NONE) mask |= 1u << s;
            }
            if (ok) b->pad[pl - 1][t] = mask;
        }
    }
    fclose(f);
    // A file from before the controller layout changed (no attack_stick line) that still has that version's
    // untouched buttons (X Y A B, Start, Back) gets the new layout. Anything the player changed is kept.
    static const int old_pad[6] = {SRC_X, SRC_Y, SRC_A, SRC_B, SRC_START, SRC_BACK};
    for (int pl = 0; pl < 2; pl++) {
        if (seen_stick[pl]) continue;
        bool old = true;
        for (int i = 0; i < 6; i++) old &= b->pad[pl][FB_B1 + i] == 1u << old_pad[i];
        if (old) memcpy(b->pad[pl], fresh.pad[pl], sizeof b->pad[pl]);
    }
    // A file from before Select and Other existed. Keys: they follow the L and R keys, so the keyboard works
    // as it did. Controller: if L and R still have that version's untouched buttons (LT LB A, RT RB B), take
    // A and B off them, since those are Select and Other now.
    for (int pl = 0; pl < 2; pl++) {
        if (seen_select[pl]) continue;
        if (!seen_key[pl][FB_SELECT]) b->key[pl][FB_SELECT] = b->key[pl][FB_B1];
        if (!seen_key[pl][FB_OTHER]) b->key[pl][FB_OTHER] = b->key[pl][FB_B2];
        const uint32_t l3 = 1u << SRC_LT | 1u << SRC_LB | 1u << SRC_A, r3 = 1u << SRC_RT | 1u << SRC_RB | 1u << SRC_B;
        if (b->pad[pl][FB_B1] == l3 && b->pad[pl][FB_B2] == r3) {
            b->pad[pl][FB_B1] = fresh.pad[pl][FB_B1];
            b->pad[pl][FB_B2] = fresh.pad[pl][FB_B2];
        }
    }
}

// The names of the controller buttons in a set, in a fixed order (the order the menu shows them in).
static int pad_list(uint32_t mask, int* out) {
    static const int order[] = {SRC_LT, SRC_LB, SRC_RT, SRC_RB, SRC_A, SRC_B, SRC_X, SRC_Y, SRC_DUP, SRC_DDOWN,
                                SRC_DLEFT, SRC_DRIGHT, SRC_START, SRC_BACK, SRC_LS, SRC_RS};
    int n = 0;
    for (int s : order) if (mask & (1u << s)) out[n++] = s;
    return n;
}

static void save(const Bindings* b, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Buriki One controls (also editable in the in-game menu: Controls).\n"
               "# Keys: A-Z 0-9 up down left right space enter shift ctrl alt num0-num9 f1 ... or vkXX (hex)\n"
               "# Controller buttons: A B X Y LB RB LT RT START BACK LS RS DUP DDOWN DLEFT DRIGHT NONE;\n"
               "#   several can share one control: p1_b1_pad = LT LB. A key or button can be on several controls.\n"
               "# b1 and b2 are the game's L and R buttons in a fight (step left, step right); select and other are\n"
               "#   the same two buttons on the game's other screens (confirm).\n"
               "# attack_stick: which stick is the arcade stick: left, right or either.\n"
               "# Fixed: Esc / Tab menu, F2 Test, 9 Service, F11 or Alt+Enter full screen.\n\n");
    for (int pl = 0; pl < 2; pl++) {
        for (int t = 0; t < FB_COUNT; t++) {
            fprintf(f, "p%d_%s_key = %s\n", pl + 1, k_fb_names[t], key_name(b->key[pl][t], false));
            int list[SRC_COUNT];
            const int n = pad_list(b->pad[pl][t], list);
            fprintf(f, "p%d_%s_pad =", pl + 1, k_fb_names[t]);
            for (int i = 0; i < n; i++)
                for (auto& k : k_pad_names) if (k.src == list[i]) { fprintf(f, " %s", k.name); break; }
            fprintf(f, n ? "\n" : " none\n");
        }
        fprintf(f, "p%d_attack_stick = %s\n\n", pl + 1, k_stick_names[b->stick[pl] % 3]);
    }
    fprintf(f, "stick_deadzone = %.2f\n", b->deadzone);
    fclose(f);
}

void bind_defaults(void) { float dz = g_b.deadzone; defaults(&g_b); g_b.deadzone = dz; bind_save(); }
void bind_save(void) { if (!g_no_save) save(&g_b, fe_path("controls.ini").c_str()); }
int bind_deadzone_percent(void) { return (int)lroundf(g_b.deadzone * 100); }
void bind_set_deadzone_percent(int p) { g_b.deadzone = (float)p / 100.0f; bind_save(); }
// Remappable actions: player * 16 + FB_*.
static int action_pl(int action) { return action / 16 & 1; }
static int action_fb(int action) { const int t = action % 16; return t < FB_COUNT ? t : 0; }
int bind_pad_count(int action) {
    int list[SRC_COUNT];
    return pad_list(g_b.pad[action_pl(action)][action_fb(action)], list);
}
const char* bind_pad_at(int action, int i) {
    int list[SRC_COUNT];
    const int n = pad_list(g_b.pad[action_pl(action)][action_fb(action)], list);
    return i >= 0 && i < n ? pad_src_name(list[i]) : pad_src_name(SRC_NONE);
}
const char* bind_pad_name(int action) { return bind_pad_at(action, 0); }
const char* bind_key_name(int action) { return key_name(g_b.key[action_pl(action)][action_fb(action)], true); }
// A controller button pressed while remapping: one that is already on this control comes off it; another is
// added. 1 added, 0 taken off, -1 the control is full.
int bind_toggle_pad(int action, int src) {
    uint32_t* row = g_b.pad[action_pl(action)];
    const uint32_t bit = 1u << src;
    const int t = action_fb(action);
    if (row[t] & bit) { row[t] &= ~bit; bind_save(); return 0; }
    int list[SRC_COUNT];
    if (pad_list(row[t], list) >= MAX_PAD_PER) return -1;
    row[t] |= bit;
    bind_save();
    return 1;
}
void bind_clear_pad(int action) { g_b.pad[action_pl(action)][action_fb(action)] = 0; bind_save(); }
int bind_set_key(int action, int vk) {
    if (key_reserved(vk) || vk <= 0 || vk > 255) return 0;
    g_b.key[action_pl(action)][action_fb(action)] = vk;
    bind_save();
    return 1;
}
int bind_stick(int player) { return g_b.stick[player & 1]; }
void bind_set_stick(int player, int v) { g_b.stick[player & 1] = v < 0 || v > 2 ? STICK_EITHER : v; bind_save(); }

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

// An analogue stick as the arcade's 8-way stick: 8 sectors of 45 degrees, so diagonals are as easy as
// straight directions.
static void stick_bits(int16_t sx, int16_t sy, bool* b) {
    float x = sx / 32767.0f, y = sy / 32767.0f;
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
}

// Tests: BURIKI_TEST_HOLD="1600-1606:select,2500-2530:b1,..." holds player 1's named controls (the names
// controls.ini uses) during those frames, as if a key bound to them were down. Unlike an --input-script,
// which names the arcade's buttons directly, this goes through the same step that turns the player's
// controls into the arcade's (the fight / other screens split).
static void test_hold(bool* raw) {
    static const char* spec = getenv("BURIKI_TEST_HOLD");
    if (!spec) return;
    const unsigned frame = hw_frame_number();
    for (const char* p = spec; *p;) {
        unsigned f0 = 0, f1 = 0;
        char name[16] = {0};
        if (sscanf(p, "%u-%u:%15[a-z0-9]", &f0, &f1, name) == 3 && frame >= f0 && frame < f1)
            for (int i = 0; i < FB_COUNT; i++) if (!strcmp(name, k_fb_names[i])) raw[i] = true;
        const char* c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
    }
}

static void pad_bits(const PadRaw* p, int pl, bool* b) {
    if (!p) return;
    const uint32_t held = held_sources(p);
    for (int t = 0; t < FB_COUNT; t++) if (held & g_b.pad[pl][t]) b[t] = true;
    if (g_b.stick[pl] != STICK_RIGHT) stick_bits(p->lx, p->ly, b);
    if (g_b.stick[pl] != STICK_LEFT) stick_bits(p->rx, p->ry, b);
}

void fe_read_host(bool p[2][10], bool* service, bool* test) {
    if (!fe_window_active()) pad_poll();       // headless: nobody else polls the controllers
    *service = *test = false;
    bool blocked = fe_window_active() && menu_host_input_blocked();
    if (!blocked) {
        bool raw[2][FB_COUNT] = {};
        bool focused = fe_window_active() ? video_focused() : our_process_focused();
        if (focused) {
            for (int pl = 0; pl < 2; pl++)
                for (int i = 0; i < FB_COUNT; i++) raw[pl][i] = key(g_b.key[pl][i]);
            *service = key('9');
            *test = key(VK_F2);
        }
        for (int pl = 0; pl < 2; pl++) pad_bits(player_pad(pl), pl, raw[pl]);
        test_hold(raw[0]);
        // The game's L and R buttons: in a fight they are the two step controls, and on every other screen
        // they are Select and Other. (So a trigger doesn't pick a fighter, and A doesn't step.)
        const bool fight = hw_fight_in_progress();
        for (int pl = 0; pl < 2; pl++) {
            for (int i = 0; i < FB_ARCADE; i++) p[pl][i] = raw[pl][i];
            p[pl][FB_B1] = raw[pl][fight ? FB_B1 : FB_SELECT];
            p[pl][FB_B2] = raw[pl][fight ? FB_B2 : FB_OTHER];
        }
    }
    if (g_test_pulse > 0) { g_test_pulse--; *test = true; }
}
