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

// The finished 512x448 picture (0x00RRGGBB), updated every frame.
const uint32_t* video_framebuffer();
constexpr int VIDEO_W = 512, VIDEO_H = 448;
