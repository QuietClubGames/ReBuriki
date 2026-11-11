// Pause menu, toasts and frame-rate counter (see ui.h).
//
// Adapted from ReLayer (port/ui/menu.c), Copyright (c) 2026 The ReLayer project, MIT License
// (see LICENSE-ReLayer.txt in this folder). Changes for Buriki One: its own pages (region, key + pad
// remapping per player, frame pacing), no PS1 texture option, plain panel when there is no menu art.
#include "ui.h"
#include "settings.h"
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------- palette
#define COL_GOLD        0xE8B64CFFu
#define COL_TEXT        0xF2F3F7FFu
#define COL_TEXT_DIM    0x9AA0AEFFu
#define COL_TEXT_OFF    0x5C6170FFu
#define COL_PANEL_TOP   0x1B1D27F2u
#define COL_PANEL_BOT   0x0E0F15F4u
#define COL_LINE        0xFFFFFF16u
#define COL_RED         0xE5675FFFu

static uint32_t fade(uint32_t c, float a) {
    uint32_t al = (uint32_t)((float)(c & 0xFF) * (a < 0 ? 0 : a > 1 ? 1 : a) + 0.5f);
    return (c & 0xFFFFFF00u) | al;
}
static uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = (float)((a >> s) & 0xFF), y = (float)((b >> s) & 0xFF);
        r |= (uint32_t)(x + (y - x) * t + 0.5f) << s;
    }
    return r;
}
static float ease_out(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return 1 - (1 - t) * (1 - t) * (1 - t); }
static float approach(float cur, float target, float dt, float speed) {
    float k = 1 - expf(-speed * dt);
    return cur + (target - cur) * k;
}

// ---------------------------------------------------------------- menu model
enum { IT_CHOICE, IT_TOGGLE, IT_SLIDER, IT_ACTION, IT_INFO, IT_BINDING, IT_TEXT };

struct Item {
    int type;
    const char* label;
    const char* help;
    int (*get)(void);
    void (*set)(int);
    const char* const* choices;
    int nchoices;
    int lo, hi, step;            // slider
    void (*action)(void);
    int binding;                 // which control (FB_* of fe_internal.h: 0-3 stick, 4 5 L R, 6 7 buttons 3 4, 8 start, 9 coin, 10 select, 11 other, 12 guard)
    int (*enabled)(void);
    const char* (*info)(void);
    int (*dyn_count)(void);      // choices that depend on the PC (monitors, graphics cards)
    const char* (*dyn_name)(int);
    const char* (*text_get)(void);   // IT_TEXT: a line the player types
    void (*text_set)(const char*);
    int (*visible)(void);            // rows that are only there some of the time (the Controls page's three views)
};

static const UiHost* H;

// settings accessors
#define ACCESS(name, field) \
    static int get_##name(void) { return g_settings.field; } \
    static void set_##name(int v) { g_settings.field = v; if (H) H->settings_changed(); }
ACCESS(display_mode, display_mode)
ACCESS(window_size, window_size)
ACCESS(monitor, monitor)
ACCESS(aspect, aspect)
ACCESS(hud_edges, hud_edges)
static int widescreen_on(void) { return g_settings.aspect == ASPECT_WIDE; }
ACCESS(quality, quality)
ACCESS(filter, filter)
ACCESS(pacing, pacing)
ACCESS(gpu, gpu)
ACCESS(fps, show_fps)
ACCESS(volume, volume)
ACCESS(mute, mute)
static int get_graphics(void) { return g_settings.enhanced ? 0 : 1; }
static void set_graphics(int v) { g_settings.enhanced = v == 0; H->settings_changed(); }
static int enhanced_on(void) { return g_settings.enhanced; }
static int windowed(void) { return g_settings.display_mode == DISPLAY_WINDOW; }
static int get_deadzone(void) { return H->get_deadzone(); }
static void set_deadzone(int v) { H->set_deadzone(v); }

static int g_ctl_player;        // Controls page: which player's buttons are shown
static int get_player(void) { return g_ctl_player; }
static void set_player(int v) { g_ctl_player = v; }
static int get_lstick(void) { return H->get_stick(g_ctl_player * 2); }
static void set_lstick(int v) { H->set_stick(g_ctl_player * 2, v); }
static int get_rstick(void) { return H->get_stick(g_ctl_player * 2 + 1); }
static void set_rstick(int v) { H->set_stick(g_ctl_player * 2 + 1, v); }
static int get_scheme(void) { return H->get_scheme(g_ctl_player); }
static void set_scheme(int v) { H->set_scheme(g_ctl_player, v); }
// The Controls page shows one group of controls at a time, so that it is never a long list.
static int g_ctl_view;
static int get_view(void) { return g_ctl_view; }
static void set_view(int v) { g_ctl_view = v; }
static int view_fight(void) { return g_ctl_view == 0; }
static int view_screens(void) { return g_ctl_view == 1; }
static int view_sticks(void) { return g_ctl_view == 2; }

ACCESS(unlock_silber, unlock_silber)

// Online play. While an online game runs, everything that changes how the game itself behaves is the host's
// and fixed for the session, so those rows are switched off.
ACCESS(online_delay, online_delay)
static int online(void) { return H->online_active(); }
static int offline(void) { return !H->online_active(); }
static int can_join(void) { return offline() && g_settings.online_address[0]; }
static const char* get_address(void) { return g_settings.online_address; }
static void set_address(const char* s) { settings_set_address(&g_settings, s); H->settings_changed(); }
static const char* info_online(void) { return H->online_status(); }
static const char* info_address(void) { return H->local_address(); }
static void act_host(void) { H->online_host(); }
static void act_join(void) { H->online_join(); }
static void act_leave(void) { H->online_leave(); }

// The arcade's own operator settings (hw.cpp: hw_game_option_*), shown as rows on the Game and Rules pages.
// They live in the arcade's save memory, not settings.ini, so the Test menu and these rows always agree.
// (declared here as in ../hw.h, which this platform-independent file doesn't include)
enum { GOPT_FREE_PLAY, GOPT_DIFFICULTY, GOPT_DEMO_SOUND, GOPT_COMMAND_DISPLAY, GOPT_ATTACK_CPU, GOPT_ATTACK_VS,
       GOPT_TIME_CPU, GOPT_TIME_VS, GOPT_REPLAY, GOPT_CONTINUE, GOPT_BLOOD };
extern "C" {
bool hw_game_options_ready(void);
int  hw_game_option_get(int option);
void hw_game_option_set(int option, int value);
}
static int rules_ready(void) { return hw_game_options_ready() && offline(); }
// `first` = the game's number for the first choice (0 for off / on, 1 for easy / light / slow)
#define GAME_OPTION(name, id, first) \
    static int get_##name(void) { int v = hw_game_option_get(id); return v < first ? 0 : v - first; } \
    static void set_##name(int v) { hw_game_option_set(id, v + first); }
