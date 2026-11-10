// Menu art built from the game's own sprite ROMs at start-up (menu_art.cpp).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct MenuArt {
    std::string name;               // fighting style
    int w = 0, h = 0;
    std::vector<uint32_t> pixels;   // 0xAARRGGBB, row by row
};

void menu_art_build(void);                    // after hw_init + video_init
const std::vector<MenuArt>& menu_art_all(void);
