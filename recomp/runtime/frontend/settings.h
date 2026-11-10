// Player settings, saved to settings.ini next to the exe. Platform independent.
//
// Adapted from ReLayer (port/ui/settings.h), Copyright (c) 2026 The ReLayer project, MIT License
// (see LICENSE-ReLayer.txt in this folder).
#pragma once

enum { DISPLAY_WINDOW, DISPLAY_BORDERLESS, DISPLAY_EXCLUSIVE };
enum { ASPECT_ARCADE, ASPECT_STRETCH, ASPECT_INTEGER, ASPECT_WIDE };
enum { QUALITY_AUTO, QUALITY_PERFORMANCE, QUALITY_BALANCED, QUALITY_HIGH, QUALITY_ULTRA, QUALITY_EXTREME };
enum { FILTER_SHARP, FILTER_SMOOTH, FILTER_SCANLINES };
enum { PACING_ARCADE, PACING_VSYNC };
enum { REGION_JAPAN, REGION_USA, REGION_WORLD, REGION_COUNT };

struct Settings {
    int display_mode;     // DISPLAY_*
    int window_size;      // index into the window sizes (fe_window.cpp)
    int monitor;          // 0 = the monitor the window is on / primary, else 1-based display number
    int aspect;           // ASPECT_*
    int enhanced;         // 1 = high-resolution renderer (when one is plugged in, see hd_hook.h), 0 = original picture
    int quality;          // QUALITY_* (render scale for the high-resolution renderer; Auto adapts)
    int filter;           // FILTER_*
    int pacing;           // PACING_*: the arcade's 59.43 Hz, or the display's vsync when it runs at ~60 Hz
    int gpu;              // 0 = automatic (most video memory), else 1-based adapter number; applies after restart
    int show_fps;
    int volume;           // 0-100
    int mute;
    int unlock_silber;    // 1 = the hidden twelfth fighter is selectable from the start (hw_set_unlock_silber)
    int region;           // REGION_*: which BIOS the board boots with (the game's language); applies after restart
};

extern Settings g_settings;

void settings_defaults(Settings* s);
int  settings_load(Settings* s, const char* path);     // 0 if the file doesn't exist
int  settings_save(const Settings* s, const char* path);
int  settings_set(Settings* s, const char* key_eq_value);   // "key=value" (command line --set); 0 if unknown
int  settings_quality_scale(int quality);              // render scale for a preset, 0 = auto
const char* settings_region_bios(int region);          // BIOS file name in hng64/
