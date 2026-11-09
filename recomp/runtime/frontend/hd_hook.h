// Hook for a high-resolution renderer: it draws the game picture straight into the window's back buffer.
//
// The presenter (fe_d3d11.cpp) calls the HdDrawFn once per displayed frame, after clearing the back buffer,
// when a function is set, settings.enhanced is on and Direct3D 11 is in use. The function draws the picture
// into the given viewport (x, y, w, h in back-buffer pixels: the 4:3 / stretched / whole-number area) of
// `target` and returns true; or returns false and the normal 512x448 software picture is drawn instead.
// The screen filter's scanlines, the menu backdrop (darken + blur), the menu, toasts and the frame-rate
// counter are drawn on top afterwards. The presenter re-binds its own state after the call, so the hook may
// leave any pipeline state behind (but must not keep `target` bound as a shader resource).
//
// While the pause menu is open the game is paused, but frames keep being presented (the hook is still
// called: draw the last frame again).
//
// The device can change: Direct3D is restarted after a device loss, and is shut down (nullptr) if it fails
// for good and the window falls back to GDI. Compare fe_d3d_device() with the device your resources were
// made on, each call.
#pragma once
#include <d3d11.h>

ID3D11Device* fe_d3d_device();          // nullptr when Direct3D 11 isn't in use (headless, or GDI fallback)
ID3D11DeviceContext* fe_d3d_context();  // nullptr when Direct3D 11 isn't in use

using HdDrawFn = bool (*)(ID3D11RenderTargetView* target, int x, int y, int w, int h);
void fe_set_hd_draw(HdDrawFn fn);       // nullptr removes it

// Settings the renderer needs (menu: Display page; saved in settings.ini).
bool fe_setting_enhanced();             // Graphics: Enhanced (true) / Original arcade (false)
int  fe_setting_quality();              // 0 Auto, 1 Performance, 2 Balanced, 3 High, 4 Ultra, 5 Extreme
int  fe_setting_quality_scale();        // render scale for that preset: 0 (Auto), 2, 3, 4, 6, 8
int  fe_setting_filter();               // 0 Sharp, 1 Smooth, 2 Scanlines (scanlines are drawn on top for you)
int  fe_setting_aspect();               // 0 Arcade 4:3, 1 Stretch, 2 4:3 whole-number scale (already in x/y/w/h)
