// HNG64 video output for the Buriki One recomp (video.cpp).
#pragma once
#include <stdint.h>
#include <vector>

struct VideoConfig {
    bool headless = false;          // no window (for automated runs)
    bool throttle = true;           // limit to the real ~59.43 Hz when showing a window
    int scale = 0;                  // window N x 448 lines tall for this run (0 = the size in settings.ini)
    const char* dump_dir = nullptr; // where to save frames as .bmp
    std::vector<uint32_t> dump_frames;  // frame numbers to save
    uint32_t dump_every = 0;        // also save every Nth frame (0 = off)
};

extern VideoConfig g_video_cfg;

// One chained sprite block: `cols` x `rows` 16x16 tiles numbered from raw_tile (the sprite
// entry's tile field) at position x, y.
struct SpritePiece { uint32_t raw_tile; int x, y, cols, rows; };
// Build a picture from sprite blocks and a 256-colour palette (0x00RRGGBB). Result: 0xAARRGGBB,
// colour 0 transparent. Needs video_init() first (the sprite ROMs).
bool video_build_sprite_picture(const SpritePiece* pieces, int count, const uint32_t* pal256,
                                std::vector<uint32_t>& out, int& w, int& h);

// The finished 512x448 picture (0x00RRGGBB), updated every frame.
const uint32_t* video_framebuffer();
constexpr int VIDEO_W = 512, VIDEO_H = 448;