GAME_OPTION(free_play, GOPT_FREE_PLAY, 0)
GAME_OPTION(difficulty, GOPT_DIFFICULTY, 1)
GAME_OPTION(demo_sound, GOPT_DEMO_SOUND, 0)
GAME_OPTION(command_display, GOPT_COMMAND_DISPLAY, 0)
GAME_OPTION(attack_cpu, GOPT_ATTACK_CPU, 1)
GAME_OPTION(attack_vs, GOPT_ATTACK_VS, 1)
GAME_OPTION(time_cpu, GOPT_TIME_CPU, 1)
GAME_OPTION(time_vs, GOPT_TIME_VS, 1)
GAME_OPTION(replay, GOPT_REPLAY, 0)
GAME_OPTION(continue_, GOPT_CONTINUE, 0)
GAME_OPTION(blood, GOPT_BLOOD, 0)
static int get_region(void) { return g_settings.region; }
static void set_region(int v) {
    g_settings.region = v;
    H->settings_changed();
    if (v != H->running_region()) ui_toast("The new region applies after a restart");
}
static int region_pending(void) { return g_settings.region != H->running_region() && offline(); }

static const char* const k_mode[] = {"Window", "Borderless full screen", "Exclusive full screen"};
// window sizes: the list is in fe_window.cpp (widescreen makes each one 16:9 at the same height)
void video_window_size(int index, int* w, int* h);
static int count_winsizes(void) { return 6; }
static const char* name_winsize(int i) {
    static char names[6][48];
    int w = 0, h = 0;
    if (i < 0 || i >= 6) return "?";
    video_window_size(i, &w, &h);
    snprintf(names[i], sizeof names[i], "%d\xC3\x97%d%s", w, h, h == 896 ? "  (2\xC3\x97 lines)" : h == 1344 ? "  (3\xC3\x97 lines)" : "");
    return names[i];
}
static const char* const k_aspect[] = {"Arcade 4:3", "Stretch to fill", "4:3, whole-number scale", "Widescreen"};
static const char* const k_hud[] = {"Arcade position", "Screen edges"};
static const char* const k_graphics[] = {"Enhanced", "Original arcade"};
static const char* const k_quality[] = {"Auto (keeps 60 fps)", "Performance  2\xC3\x97", "Balanced  3\xC3\x97",
                                        "High  4\xC3\x97", "Ultra  6\xC3\x97", "Extreme  8\xC3\x97"};
static const char* const k_filter[] = {"Sharp", "Smooth", "Scanlines"};
static const char* const k_pacing[] = {"Arcade speed  59.43 Hz", "Display vsync"};
static const char* const k_player[] = {"Player 1", "Player 2"};
static const char* const k_role[] = {"Steps left and right", "Attacks", "Off"};
static const char* const k_scheme[] = {"Modern", "Classic", "Buttons attack", "Custom"};
static const char* const k_view[] = {"In a fight", "The game's screens, and the cabinet", "Sticks, and starting over"};
static const char* const k_offon[] = {"Off", "On"};
static const char* const k_difficulty[] = {"Easy", "Normal", "Hard", "Hardest"};
static const char* const k_attack[] = {"Light", "Normal", "Heavy"};
static const char* const k_time[] = {"Slow", "Normal", "Quick"};
static const char* const k_region[] = {"Japan  (Japanese)", "USA  (English)", "World  (English)"};
static const char* const k_delay[] = {"None", "1 frame", "2 frames", "3 frames", "4 frames", "5 frames", "6 frames"};

static void act_resume(void);
static void act_operator(void) { H->operator_menu(); }
static void act_restart(void) { H->restart(); }
static int g_quit_armed;
static void act_quit(void) {
    if (g_quit_armed) H->quit();
    else g_quit_armed = 1;
}
static void act_reset_binds(void) { H->reset_bindings(); ui_toast("Keys and buttons restored to the defaults"); }
static const char* info_controllers(void) { return ""; }    // drawn as two status badges
static const char* info_save(void) { return H->save_memory_info(); }

// lists that depend on the PC (monitors, graphics cards)
static int count_monitors(void) { return 1 + H->monitor_count(); }
static const char* name_monitor(int i) { return i == 0 ? "Automatic" : H->monitor_name(i - 1); }
static int count_gpus(void) { return 1 + H->gpu_count(); }
static const char* name_gpu(int i) { return i == 0 ? "Automatic (fastest)" : H->gpu_name(i - 1); }

#define NCH(arr) (int)(sizeof arr / sizeof arr[0])
#define CHOICE(lbl, hlp, g, s, arr, en) {IT_CHOICE, lbl, hlp, g, s, arr, NCH(arr), 0, 0, 0, nullptr, 0, en, nullptr, nullptr, nullptr}
#define DCHOICE(lbl, hlp, g, s, cnt, nm, en) {IT_CHOICE, lbl, hlp, g, s, nullptr, 0, 0, 0, 0, nullptr, 0, en, nullptr, cnt, nm}
#define TOGGLE(lbl, hlp, g, s) {IT_TOGGLE, lbl, hlp, g, s, nullptr, 0, 0, 1, 1, nullptr, 0, nullptr, nullptr, nullptr, nullptr}
#define TOGGLE_EN(lbl, hlp, g, s, en) {IT_TOGGLE, lbl, hlp, g, s, nullptr, 0, 0, 1, 1, nullptr, 0, en, nullptr, nullptr, nullptr}
#define TEXT(lbl, hlp, g, s, en) {IT_TEXT, lbl, hlp, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0, en, nullptr, nullptr, nullptr, g, s}
#define SLIDER(lbl, hlp, g, s, lo, hi, st) {IT_SLIDER, lbl, hlp, g, s, nullptr, 0, lo, hi, st, nullptr, 0, nullptr, nullptr, nullptr, nullptr}
#define ACTION(lbl, hlp, fn, en) {IT_ACTION, lbl, hlp, nullptr, nullptr, nullptr, 0, 0, 0, 0, fn, 0, en, nullptr, nullptr, nullptr}
#define BIND(lbl, hlp, n) {IT_BINDING, lbl, hlp, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, n, nullptr, nullptr, nullptr, nullptr}
#define INFO(lbl, hlp, fn) {IT_INFO, lbl, hlp, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, 0, nullptr, fn, nullptr, nullptr}

#define BINDV(lbl, hlp, n, vis) {IT_BINDING, lbl, hlp, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr, n, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, vis}
#define CHOICEV(lbl, hlp, g, s, arr, vis) {IT_CHOICE, lbl, hlp, g, s, arr, NCH(arr), 0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, vis}
#define SLIDERV(lbl, hlp, g, s, lo, hi, st, vis) {IT_SLIDER, lbl, hlp, g, s, nullptr, 0, lo, hi, st, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, vis}
#define ACTIONV(lbl, hlp, fn, vis) {IT_ACTION, lbl, hlp, nullptr, nullptr, nullptr, 0, 0, 0, 0, fn, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, vis}

