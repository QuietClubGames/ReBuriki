// The 2D picture split around the 3D layer, for the high-resolution renderer (video.cpp -> hd3d.cpp).
#pragma once
#include <stdint.h>

constexpr int HD_WIDE_MARGIN = 200;                       // extra 2D columns each side (enough for 21:9)
constexpr int HD_LAYER_MAX_W = 512 + 2 * HD_WIDE_MARGIN;

struct HdLayers {
    int width, margin;        // the layers are `width` x 448; the arcade's 512 columns start at `margin`
                              // (512 and 0 normally; HD_LAYER_MAX_W and HD_WIDE_MARGIN for widescreen)
    bool arena;               // a perspective floor is on screen (fights and the style intros)
    const uint32_t* below;    // 0x00RRGGBB: everything the hardware draws before the 3D
    const uint32_t* over;     // 0xAARRGGBB: everything drawn after the 3D. A = how it combines:
                              //   0xFF opaque, 0xC0 50% blend, 0x80 additive, 0x00 nothing
    const uint32_t* pal_3d;   // 0x10000 entries 0x00RRGGBB: the 16-brightness 3D palette
    uint32_t palbase;         // 0x800 when tcram 0x4C bit 16 selects the upper 3D palette half
    int xscroll;              // 3D buffer horizontal offset (MAME fbscroll)
    bool enable3d;            // false when fbcontrol[0] bit 0 hides the 3D
};

// Widescreen: draw the 2D layers with HD_WIDE_MARGIN extra columns each side from the next frame on.
void video_set_wide_layers(bool on);

// Fill in this frame's layers. false = the frame can't be split (screen off, etc.): show the
// normal 512x448 picture instead.
bool video_hd_layers(HdLayers* out);

// Install the high-resolution renderer into the front end's presenter (hd3d.cpp).
void hd3d_install(void);
