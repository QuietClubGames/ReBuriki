// Player controls: the host's keyboard and controllers (read and mapped by the front end, see
// frontend/fe_input.cpp and controls.ini) or an input script, combined into the active-low port bytes the
// I/O board firmware reads.
#include "input.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include "hw.h"
#include "frontend/frontend.h"

namespace {

InputFrame g_frame = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

enum Btn { UP, DOWN, LEFT, RIGHT, B1, B2, B3, B4, START, COIN, NUM_BTN };


// Coins are pulses on real cabinets (MAME PORT_IMPULSE): report a press for a few frames
// on the key-down edge only, so holding the key doesn't insert many coins.
struct CoinPulse { bool was_down = false; int frames_left = 0; };
CoinPulse g_coin[2];
bool coin_pulse(CoinPulse& c, bool down) {
    if (down && !c.was_down) c.frames_left = 3;
    c.was_down = down;
    if (c.frames_left > 0) { c.frames_left--; return true; }
    return false;
}

// ---- scripted input (for automated tests) ----
// "--input-script 600=coin1;610=;700=p1start;710=;900=p1right,p1b1"
// From each frame number on, exactly the listed controls are held (empty = nothing).
struct ScriptStep { uint32_t frame; std::vector<std::string> held; };
std::vector<ScriptStep> g_script;
bool g_use_script = false;

bool script_held(const char* name) {
    const uint32_t f = hw_frame_number();
    const ScriptStep* cur = nullptr;
    for (auto& s : g_script) if (s.frame <= f) cur = &s;
    if (!cur) return false;
    for (auto& h : cur->held) if (h == name) return true;
    return false;
}

} // namespace

void input_set_script(const char* script) {
    g_use_script = true;
    std::string s = script;
    if (!s.empty() && s[0] == '@') {   // "@file": read the script from a file (long scripts)
        FILE* f = fopen(s.c_str() + 1, "rb");
        if (!f) { fprintf(stderr, "[input] can't read script file %s\n", s.c_str() + 1); s.clear(); }
        else {
            s.clear();
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
            fclose(f);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        }
    }
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        std::string step = s.substr(pos, end - pos);
        size_t eq = step.find('=');
        if (eq != std::string::npos) {
            ScriptStep st{(uint32_t)atoi(step.substr(0, eq).c_str()), {}};
            std::string names = step.substr(eq + 1);
            size_t p = 0;
            while (p < names.size()) {
                size_t c = names.find(',', p);
                if (c == std::string::npos) c = names.size();
                if (c > p) st.held.push_back(names.substr(p, c - p));
                p = c + 1;
            }
            g_script.push_back(st);
        }
        pos = end + 1;
    }
    fprintf(stderr, "[input] using an input script with %zu steps (host input ignored)\n", g_script.size());
}

void input_sample_frame(void) {
    bool p[2][NUM_BTN] = {};
    bool service = false, test = false;
    if (g_use_script) {
        const char* names[NUM_BTN] = {"up", "down", "left", "right", "b1", "b2", "b3", "b4", "start", "coin"};
        for (int pl = 0; pl < 2; pl++)
            for (int i = 0; i < NUM_BTN; i++) {
                char n[16];
                snprintf(n, sizeof(n), "p%d%s", pl + 1, names[i]);
                p[pl][i] = script_held(n);
            }
        if (script_held("coin1")) p[0][COIN] = true;
        if (script_held("coin2")) p[1][COIN] = true;
        service = script_held("service");
        test = script_held("test");
    }
    if (!g_use_script) {
        fe_read_host(p, &service, &test);
    } else {
        bool hp[2][NUM_BTN] = {}, hs = false, ht = false;
        fe_read_host(hp, &hs, &ht);          // host controls are ignored, but the menu's Test-button pulse isn't
        test |= ht;
    }
    // Opposite directions can't both be pressed on a real stick.
    for (auto& pl : p) {
        if (pl[UP] && pl[DOWN]) pl[UP] = pl[DOWN] = false;
        if (pl[LEFT] && pl[RIGHT]) pl[LEFT] = pl[RIGHT] = false;
    }
    const bool coin1 = coin_pulse(g_coin[0], p[0][COIN]);
    const bool coin2 = coin_pulse(g_coin[1], p[1][COIN]);

    auto bits = [](std::initializer_list<std::pair<bool, uint8_t>> l) {
        uint8_t v = 0xFF;
        for (auto& [on, m] : l) if (on) v &= (uint8_t)~m;
        return v;
    };
    InputFrame f;
    f.port[0] = f.port[1] = f.port[2] = f.port[3] = 0xFF;
    f.port[4] = bits({{p[0][UP], 0x01}, {p[0][DOWN], 0x02}, {p[0][LEFT], 0x04}, {p[0][RIGHT], 0x08},
                      {p[0][B1], 0x10}, {p[0][B2], 0x20}, {p[0][B3], 0x40}, {p[0][B4], 0x80}});
    f.port[5] = bits({{p[1][UP], 0x02}, {p[1][DOWN], 0x04}, {p[1][LEFT], 0x08}, {p[1][RIGHT], 0x10},
                      {p[1][B1], 0x20}, {p[1][B2], 0x40}, {p[1][B3], 0x80}});
    f.port[6] = bits({{p[1][B4], 0x01}});
    f.port[7] = bits({{service, 0x01}, {test, 0x02}, {coin1, 0x04}, {coin2, 0x08},
                      {p[0][START], 0x40}, {p[1][START], 0x80}});
    g_frame = f;
}

const InputFrame& input_current(void) { return g_frame; }

void input_print_help(void) {
    fprintf(stderr,
        "[input] Player 1: arrows + Z X C V, start 1, coin 5 | Player 2: R F D G + A S Q W, start 2, coin 6\n"
        "[input] Service 9, Test F2, Tab / Esc menu | Gamepads: stick/d-pad, X Y A B = buttons 1-4, Start, Back = coin,\n"
        "[input] Back + Start menu (defaults; remap in the menu's Controls page or controls.ini)\n");
}