static const Item k_display[] = {
    CHOICE("Display mode", "Borderless keeps alt-tab instant. Exclusive takes over the monitor. Alt+Enter / F11 also switch.", get_display_mode, set_display_mode, k_mode, nullptr),
    DCHOICE("Window size", "Size of the window when not in full screen.", get_window_size, set_window_size, count_winsizes, name_winsize, windowed),
    DCHOICE("Monitor", "Which display to use for full screen (and to place the window on).", get_monitor, set_monitor, count_monitors, name_monitor, nullptr),
    CHOICE("Picture shape", "The arcade monitor was 4:3. Widescreen shows more of the arena at the sides in fights (needs Enhanced graphics).", get_aspect, set_aspect, k_aspect, nullptr),
    CHOICE("Widescreen display", "Where the health bars and the boxes at the top sit in widescreen fights: where the arcade put them, or out at the edges of the screen.", get_hud_edges, set_hud_edges, k_hud, widescreen_on),
    CHOICE("Graphics", "Enhanced redraws the 3D on your graphics card at high resolution. F9 switches while playing.", get_graphics, set_graphics, k_graphics, nullptr),
    CHOICE("Quality", "Auto picks the sharpest setting your PC can hold at full speed and steps down if it ever can't.", get_quality, set_quality, k_quality, enhanced_on),
    CHOICE("Screen filter", "Sharp pixels, smooth scaling, or arcade-monitor scanlines.", get_filter, set_filter, k_filter, nullptr),
    CHOICE("Frame pacing", "Arcade speed is the board's own rate. Vsync is smoother on 60 Hz screens but 1% fast.", get_pacing, set_pacing, k_pacing, nullptr),
    DCHOICE("Graphics card", "For PCs with more than one. Takes effect after Restart game (System).", get_gpu, set_gpu, count_gpus, name_gpu, nullptr),
    TOGGLE("Frame rate counter", "Shows frames per second in the corner of the screen.", get_fps, set_fps),
};
static const Item k_audio[] = {
    SLIDER("Volume", "Music, voices and effects (the arcade board mixes them together).", get_volume, set_volume, 0, 100, 5),
    TOGGLE("Mute", "Silence all sound.", get_mute, set_mute),
};
static const Item k_controls[] = {
    INFO("Controllers", "Xbox controllers: the first one connected is player 1, the second is player 2.", info_controllers),
    CHOICE("Player", "Whose keys and buttons to change. Each row shows the keyboard key, then the controller buttons.", get_player, set_player, k_player, nullptr),
    CHOICE("Control scheme", "Ready-made controller layouts. Change any control and this becomes Custom, which is kept for you.", get_scheme, set_scheme, k_scheme, nullptr),
    CHOICE("Show", "The controls come in three groups. Choose a control and press Enter to change its key and buttons.", get_view, set_view, k_view, nullptr),
    // in a fight
    BINDV("Move left", "Steps left in a fight (the game's L button). It does nothing on the game's other screens: that is Select.", 4, view_fight),
    BINDV("Move right", "Steps right in a fight (the game's R button). It does nothing on the game's other screens.", 5, view_fight),
    BINDV("Guard", "Raises your guard in a fight. It holds both step buttons for you, which is how the cabinet guards.", 12, view_fight),
    BINDV("Stick up", "The arcade stick: attacks in a fight, and moves the cursor on the game's screens.", 0, view_fight),
    BINDV("Stick down", "The arcade stick: attacks in a fight, and moves the cursor on the game's screens.", 1, view_fight),
    BINDV("Stick left", "The arcade stick: attacks in a fight, and moves the cursor on the game's screens.", 2, view_fight),
    BINDV("Stick right", "The arcade stick: attacks in a fight, and moves the cursor on the game's screens.", 3, view_fight),
    // the game's own screens, and the cabinet's buttons
    BINDV("Select", "Confirms on the game's own screens, such as choosing your fighter. It does nothing in a fight.", 10, view_screens),
    BINDV("Other button", "The game's second button on its own screens (it confirms as well). It does nothing in a fight.", 11, view_screens),
    BINDV("Start", "The cabinet's Start button.", 8, view_screens),
    BINDV("Insert coin", "Puts a coin in. Not needed with Free play on (Game page).", 9, view_screens),
    BINDV("Button 3", "A spare button of the arcade board. This game shows no use for it.", 6, view_screens),
    BINDV("Button 4", "A spare button of the arcade board. This game shows no use for it.", 7, view_screens),
    // sticks
    CHOICEV("Left stick", "What the left stick does in a fight. On the game's other screens a stick moves the cursor.", get_lstick, set_lstick, k_role, view_sticks),
    CHOICEV("Right stick", "What the right stick does in a fight. On the game's other screens a stick moves the cursor.", get_rstick, set_rstick, k_role, view_sticks),
    SLIDERV("Stick dead zone", "How far a stick must move before it counts.", get_deadzone, set_deadzone, 10, 90, 5, view_sticks),
    ACTIONV("Restore defaults", "Both players back to the Modern scheme and the original keys: arrows, Z X C V 1 5 (player 2: R F D G, A S Q W 2 6).", act_reset_binds, view_sticks),
};
static const Item k_game[] = {
    TOGGLE_EN("Unlock Silber", "The hidden twelfth fighter, Original Karate, at the bottom of the style list. Off: locked until earned, as in the arcade.", get_unlock_silber, set_unlock_silber, offline),
    CHOICE("Free play", "On: Start alone begins a game, no coin needed. Off: insert a coin first, as in the arcade.", get_free_play, set_free_play, k_offon, rules_ready),
    CHOICE("Language / region", "The board's BIOS sets the game's language. The game reads it at power on, so it applies after a restart.", get_region, set_region, k_region, offline),
    ACTION("Restart now to apply", "Starts the arcade board again with the new region.", act_restart, region_pending),
    INFO("Arcade save memory", "Operator settings and high scores are kept between runs.", info_save),
};
// The arcade operator's game options (Test menu > GAME OPTIONS). Saved in the arcade's save memory.
#define RULES_WHEN " Applies from the next fight."
static const Item k_rules[] = {
    CHOICE("Difficulty", "How well the computer fights." RULES_WHEN, get_difficulty, set_difficulty, k_difficulty, rules_ready),
    CHOICE("Attack power, 1 player", "How much damage attacks do against the computer." RULES_WHEN, get_attack_cpu, set_attack_cpu, k_attack, rules_ready),
    CHOICE("Attack power, versus", "How much damage attacks do in a two-player fight." RULES_WHEN, get_attack_vs, set_attack_vs, k_attack, rules_ready),
    CHOICE("Timer speed, 1 player", "How fast the fight clock runs against the computer." RULES_WHEN, get_time_cpu, set_time_cpu, k_time, rules_ready),
    CHOICE("Timer speed, versus", "How fast the fight clock runs in a two-player fight." RULES_WHEN, get_time_vs, set_time_vs, k_time, rules_ready),
    CHOICE("Continue", "Whether you can carry on after losing.", get_continue_, set_continue_, k_offon, rules_ready),
    CHOICE("Replay", "The slow-motion replay of the finish after a fight.", get_replay, set_replay, k_offon, rules_ready),
    CHOICE("Blood", "Blood effects.", get_blood, set_blood, k_offon, rules_ready),
    CHOICE("Move commands on screen", "The arcade's COMMAND DISP. option: the on-screen guide to the controls.", get_command_display, set_command_display, k_offon, rules_ready),
    CHOICE("Sound in the title demo", "Whether the demo fights play sound while nobody is playing.", get_demo_sound, set_demo_sound, k_offon, rules_ready),
};
// Online play (netplay.cpp). Hosting and joining restart the game: both players' machines start from power on
// with the host's settings, which is what keeps them identical.
static const Item k_online[] = {
    INFO("Status", "Experimental. Two players over the internet or a home network, each on their own PC.", info_online),
    ACTION("Host a game", "Experimental. You are player 1. The game restarts and waits; the other player joins with your address.", act_host, offline),
    TEXT("Address to join", "The host's address, such as 192.168.1.20. Enter to type it, Ctrl+V to paste.", get_address, set_address, offline),
    ACTION("Join a game", "You are player 2 and play with the host's game settings. The game restarts and connects to that address.", act_join, can_join),
    CHOICE("Input delay", "Frames before the game sees a button press, for both players (the host's choice). Raise it if the picture often jumps.", get_online_delay, set_online_delay, k_delay, offline),
    INFO("This PC's address", "For a player on your network. Over the internet they need your public address, and your router must forward this port.", info_address),
    ACTION("Leave the online game", "Ends the online game for both players and restarts for ordinary play.", act_leave, online),
};
static const Item k_system[] = {
    ACTION("Resume", "Back to the game.", act_resume, nullptr),
    ACTION("Arcade operator menu", "The cabinet's own test menu (the Test button, F2): settings, input and sound tests.", act_operator, offline),
    ACTION("Restart game", "Starts the arcade board again from power on.", act_restart, offline),
    ACTION("Quit to desktop", "Settings and the arcade's save memory are saved automatically.", act_quit, nullptr),
};

