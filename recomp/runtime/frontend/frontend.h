// The PC front end: window and full-screen handling, Direct3D 11 presentation (GDI fallback), frame pacing,
// the pause menu, settings.ini / controls.ini, host controls and the arcade's save memory (nvram.bin).
// Headless runs use only fe_init, fe_read_host and (when asked for) the save memory.
#pragma once
#include <stdint.h>

struct FeOptions {
    bool headless = false;
    bool throttle = true;              // pace to the arcade's 59.43 Hz (or vsync); false = as fast as possible
    int scale = 0;                     // --scale N: window N x 448 lines tall for this run (0 = settings.ini)
    const char* menu_test = nullptr;   // --menu-test PAGE: open the menu on that page, save a screenshot, quit
    uint32_t menu_test_at = 600;       // ...at this frame
    const char* menu_shot = nullptr;   // ...to this .bmp (default menu_test_<page>.bmp)
    bool no_save = false;              // don't write settings.ini / controls.ini (tests)
    uint32_t shot_every = 0;           // --shot-every N: save the window picture every N frames (tests)...
    const char* shot_dir = nullptr;    // --shot-dir DIR: ...as DIR/shot_NNNNN.bmp
    const char* test_keys = nullptr;   // --test-keys "N:key,...": simulated key presses at displayed frame N
};

// Before hw_init: loads settings.ini and controls.ini from the exe's folder.
void fe_init(const FeOptions& opt);
// --set key=value: change a setting for this run only (the menu test uses it); 0 if unknown.
int fe_override_setting(const char* key_eq_value);
// BIOS file for the region chosen in the menu (Game page).
const char* fe_bios_file(void);
void fe_note_running_bios(const char* file);   // what the board actually boots with

// Called by video.cpp.
void fe_window_open(void);
// Show the finished frame, pace to the frame rate, run the menu. While the menu is open this doesn't
// return: emulated time stops and the same frame keeps being shown under the menu.
void fe_present_frame(const uint32_t* rgb, int w, int h);
bool fe_window_active(void);           // a window is open (not headless)

// Called by input.cpp once per emulated frame: the host's keyboard + controllers, already mapped to the
// arcade controls [player][up, down, left, right, b1-b4, start, coin]. Nothing reaches the game while the
// menu is open or just after it closes; the menu's "operator menu" action shows up as *test.
void fe_read_host(bool p[2][10], bool* service, bool* test);
void fe_controls_load(void);

// Watchdog support: changes while the front end is busy (menu open, window being dragged...).
uint64_t fe_activity(void);
// While the game is made to wait on purpose (online play, for the other player): keeps the window
// answering and tells the watchdog the wait is meant.
void fe_wait_pump(void);

// ---- online play's screens (netplay.cpp) ----
// One displayed frame of a message box over the last picture (or black before the game has drawn one).
// Returns 1 when the player pressed Enter / A, 2 for Esc / B, else 0. Without a window: a short sleep, 0.
int  fe_dialog_frame(const char* title, const char* body, const char* key, const char* label);
// One displayed frame while the game waits for the other player: the last picture again with a notice at the
// top; the pause menu still works. fe_stall_end() when the wait is over.
void fe_dialog_off(void);
void fe_stall_frame(const char* notice);
void fe_stall_end(void);
void fe_toast(const char* text);
// Start the program again with the online-play options replaced by `netplay_args` ("" = ordinary play), and
// end this copy.
[[noreturn]] void fe_relaunch(const char* netplay_args);

// Arcade save memory (the 16 KB nvram at physical 0x1F800000): loaded after hw_init, saved when it changes
// (at most once a second) and at exit. path = nullptr disables it.
void fe_nvram_init(const char* path);
void fe_nvram_tick(void);              // once per frame
void fe_nvram_save(void);
const char* fe_nvram_path(void);       // nullptr when off
const char* fe_default_nvram_path(void);   // nvram.bin next to the exe

// main.cpp: stderr is the log file next to the exe. "Restart game" then lets go of it before starting the new
// copy, which moves it to buriki.old.log and opens its own.
void fe_note_log_file(bool on);

// Volume for audio_out (0-100, mute folded in).
int fe_volume(void);
