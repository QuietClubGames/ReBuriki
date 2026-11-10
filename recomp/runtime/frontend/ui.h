// Overlay UI: the pause menu, toast notifications and the frame-rate counter.
// Platform independent: it lays everything out in a virtual space 1080 units tall (width follows the window)
// and draws through the small uip_* interface, implemented per platform (fe_ui_d3d11.cpp).
// Colours are 0xRRGGBBAA.
//
// Adapted from ReLayer (port/ui/ui.h), Copyright (c) 2026 The ReLayer project, MIT License
// (see LICENSE-ReLayer.txt in this folder).
#pragma once
#include <stdint.h>

enum { UI_FONT_BODY, UI_FONT_VALUE, UI_FONT_SMALL, UI_FONT_HEAD, UI_FONT_TITLE, UI_FONT_COUNT };

// ---- drawing, provided by the platform ----
// rounded rectangle with a vertical gradient, optional border; feather > 0 makes a soft shadow
void  uip_rect(float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom,
               float border, uint32_t border_col, float feather);
void  uip_text(float x, float y, int font, uint32_t col, const char* utf8);   // y = top of the line
float uip_text_w(int font, const char* utf8);
float uip_line_h(int font);
// horizontal gradient rectangle (left -> right)
void  uip_rect_hgrad(float x, float y, float w, float h, float radius, uint32_t left, uint32_t right);
// menu art: the fighting-style portraits (whole, against the right edge), or menu_art.png next to the exe
// (drawn to cover the rectangle); clipped to the rectangle's rounded corners
int   uip_art_available(void);  // 0 none, 1 menu_art.png, 2 a portrait
void  uip_art_next(void);     // show the next portrait (shuffled; called each time the menu opens)
void  uip_art(float x, float y, float w, float h, float radius, float alpha);

// ---- input ----
enum { UI_UP, UI_DOWN, UI_LEFT, UI_RIGHT, UI_OK, UI_BACK, UI_PREV_TAB, UI_NEXT_TAB, UI_CLOSE };
enum { UI_DEV_KEYBOARD, UI_DEV_PAD };
enum { UI_PAGE_DISPLAY, UI_PAGE_AUDIO, UI_PAGE_CONTROLS, UI_PAGE_GAME, UI_PAGE_SYSTEM, UI_PAGE_COUNT };

// ---- services the menu needs from the platform ----
// Remappable actions: player * 6 + {0-3 buttons 1-4, 4 start, 5 coin}.
struct UiHost {
    void        (*settings_changed)(void);          // apply g_settings and save them
    const char* (*controller_name)(int index);      // NULL when not connected
    const char* (*pad_binding_name)(int action);    // "X", "RB", "VIEW"...
    const char* (*key_binding_name)(int action);    // "Z", "Enter"...
    void        (*start_capture)(int action);       // remap: wait for the next key or button press
    int         (*capturing)(void);                 // action being captured, or -1
    void        (*reset_bindings)(void);
    int         (*get_deadzone)(void);              // percent
    void        (*set_deadzone)(int percent);
    void        (*operator_menu)(void);
    void        (*restart)(void);
    void        (*quit)(void);
    int         (*monitor_count)(void);
    const char* (*monitor_name)(int index);
    int         (*gpu_count)(void);
    const char* (*gpu_name)(int index);
    int         (*running_region)(void);            // the region the board booted with
    const char* (*save_memory_info)(void);          // where the arcade's save memory goes
};

void ui_init(const UiHost* host);
void ui_menu_toggle(int device);
void ui_menu_set_page(int page);
int  ui_menu_visible(void);      // drawn (including the closing animation)
int  ui_menu_wants_pause(void);  // the game should stay paused
void ui_key(int key, int device);
void ui_toast(const char* msg);
void ui_update(float dt);
void ui_draw(float vw, float vh, float fps);
float ui_backdrop(void);         // 0..1: how much to darken and blur the game behind the menu