struct Section { const char* name; const Item* items; int n; };
static const Section k_sections[] = {
    {"DISPLAY", k_display, NCH(k_display)},
    {"AUDIO", k_audio, NCH(k_audio)},
    {"CONTROLS", k_controls, NCH(k_controls)},
    {"GAME", k_game, NCH(k_game)},
    {"RULES", k_rules, NCH(k_rules)},
    {"ONLINE", k_online, NCH(k_online)},
    {"SYSTEM", k_system, NCH(k_system)},
};
#define NSECT 7
#define MAXROWS 24
#define VISROWS 11              // rows that fit on the panel; a longer page scrolls to keep the chosen row in view

static int choice_count(const Item* it) { return it->dyn_count ? it->dyn_count() : it->nchoices; }
static const char* choice_name(const Item* it, int v) {
    int n = choice_count(it);
    if (v < 0 || v >= n) v = 0;
    return it->dyn_name ? it->dyn_name(v) : it->choices[v];
}

// ---------------------------------------------------------------- state
static int g_open;              // target state
static float g_t;               // 0 closed .. 1 open (animated)
static int g_sect, g_row[NSECT], g_top[NSECT];
static float g_sel_y, g_tab_x, g_tab_w;   // animated highlight positions
static float g_knob[NSECT][MAXROWS];      // toggle knob / slider animation
static int g_device = UI_DEV_KEYBOARD;
static float g_flash;                     // value-change pulse
static float g_time;                      // seconds, for gentle pulsing
static float g_panel_h;                   // animated panel height

struct Toast { char text[96]; float age; };
static Toast g_toasts[4];

static const Item* g_cap_item;            // the control whose key and buttons are being changed (the editing box)
// a line being typed (IT_TEXT)
static const Item* g_edit;
static char g_edit_buf[64];
// message box and notice (ui_dialog, ui_notice)
static int g_dlg;
static char g_dlg_title[64], g_dlg_body[640], g_dlg_key[16], g_dlg_label[64], g_notice[96];

void ui_init(const UiHost* host) { H = host; }

static int item_visible(const Item* it) { return !it->visible || it->visible(); }
static int item_enabled(const Item* it) {
    return item_visible(it) && it->type != IT_INFO && (!it->enabled || it->enabled());
}

static void select_valid(int dir) {
    const Section* s = &k_sections[g_sect];
    int r = g_row[g_sect];
    for (int k = 0; k < s->n; k++) {
        if (r < 0) r = s->n - 1;
        if (r >= s->n) r = 0;
        if (item_enabled(&s->items[r])) break;
        r += dir ? dir : 1;
    }
    g_row[g_sect] = r < 0 ? 0 : r >= s->n ? s->n - 1 : r;
}

int ui_text_editing(void) { return g_open && g_edit != nullptr; }
void ui_text_input(int ch) {
    if (!g_edit) return;
    size_t n = strlen(g_edit_buf);
    if (ch == 8) { if (n) g_edit_buf[n - 1] = 0; }
    else if (ch == 13) { g_edit->text_set(g_edit_buf); g_edit = nullptr; g_flash = 1; }
    else if (ch == 27) g_edit = nullptr;
    else if (ch > 32 && ch < 127 && n + 2 < sizeof g_edit_buf) { g_edit_buf[n] = (char)ch; g_edit_buf[n + 1] = 0; }
}

void ui_dialog(const char* title, const char* body, const char* key, const char* label) {
    g_dlg = title != nullptr;
    if (!title) return;
    snprintf(g_dlg_title, sizeof g_dlg_title, "%s", title);
    snprintf(g_dlg_body, sizeof g_dlg_body, "%s", body ? body : "");
    snprintf(g_dlg_key, sizeof g_dlg_key, "%s", key ? key : "");
    snprintf(g_dlg_label, sizeof g_dlg_label, "%s", label ? label : "");
}
void ui_notice(const char* text) { snprintf(g_notice, sizeof g_notice, "%s", text ? text : ""); }

void ui_menu_toggle(int device) {
    g_device = device;
    g_open = !g_open;
    g_quit_armed = 0;
    g_edit = nullptr;
    if (g_open) { select_valid(1); uip_art_next(); }
}
void ui_menu_set_page(int page) {
    if (page >= 0 && page < NSECT) { g_sect = page; select_valid(1); }
}
static void act_resume(void) { g_open = 0; }
int ui_menu_visible(void) { return g_open || g_t > 0.001f; }
int ui_menu_wants_pause(void) { return g_open || g_t > 0.001f; }
float ui_backdrop(void) { return ease_out(g_t); }

void ui_toast(const char* msg) {
    for (int i = 3; i > 0; i--) g_toasts[i] = g_toasts[i - 1];
    snprintf(g_toasts[0].text, sizeof g_toasts[0].text, "%s", msg);
    g_toasts[0].age = 0;
}

static void change(const Item* it, int dir) {
    if (!it->get || !it->set) return;
    int v = it->get();
    if (it->type == IT_CHOICE) { int n = choice_count(it); v = (v + dir + n) % n; }
    else if (it->type == IT_TOGGLE) v = !v;
    else if (it->type == IT_SLIDER) {
        v += dir * it->step;
        if (v < it->lo) v = it->lo;
        if (v > it->hi) v = it->hi;
        if (v == it->get()) return;
    }
    it->set(v);
    g_flash = 1;
}

