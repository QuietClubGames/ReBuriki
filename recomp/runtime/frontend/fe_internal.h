// Shared between the front-end files (window, Direct3D 11 presenter, overlay renderer, menu host, input).
//
// Parts adapted from ReLayer (port/pc/d3d11_internal.h, d3d11_present.h, video.h, menu_host.h),
// Copyright (c) 2026 The ReLayer project, MIT License (see LICENSE-ReLayer.txt in this folder).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <string>

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                      ID3DBlob**, ID3DBlob**);

void fe_log(const char* fmt, ...);
std::string fe_path(const char* name);          // a file next to the exe

// ---- Direct3D 11 presenter (fe_d3d11.cpp) ----
int  d3d_init(HWND hwnd, int refresh_hz, int gpu);   // gpu: 0 automatic, else 1-based adapter. 0 = failed (use GDI)
void d3d_shutdown(void);
int  d3d_set_exclusive(int on, HMONITOR monitor);    // exclusive full screen on that monitor; 0 if refused
int  d3d_gpu_count(void);
const char* d3d_gpu_name(int index);
int  d3d_present(const uint32_t* rgb, int w, int h); // 0 on failure (caller restarts Direct3D or falls back to GDI)
void d3d_set_sync(int interval);                     // 0 = don't wait for the display, k = every k-th refresh
int  d3d_vsync(void);                                // 1 if d3d_present() waits for the display
void d3d_request_backbuffer_save(const char* path);
ID3D11Device*        d3d_dev(void);
ID3D11DeviceContext* d3d_ctx(void);
D3DCompileFn         d3d_compiler(void);

extern float g_blit_dim, g_blit_blur, g_blit_scan;   // backdrop behind the menu, scanline strength
extern int g_blit_smooth;                            // bilinear instead of sharp pixels
extern int g_aspect_mode;                            // ASPECT_*

// Where the picture goes inside a cw x ch client area: the arcade monitor is 4:3, so keep that shape and
// leave black bars on the sides (or top/bottom) instead of stretching.
static inline void picture_rect(int cw, int ch, int* x, int* y, int* w, int* h) {
    if (g_aspect_mode == 1) { *x = 0; *y = 0; *w = cw; *h = ch; return; }      // stretch to fill
    if (cw * 3 >= ch * 4) { *h = ch; *w = ch * 4 / 3; }
    else { *w = cw; *h = cw * 3 / 4; }
    if (g_aspect_mode == 2 && *h >= 448) {           // 4:3 at a whole-number multiple of the 448 lines
        *h = (*h / 448) * 448;
        *w = *h * 4 / 3;
    }
    *x = (cw - *w) / 2;
    *y = (ch - *h) / 2;
}

// Widescreen (ASPECT_WIDE): the area handed to the high-resolution renderer. As tall as the 4:3 picture and
// as wide as the window allows, up to 21:9. (picture_rect() still gives the 4:3 area for everything else.)
static inline void picture_rect_wide(int cw, int ch, int* x, int* y, int* w, int* h) {
    picture_rect(cw, ch, x, y, w, h);
    int ww = *h * 64 / 27;                           // 21:9
    if (ww > cw) ww = cw;
    if (ww > *w) { *w = ww; *x = (cw - ww) / 2; }
}

// ---- overlay renderer (fe_ui_d3d11.cpp) ----
void ui_d3d_render(int w, int h);
void ui_d3d_release(void);
int  ui_d3d_failed(void);
extern float g_ui_fps;

// ---- window (fe_window.cpp) ----
void video_apply_display(int mode, int window_size, int monitor);   // DISPLAY_*
void video_window_size(int index, int* w, int* h);                 // client size of a window-size choice, for the picture shape in use
int  video_monitor_count(void);
const char* video_monitor_name(int index);
int  video_pop_key(void);              // next queued key press: VK code, | KEY_REPEAT for auto-repeat; 0 if none
int  video_menu_drawable(void);        // 0: no Direct3D, the menu can't be shown
void video_set_pacing(int pacing);     // PACING_*
bool video_focused(void);
#define KEY_REPEAT 0x10000

// ---- controls (fe_input.cpp) ----
// A player's controls. The first ten are the arcade's own. The game's L and R buttons (FB_B1, FB_B2) step
// left and right in a fight and confirm on every other screen; FB_SELECT and FB_OTHER are those same two
// buttons on the other screens, so that the two jobs can be on different keys and buttons (fe_read_host).
enum { FB_UP, FB_DOWN, FB_LEFT, FB_RIGHT, FB_B1, FB_B2, FB_B3, FB_B4, FB_START, FB_COIN,
       FB_SELECT, FB_OTHER, FB_COUNT, FB_ARCADE = FB_SELECT };
enum { SRC_NONE = 0, SRC_DUP, SRC_DDOWN, SRC_DLEFT, SRC_DRIGHT, SRC_START, SRC_BACK, SRC_LS, SRC_RS,
       SRC_LB, SRC_RB, SRC_A, SRC_B, SRC_X, SRC_Y, SRC_LT, SRC_RT, SRC_COUNT };
void pad_init(void);
void pad_poll(void);                       // once per displayed frame
int  pad_player_connected(int player);
uint32_t pad_held_all(void);               // SRC_* bits held on any connected pad
int  pad_stick_dir(void);                  // left stick on any pad: bit0 up, 1 down, 2 left, 3 right
// remappable actions: player * 16 + FB_*
const char* bind_pad_name(int action);     // the first controller button on it
int  bind_pad_count(int action);           // how many controller buttons it has
const char* bind_pad_at(int action, int i);
const char* bind_key_name(int action);
int  bind_toggle_pad(int action, int src); // 1 added, 0 taken off, -1 no room for another
void bind_clear_pad(int action);
int  bind_set_key(int action, int vk);     // 0 if that key can't be used
int  bind_stick(int player);               // which stick is the arcade stick: 0 left, 1 right, 2 either
void bind_set_stick(int player, int v);
void bind_defaults(void);
int  bind_deadzone_percent(void);
void bind_set_deadzone_percent(int p);
void bind_save(void);
const char* pad_src_name(int src);
bool keys_bound_held(void);                // any key a player uses is held (for the after-menu block)

// ---- menu host (fe_menu_host.cpp) ----
void menu_host_init(void);
void menu_host_frame(float dt);            // once per displayed frame (also while paused)
int  menu_host_input_blocked(void);        // 1 while the game must not see controller input
void menu_host_apply(void);
int  menu_host_capturing(void);
void menu_host_cancel_capture(void);
extern int g_test_pulse;                   // frames left of a simulated operator Test-button press
extern bool g_no_save;                     // tests: don't write settings.ini / controls.ini
