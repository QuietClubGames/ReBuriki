// The 2D picture split around the 3D layer, for the high-resolution renderer (video.cpp -> hd3d.cpp).
#pragma once
#include <stdint.h>

struct HdLayers {
    const uint32_t* below;    // 512x448 0x00RRGGBB: everything the hardware draws before the 3D
    const uint32_t* over;     // 512x448 0xAARRGGBB: everything drawn after the 3D. A = how it combines:
                              //   0xFF opaque, 0xC0 50% blend, 0x80 additive, 0x00 nothing
    const uint32_t* pal_3d;   // 0x10000 entries 0x00RRGGBB: the 16-brightness 3D palette
    uint32_t palbase;         // 0x800 when tcram 0x4C bit 16 selects the upper 3D palette half
    int xscroll;              // 3D buffer horizontal offset (MAME fbscroll)
    bool enable3d;            // false when fbcontrol[0] bit 0 hides the 3D
};

// Fill in this frame's layers. false = the frame can't be split (screen off, etc.): show the
// normal 512x448 picture instead.
bool video_hd_layers(HdLayers* out);

// Install the high-resolution renderer into the front end's presenter (hd3d.cpp).
void hd3d_install(void);