void ui_key(int key, int device) {
    g_device = device;
    if (!g_open) return;
    if (H->capturing() >= 0) return;               // waiting for a key or button press to remap
    if (g_edit) return;                            // typing: the platform sends characters to ui_text_input
    const Section* s = &k_sections[g_sect];
    const Item* it = &s->items[g_row[g_sect]];
    if (key != UI_OK) g_quit_armed = 0;
    switch (key) {
    case UI_UP: g_row[g_sect]--; select_valid(-1); break;
    case UI_DOWN: g_row[g_sect]++; select_valid(1); break;
    case UI_LEFT: if (it->type == IT_CHOICE || it->type == IT_SLIDER) change(it, -1); break;
    case UI_RIGHT: if (it->type != IT_ACTION && it->type != IT_BINDING) change(it, 1); break;
    case UI_PREV_TAB: g_sect = (g_sect + NSECT - 1) % NSECT; select_valid(1); break;
    case UI_NEXT_TAB: g_sect = (g_sect + 1) % NSECT; select_valid(1); break;
    case UI_OK:
        if (it->type == IT_ACTION) it->action();
        else if (it->type == IT_BINDING) { g_cap_item = it; H->start_capture(g_ctl_player * 16 + it->binding); }
        else if (it->type == IT_TEXT) { g_edit = it; snprintf(g_edit_buf, sizeof g_edit_buf, "%s", it->text_get()); }
        else if (it->type == IT_TOGGLE || it->type == IT_CHOICE) change(it, 1);
        break;
    case UI_BACK:
    case UI_CLOSE: g_open = 0; break;
    }
    // an action may have disabled the row it was on (e.g. "Restart now to apply")
    if (g_open && !item_enabled(&k_sections[g_sect].items[g_row[g_sect]])) select_valid(1);
}

void ui_update(float dt) {
    if (dt > 0.1f) dt = 0.1f;
    g_t += (g_open ? 1 : -1) * dt / (g_open ? 0.22f : 0.16f);
    if (g_t < 0) g_t = 0;
    if (g_t > 1) g_t = 1;
    g_flash = approach(g_flash, 0, dt, 6);
    g_time += dt;
    if (!g_open) g_edit = nullptr;
    for (auto& t : g_toasts) t.age += dt;
    for (int s = 0; s < NSECT; s++)
        for (int r = 0; r < k_sections[s].n; r++) {
            const Item* it = &k_sections[s].items[r];
            float target = 0;
            if (it->type == IT_TOGGLE) target = (float)it->get();
            else if (it->type == IT_SLIDER) target = (float)(it->get() - it->lo) / (float)(it->hi - it->lo);
            g_knob[s][r] = approach(g_knob[s][r], target, dt, 18);
        }
}

// ---------------------------------------------------------------- drawing helpers
static void text_c(float cx, float y, int font, uint32_t col, const char* s) { uip_text(cx - uip_text_w(font, s) / 2, y, font, col, s); }
static void text_r(float rx, float y, int font, uint32_t col, const char* s) { uip_text(rx - uip_text_w(font, s), y, font, col, s); }
static float vcenter(float y, float h, int font) { return y + (h - uip_line_h(font)) / 2; }

// a controller button drawn like the real thing: coloured face buttons, dark shoulder buttons
static float button_glyph(float x, float cy, const char* name, float a, int right_align) {
    uint32_t face = 0;
    if (!strcmp(name, "A")) face = 0x5DBB63FFu;
    else if (!strcmp(name, "B")) face = 0xE05C5CFFu;
    else if (!strcmp(name, "X")) face = 0x4A90E2FFu;
    else if (!strcmp(name, "Y")) face = 0xE8C447FFu;
    float h = 32, w;
    if (face) {
        w = h;
        if (right_align) x -= w;
        uip_rect(x, cy - h / 2, w, h, h / 2, fade(face, a), fade(mix(face, 0x000000FFu, 0.25f), a), 0, 0, 0);
        text_c(x + w / 2, vcenter(cy - h / 2, h, UI_FONT_SMALL), UI_FONT_SMALL, fade(0x101114FFu, a), name);
    } else {
        w = uip_text_w(UI_FONT_SMALL, name) + 22;
        if (right_align) x -= w;
        uip_rect(x, cy - h / 2, w, h, 8, fade(0x3A3E4CFFu, a), fade(0x2A2D38FFu, a), 1, fade(0xFFFFFF22u, a), 0);
        text_c(x + w / 2, vcenter(cy - h / 2, h, UI_FONT_SMALL), UI_FONT_SMALL, fade(COL_TEXT, a), name);
    }
    return w;
}

static float key_glyph_w(const char* name) {
    float h = 30, w = uip_text_w(UI_FONT_SMALL, name) + 20;
    return w < h ? h : w;
}
static float key_glyph(float x, float cy, const char* name, float a) {
    float h = 30, w = key_glyph_w(name);
    uip_rect(x, cy - h / 2, w, h, 6, fade(0xE9EAEEFFu, a), fade(0xC9CBD3FFu, a), 0, 0, 0);
    text_c(x + w / 2, vcenter(cy - h / 2, h, UI_FONT_SMALL), UI_FONT_SMALL, fade(0x15161BFFu, a), name);
    return w;
}

static float hint(float x, float cy, const char* glyph_pad, const char* glyph_key, const char* label, float a) {
    float w = g_device == UI_DEV_PAD ? button_glyph(x, cy, glyph_pad, a, 0) : key_glyph(x, cy, glyph_key, a);
    uip_text(x + w + 10, vcenter(cy - 16, 32, UI_FONT_SMALL), UI_FONT_SMALL, fade(COL_TEXT_DIM, a), label);
    return w + 10 + uip_text_w(UI_FONT_SMALL, label) + 34;
}

