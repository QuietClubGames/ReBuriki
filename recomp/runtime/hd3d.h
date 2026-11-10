// High-resolution 3D: the triangles the 3D hardware drew this frame, recorded by video3d.cpp
// in full precision so hd3d.cpp can redraw them on the GPU at any resolution.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct HdVert {
    float x, y;     // window position in the 512x512 3D buffer (fractional, as MAME computes it)
    float z;        // depth 0..1 (MAME window z; smaller = nearer, depth test is "less")
    float w;        // clip-space w (> 0 after clipping), for perspective-correct interpolation
    float s, t;     // texture coordinates (0..1 per 1024 texels, before scroll/page math)
    float light;    // lighting value 0..255 (selects one of 16 brightness palettes: light / 16)
};

struct HdTri {
    HdVert v[3];
    uint8_t flat;          // 1 = flat shaded (no texture, no light): colour = palOffset + colorIndex
    uint8_t tex4bpp, texIndex, blend;
    uint16_t texPageSmall, colorIndex;
    uint32_t palOffset;
    uint16_t texscrollx, texscrolly, tex_mask_x, tex_mask_y;
    int16_t clip_l, clip_t, clip_r, clip_b;   // inclusive scissor in buffer pixels (MAME cliprect)
};

// Copy the triangles drawn since the 3D buffer was last cleared (= this frame's 3D picture)
// for display. Called when a frame is finished, before the vblank clear.
void video3d_hd_snapshot(void);
// Triangles drawn since the last 3D clear (diagnostics).
void video3d_live_tri_count(size_t* n);
// The last snapshot, in draw order. Stays valid while the game is paused.
const HdTri* video3d_hd_tris(size_t* count);
// The 3D texture ROM (16 pages of 1024x1024 bytes) for uploading to the GPU.
const uint8_t* video3d_texture_rom(size_t* size);