// ---------------------------------------------------------------- the menu
static void draw_menu(float vw, float vh) {
    float e = ease_out(g_t), a = e;
    const Section* s = &k_sections[g_sect];
    const float W = 1140, HMAX = 940, row_h = 48, gap = 4;
    float target_h = 210 + 11.0f * (row_h + gap) + 150;      // one height for every page
    if (target_h > HMAX) target_h = HMAX;
    if (g_panel_h == 0) g_panel_h = target_h;
    g_panel_h = approach(g_panel_h, target_h, 1.0f / 60, 16);
    const float Hh = g_panel_h;
    float px = (vw - W) / 2, py = (vh - Hh) / 2 + (1 - e) * 28;      // centred; glides as the height changes

    // soft shadow, then the glass panel
    uip_rect(px - 10, py + 6, W + 20, Hh + 20, 30, fade(0x00000090u, a), fade(0x00000090u, a), 0, 0, 34);
    uip_rect(px, py, W, Hh, 20, fade(COL_PANEL_TOP, a), fade(COL_PANEL_BOT, a), 1.5f, fade(0xFFFFFF1Cu, a), 0);
    if (int art = uip_art_available()) {
        // menu art (a fighting-style portrait, or menu_art.png): under a dark gradient so the text stays readable;
        // the portraits are dark already, so they get a lighter one
        uip_art(px, py, W, Hh, 20, a);
        uip_rect_hgrad(px, py, W, Hh, 20, fade(0x0E0F15F4u, a), fade(art == 2 ? 0x0E0F1508u : 0x0E0F1550u, a));
        uip_rect(px, py, W, Hh, 20, fade(0x0E0F1500u, a), fade(art == 2 ? 0x0E0F1590u : 0x0E0F15C0u, a), 0, 0, 0);
        uip_rect(px, py, W, Hh, 20, 0, 0, 1.5f, fade(0xFFFFFF1Cu, a), 0);
    } else {
        // no art: a faint warm light from the right-hand side, so the plain panel isn't flat
        uip_rect_hgrad(px, py, W, Hh, 20, fade(0xE8B64C00u, a), fade(0xE8B64C0Cu, a));
        uip_rect(px, py, W, Hh * 0.45f, 20, fade(0xFFFFFF07u, a), fade(0xFFFFFF00u, a), 0, 0, 0);
    }
    // a faint gold glow along the top edge
    uip_rect(px + 40, py, W - 80, 2, 1, fade(0xE8B64C70u, a), fade(0xE8B64C70u, a), 0, 0, 0);

    // header
    uip_text(px + 58, py + 38, UI_FONT_TITLE, fade(COL_TEXT, a), "BURIKI ONE");
    uip_rect(px + 60, py + 100, 70, 4, 2, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
    {
        const char* p = online() ? "PAUSED FOR BOTH PLAYERS" : "PAUSED";
        float tw = uip_text_w(UI_FONT_SMALL, p) + 28;
        uip_rect(px + W - 58 - tw, py + 48, tw, 34, 17, fade(0xFFFFFF14u, a), fade(0xFFFFFF0Cu, a), 1, fade(0xFFFFFF18u, a), 0);
        text_c(px + W - 58 - tw / 2, vcenter(py + 48, 34, UI_FONT_SMALL), UI_FONT_SMALL, fade(COL_TEXT_DIM, a), p);
    }

    // section tabs
    float ty = py + 132, tx = px + 58;
    float tab_x[NSECT], tab_w[NSECT];
    float lead = g_device == UI_DEV_PAD ? button_glyph(tx, ty + 18, "LB", a, 0) : key_glyph(tx, ty + 18, "Q", a);
    tx += lead + 26;
    for (int i = 0; i < NSECT; i++) {
        tab_w[i] = uip_text_w(UI_FONT_HEAD, k_sections[i].name);
        tab_x[i] = tx;
        uint32_t c = i == g_sect ? COL_TEXT : COL_TEXT_DIM;
        uip_text(tx, vcenter(ty, 36, UI_FONT_HEAD), UI_FONT_HEAD, fade(c, a), k_sections[i].name);
        tx += tab_w[i] + 44;
    }
    if (g_device == UI_DEV_PAD) button_glyph(tx - 18, ty + 18, "RB", a, 0);
    else key_glyph(tx - 18, ty + 18, "E", a);
    if (g_tab_w == 0) { g_tab_x = tab_x[g_sect]; g_tab_w = tab_w[g_sect]; }
    uip_rect(g_tab_x, ty + 42, g_tab_w, 3, 1.5f, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
    uip_rect(px + 40, py + 190, W - 80, 1, 0, fade(COL_LINE, a), fade(COL_LINE, a), 0, 0, 0);

    // rows
    const float y0 = py + 210, rows_bottom = py + Hh - 124;
    int nvis = 0, sel_vis = 0;      // rows on show, and the chosen row's place among them
    for (int r = 0; r < s->n; r++) {
        if (!item_visible(&s->items[r])) continue;
        if (r == g_row[g_sect]) sel_vis = nvis;
        nvis++;
    }
    int top = g_top[g_sect];
    if (sel_vis < top) top = sel_vis;
    if (sel_vis >= top + VISROWS) top = sel_vis - VISROWS + 1;
    if (top > nvis - VISROWS) top = nvis > VISROWS ? nvis - VISROWS : 0;
    g_top[g_sect] = top;
    float target_y = y0 + (float)(sel_vis - top) * (row_h + gap);
    if (nvis > VISROWS) {           // a slim bar on the right: where this view is in the list
        const float th = (float)VISROWS * (row_h + gap) - gap, bh = th * (float)VISROWS / (float)nvis;
        const float by = y0 + (th - bh) * (float)top / (float)(nvis - VISROWS);
        uip_rect(px + W - 24, y0, 4, th, 2, fade(0xFFFFFF10u, a), fade(0xFFFFFF10u, a), 0, 0, 0);
        uip_rect(px + W - 24, by, 4, bh, 2, fade(COL_GOLD, a * 0.8f), fade(COL_GOLD, a * 0.8f), 0, 0, 0);
    }
    if (g_sel_y == 0) g_sel_y = target_y;
    int capturing = H->capturing();
    {
        float hy = g_sel_y;
        uip_rect(px + 34, hy, W - 68, row_h, 12, fade(0xE8B64C24u, a), fade(0xE8B64C14u, a), 1, fade(0xE8B64C30u, a), 0);
        uip_rect(px + 34, hy + 10, 4, row_h - 20, 2, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
    }
    for (int r = 0, vi = -1; r < s->n; r++) {
        const Item* it = &s->items[r];
        if (!item_visible(it)) continue;
        vi++;
        if (vi < top || vi >= top + VISROWS) continue;
        float y = y0 + (float)(vi - top) * (row_h + gap);
        // while the panel is still growing, rows below its edge fade in
        float ra = (rows_bottom - y) / row_h;
        if (ra <= 0) continue;
        float a_save = a;
        a = a_save * (ra > 1 ? 1 : ra);
        int sel = r == g_row[g_sect], en = it->type == IT_INFO || !it->enabled || it->enabled();
        uint32_t lc = !en ? COL_TEXT_OFF : sel ? COL_TEXT : 0xC9CDD6FFu;
        const char* label = it->label;
        bool quit_row = it->action == act_quit;
        if (quit_row && g_quit_armed) label = "Press again to quit";
        uip_text(px + 62, vcenter(y, row_h, UI_FONT_BODY), UI_FONT_BODY, fade(quit_row && g_quit_armed ? COL_RED : lc, a), label);
        float rx = px + W - 66, cy = y + row_h / 2;
        switch (it->type) {
        case IT_CHOICE: {
            const char* v = choice_name(it, it->get());
            uint32_t vc = !en ? COL_TEXT_OFF : sel ? mix(COL_TEXT, COL_GOLD, g_flash * 0.8f) : COL_TEXT_DIM;
            uint32_t ac = !en ? COL_TEXT_OFF : sel ? COL_GOLD : 0x6A7080FFu;
            float vw2 = uip_text_w(UI_FONT_VALUE, v);
            if (sel && en) {           // arrows only on the focused row
                text_r(rx, vcenter(y, row_h, UI_FONT_HEAD) - 2, UI_FONT_HEAD, fade(ac, a), "\xE2\x80\xBA");
                uip_text(rx - 30 - vw2, vcenter(y, row_h, UI_FONT_VALUE), UI_FONT_VALUE, fade(vc, a), v);
                text_r(rx - 48 - vw2, vcenter(y, row_h, UI_FONT_HEAD) - 2, UI_FONT_HEAD, fade(ac, a), "\xE2\x80\xB9");
            } else {
                text_r(rx, vcenter(y, row_h, UI_FONT_VALUE), UI_FONT_VALUE, fade(vc, a), v);
            }
            break;
        }
        case IT_TOGGLE: {
            float k = g_knob[g_sect][r], tw = 60, th = 32;
            uint32_t on = COL_GOLD, off = 0x3B3F4DFFu;
            uip_rect(rx - tw, cy - th / 2, tw, th, th / 2, fade(mix(off, on, k), a), fade(mix(off, mix(on, 0x000000FFu, 0.15f), k), a), 0, 0, 0);
            float kx = rx - tw + 4 + k * (tw - th);
            uip_rect(kx, cy - th / 2 + 4, th - 8, th - 8, (th - 8) / 2, fade(0xFFFFFFFFu, a), fade(0xE4E5EAFFu, a), 0, 0, 0);
            break;
        }
        case IT_SLIDER: {
            float k = g_knob[g_sect][r], tw = 250;
            char buf[16];
            snprintf(buf, sizeof buf, "%d%%", it->get());
            text_r(rx - tw - 22, vcenter(y, row_h, UI_FONT_VALUE), UI_FONT_VALUE, fade(sel ? COL_TEXT : COL_TEXT_DIM, a), buf);
            uip_rect(rx - tw, cy - 3, tw, 6, 3, fade(0xFFFFFF1Eu, a), fade(0xFFFFFF1Eu, a), 0, 0, 0);
            uip_rect(rx - tw, cy - 3, tw * k, 6, 3, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
            float kx = rx - tw + tw * k;
            uip_rect(kx - 11, cy - 11, 22, 22, 11, fade(0xFFFFFFFFu, a), fade(0xE4E5EAFFu, a), 0, 0, 0);
            break;
        }
        case IT_BINDING: {
            int action = g_ctl_player * 16 + it->binding;
            (void)capturing;
            {
                // controller buttons on the right (all that are on it), keyboard key to their left
                float bx = rx;
                int nb = H->pad_binding_count(action);
                for (int i = (nb ? nb : 1) - 1; i >= 0; i--) bx -= button_glyph(bx, cy, H->pad_binding_at(action, i), a, 1) + 8;
                const char* kn = H->key_binding_name(action);
                float kw = key_glyph_w(kn);
                key_glyph(bx - 10 - kw, cy, kn, a);
            }
            break;
        }
        case IT_TEXT: {
            // a box holding the text; gold with a blinking caret while it is being typed
            const bool editing = g_edit == it;
            const char* v = editing ? g_edit_buf : it->text_get();
            const char* shown = *v ? v : editing ? "" : "Not set";
            float tw = uip_text_w(UI_FONT_VALUE, shown), bw = tw + 40 < 300 ? 300 : tw + 40, bh = 36;
            uip_rect(rx - bw, cy - bh / 2, bw, bh, 9, fade(editing ? 0xE8B64C2Au : 0xFFFFFF10u, a), fade(editing ? 0xE8B64C1Au : 0xFFFFFF0Au, a),
                     1, fade(editing ? COL_GOLD : 0xFFFFFF22u, a), 0);
            uint32_t tc = !en ? COL_TEXT_OFF : !*v ? COL_TEXT_OFF : sel ? mix(COL_TEXT, COL_GOLD, g_flash * 0.8f) : COL_TEXT_DIM;
            uip_text(rx - bw + 18, vcenter(cy - bh / 2, bh, UI_FONT_VALUE), UI_FONT_VALUE, fade(tc, a), shown);
            if (editing && fmodf(g_time, 1.0f) < 0.55f)
                uip_rect(rx - bw + 20 + tw, cy - 11, 2, 22, 1, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
            break;
        }
        case IT_ACTION:
            text_r(rx, vcenter(y, row_h, UI_FONT_VALUE), UI_FONT_VALUE, fade(sel ? COL_GOLD : en ? 0x6A7080FFu : COL_TEXT_OFF, a), "\xE2\x80\xBA");
            break;
        case IT_INFO:
            if (it->info == info_controllers) {      // a badge per player with a status light
                float bx = rx;
                for (int p = 1; p >= 0; p--) {
                    int on = H->controller_name(p) != nullptr;
                    char lbl[32];
                    snprintf(lbl, sizeof lbl, "P%d  %s", p + 1, on ? "Connected" : "Not connected");
                    float bw = uip_text_w(UI_FONT_SMALL, lbl) + 46, bh = 32;
                    bx -= bw;
                    uip_rect(bx, cy - bh / 2, bw, bh, bh / 2, fade(on ? 0x5DBB6324u : 0xFFFFFF0Cu, a),
                             fade(on ? 0x5DBB6318u : 0xFFFFFF08u, a), 1, fade(on ? 0x5DBB6366u : 0xFFFFFF18u, a), 0);
                    uint32_t dot = on ? 0x6FD675FFu : 0x6A7080FFu;
                    uip_rect(bx + 16, cy - 4, 8, 8, 4, fade(dot, a), fade(dot, a), 0, 0, 0);
                    uip_text(bx + 32, vcenter(cy - bh / 2, bh, UI_FONT_SMALL), UI_FONT_SMALL, fade(on ? COL_TEXT : COL_TEXT_DIM, a), lbl);
                    bx -= 10;
                }
            } else {
                text_r(rx, vcenter(y, row_h, UI_FONT_VALUE), UI_FONT_VALUE, fade(COL_TEXT_DIM, a), it->info());
            }
            break;
        }
        a = a_save;
    }

    // help line for the selected row
    float fy = py + Hh - 118;
    uip_rect(px + 40, fy, W - 80, 1, 0, fade(COL_LINE, a), fade(COL_LINE, a), 0, 0, 0);
    uip_text(px + 62, fy + 18, UI_FONT_SMALL, fade(COL_TEXT_DIM, a),
             g_edit ? "Type the address, then press Enter. Ctrl+V pastes. Esc cancels." : s->items[g_row[g_sect]].help);

    // button hints
    float hx = px + 62, hy = py + Hh - 42;
    hx += hint(hx, hy, "A", "Enter", "Select", a);
    hx += hint(hx, hy, "B", "Esc", "Back", a);
    if (g_device == UI_DEV_PAD) hint(hx, hy, "MENU", "Tab", "Resume", a);
    else hint(hx, hy, "VIEW", "\xE2\x86\x90\xE2\x86\x92", "Change", a);

    g_sel_y = approach(g_sel_y, target_y, 1.0f / 60, 22);
    g_tab_x = approach(g_tab_x, tab_x[g_sect], 1.0f / 60, 20);
    g_tab_w = approach(g_tab_w, tab_w[g_sect], 1.0f / 60, 20);
}

// A message box in the middle of the screen, over everything.
static void draw_dialog(float vw, float vh) {
    const float W = 980, line_h = 40;
    int lines = 1;
    for (const char* c = g_dlg_body; *c; c++) lines += *c == '\n';
    const float Hh = 150 + (float)lines * line_h + 96;
    const float px = (vw - W) / 2, py = (vh - Hh) / 2;
    uip_rect(0, 0, vw, vh, 0, 0x00000070u, 0x00000070u, 0, 0, 0);
    uip_rect(px - 10, py + 6, W + 20, Hh + 20, 30, 0x00000090u, 0x00000090u, 0, 0, 34);
    uip_rect(px, py, W, Hh, 20, COL_PANEL_TOP, COL_PANEL_BOT, 1.5f, 0xFFFFFF1Cu, 0);
    uip_rect(px + 40, py, W - 80, 2, 1, 0xE8B64C70u, 0xE8B64C70u, 0, 0, 0);
    uip_text(px + 58, py + 44, UI_FONT_HEAD, COL_TEXT, g_dlg_title);
    // three dots that pulse in turn while something is going on (a wait the player can cancel)
    for (int i = 0; i < 3 && !strcmp(g_dlg_key, "Esc"); i++) {
        float k = 0.5f + 0.5f * sinf(g_time * 4.0f - (float)i * 0.9f);
        uip_rect(px + W - 110 + (float)i * 20, py + 58, 10, 10, 5, fade(COL_GOLD, 0.25f + 0.75f * k), fade(COL_GOLD, 0.25f + 0.75f * k), 0, 0, 0);
    }
    uip_rect(px + 60, py + 96, 70, 4, 2, COL_GOLD, COL_GOLD, 0, 0, 0);
    float y = py + 132;
    char line[200];
    for (const char* c = g_dlg_body; ; ) {
        const char* e = strchr(c, '\n');
        size_t n = e ? (size_t)(e - c) : strlen(c);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, c, n);
        line[n] = 0;
        uip_text(px + 60, y, UI_FONT_BODY, 0xC9CDD6FFu, line);
        y += line_h;
        if (!e) break;
        c = e + 1;
    }
    if (g_dlg_key[0]) {
        float fy = py + Hh - 84;
        uip_rect(px + 40, fy, W - 80, 1, 0, COL_LINE, COL_LINE, 0, 0, 0);
        float kw = key_glyph(px + 62, fy + 44, g_dlg_key, 1);
        uip_text(px + 62 + kw + 12, vcenter(fy + 28, 32, UI_FONT_SMALL), UI_FONT_SMALL, COL_TEXT_DIM, g_dlg_label);
    }
}

// The box for changing one control: it stays open while keys and buttons are pressed, and shows what the
// control has as it changes.
static void draw_capture(float vw, float vh, int action) {
    const float W = 860, Hh = 420, px = (vw - W) / 2, py = (vh - Hh) / 2;
    uip_rect(0, 0, vw, vh, 0, 0x00000060u, 0x00000060u, 0, 0, 0);
    uip_rect(px - 10, py + 6, W + 20, Hh + 20, 30, 0x00000090u, 0x00000090u, 0, 0, 34);
    uip_rect(px, py, W, Hh, 20, 0x1B1D27FAu, 0x0E0F15FCu, 1.5f, 0xE8B64C60u, 0);
    char title[96];
    snprintf(title, sizeof title, "%s    Player %d", g_cap_item ? g_cap_item->label : "", action / 16 + 1);
    uip_text(px + 52, py + 40, UI_FONT_HEAD, COL_TEXT, title);
    uip_rect(px + 54, py + 90, 70, 4, 2, COL_GOLD, COL_GOLD, 0, 0, 0);
    // what it has now
    float y = py + 128;
    uip_text(px + 54, vcenter(y, 44, UI_FONT_BODY), UI_FONT_BODY, COL_TEXT_DIM, "Keyboard");
    key_glyph(px + 250, y + 22, H->key_binding_name(action), 1);
    y += 58;
    uip_text(px + 54, vcenter(y, 44, UI_FONT_BODY), UI_FONT_BODY, COL_TEXT_DIM, "Controller");
    float bx = px + 250;
    const int nb = H->pad_binding_count(action);
    for (int i = 0; i < nb; i++) bx += button_glyph(bx, y + 22, H->pad_binding_at(action, i), 1, 0) + 10;
    if (!nb) uip_text(bx, vcenter(y, 44, UI_FONT_BODY), UI_FONT_BODY, COL_TEXT_OFF, "nothing");
    // how
    y += 70;
    float pulse = 0.5f + 0.5f * sinf(g_time * 5.0f);
    uip_rect(px + 54, y + 10, 10, 10, 5, fade(COL_GOLD, 0.3f + 0.7f * pulse), fade(COL_GOLD, 0.3f + 0.7f * pulse), 0, 0, 0);
    uip_text(px + 78, y, UI_FONT_SMALL, COL_TEXT, "Press a controller button to add it. Press it again to take it off.");
    uip_text(px + 78, y + 34, UI_FONT_SMALL, COL_TEXT, "Press a key to make it the keyboard key.");
    float fy = py + Hh - 76;
    uip_rect(px + 40, fy, W - 80, 1, 0, COL_LINE, COL_LINE, 0, 0, 0);
    float kw = key_glyph(px + 54, fy + 40, "Esc", 1);
    uip_text(px + 54 + kw + 12, vcenter(fy + 24, 32, UI_FONT_SMALL), UI_FONT_SMALL, COL_TEXT_DIM,
             "Done      (or just wait: it closes by itself)");
}

static void draw_notice(float vw) {
    float w = uip_text_w(UI_FONT_SMALL, g_notice) + 78, h = 46, x = (vw - w) / 2, y = 34;
    float k = 0.5f + 0.5f * sinf(g_time * 4.0f);
    uip_rect(x, y, w, h, 23, 0x1B1D27EEu, 0x12131AEEu, 1, 0xFFFFFF1Cu, 0);
    uip_rect(x + 22, y + h / 2 - 5, 10, 10, 5, fade(COL_GOLD, 0.3f + 0.7f * k), fade(COL_GOLD, 0.3f + 0.7f * k), 0, 0, 0);
    uip_text(x + 46, vcenter(y, h, UI_FONT_SMALL), UI_FONT_SMALL, COL_TEXT, g_notice);
}

static void draw_toasts(float vw, float vh) {
    (void)vw;
    float y = vh - 92;
    for (auto& t : g_toasts) {
        if (!t.text[0] || t.age > 3.0f) continue;
        float in = ease_out(t.age / 0.25f), out = t.age > 2.5f ? 1 - (t.age - 2.5f) / 0.5f : 1;
        float a = in * out;
        float w = uip_text_w(UI_FONT_SMALL, t.text) + 64, h = 46, x = 40 - (1 - in) * 30;
        uip_rect(x, y, w, h, 23, fade(0x1B1D27EEu, a), fade(0x12131AEEu, a), 1, fade(0xFFFFFF1Cu, a), 0);
        uip_rect(x + 20, y + h / 2 - 5, 10, 10, 5, fade(COL_GOLD, a), fade(COL_GOLD, a), 0, 0, 0);
        uip_text(x + 42, vcenter(y, h, UI_FONT_SMALL), UI_FONT_SMALL, fade(COL_TEXT, a), t.text);
        y -= h + 10;
    }
}

void ui_draw(float vw, float vh, float fps) {
    if (g_settings.show_fps) {
        char b[48];
        const int ping = H && H->online_active() ? H->online_ping() : -1;
        if (ping >= 0) snprintf(b, sizeof b, "%.0f FPS   %d ms", fps, ping);
        else snprintf(b, sizeof b, "%.0f FPS", fps);
        float w = uip_text_w(UI_FONT_SMALL, b) + 28;
        uip_rect(vw - 30 - w, 26, w, 36, 18, 0x101118B0u, 0x101118B0u, 1, 0xFFFFFF18u, 0);
        text_c(vw - 30 - w / 2, vcenter(26, 36, UI_FONT_SMALL), UI_FONT_SMALL, fps >= 58.5f ? 0x9BE39FFFu : COL_GOLD, b);
    }
    if (g_notice[0] && !g_dlg) draw_notice(vw);
    if (ui_menu_visible()) draw_menu(vw, vh);
    if (ui_menu_visible() && H && H->capturing() >= 0) draw_capture(vw, vh, H->capturing());
    if (g_dlg) draw_dialog(vw, vh);
    draw_toasts(vw, vh);
}
