// Menu art: the ten fighting-style portraits from the attract mode's style-intro screens,
// built at start-up from the player's own sprite ROMs. Only these recipes are stored here
// (tile numbers and positions, as the game's sprite list has them), never any artwork.
//
// How they were found (2026-11-10): BURIKI_SPRITES=<frame> dumps the sprite list on a style
// intro. The portrait is the chained sprite block(s) at depth 0x2F5 (the stale entries left
// from earlier screens sit at 0x7FF, which the hardware never draws). All ten use palette bank 4,
// which the game loads with the same 256 colours from cart ROM offset 0x3C3440.
#include "menu_art.h"
#include "hw.h"
#include "video.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

struct Recipe {
    const char* style;
    SpritePiece pieces[4];
    int count;
};

// Positions are the sprite entries' screen positions (only their differences matter).
const Recipe kRecipes[] = {
    {"Karate",                 {{0x24D58A, 0x100, 0x000, 16, 16}, {0x24D78A, 0x100, 0x100, 16, 12}}, 2},
    {"Sumo Wrestling",         {{0x24D90A, 0x000, 0x000, 16, 16}, {0x24DBAA, 0x000, 0x100, 16, 12}}, 2},
    {"Judo",                   {{0x24DDA2, 0x0D0, 0x000, 16, 16}, {0x24E002, 0x0D0, 0x100, 16, 12}}, 2},
    {"Professional Wrestling", {{0x24E1CA, 0x000, 0x000, 16, 16}, {0x24E3CA, 0x100, 0x000, 11, 16},
                                {0x24E52A, 0x000, 0x100, 16, 12}, {0x24E6AA, 0x100, 0x100, 11, 12}}, 4},
    {"Boxing",                 {{0x24E7B2, 0x0E0, 0x000, 16, 16}, {0x24E9F2, 0x0E0, 0x100, 16, 12}}, 2},
    {"Tae Kwon Do",            {{0x24EBA2, 0x010, 0x000, 15, 16}, {0x24ED82, 0x010, 0x100, 15, 12}}, 2},
    {"Tai Chi Martial Arts",   {{0x24EEEA, 0x0E0, 0x000, 16, 16}, {0x24F10A, 0x0E0, 0x100, 16, 12}}, 2},
    {"(loop 2, third style)",  {{0x24F2A2, 0x000, 0x000, 15, 16}, {0x24F482, 0x000, 0x100, 15, 12}}, 2},
    {"Wrestling",              {{0x24F5EA, 0x0E0, 0x000, 16, 16}, {0x24F82A, 0x0E0, 0x100, 16, 12}}, 2},
    {"Muay-Thai",              {{0x24F9DA, 0x000, 0x000, 16, 16}, {0x24FBFA, 0x000, 0x100, 16, 12}}, 2},
};
constexpr uint32_t kPaletteCartOffset = 0x3C3440;   // the style intros dim entries 209-223 to ~62%

std::vector<MenuArt> g_art;

// Trim fully transparent borders so the portrait fills its frame.
void trim(MenuArt& a) {
    int x0 = a.w, y0 = a.h, x1 = -1, y1 = -1;
    for (int y = 0; y < a.h; y++)
        for (int x = 0; x < a.w; x++)
            if (a.pixels[(size_t)y * a.w + x] >> 24) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 < 0) return;
    std::vector<uint32_t> t((size_t)(x1 - x0 + 1) * (y1 - y0 + 1));
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) t[(size_t)(y - y0) * (x1 - x0 + 1) + (x - x0)] = a.pixels[(size_t)y * a.w + x];
    a.pixels.swap(t);
    a.w = x1 - x0 + 1; a.h = y1 - y0 + 1;
}

void save_bmp(const MenuArt& a, const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t img = (uint32_t)(a.w * a.h * 4);
    uint8_t h[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) h[at + i] = (uint8_t)(v >> (8 * i)); };
    put32(2, 54 + img); put32(10, 54); put32(14, 40); put32(18, (uint32_t)a.w); put32(22, (uint32_t)-a.h);
    h[26] = 1; h[28] = 32; put32(34, img);
    fwrite(h, 1, 54, f);
    fwrite(a.pixels.data(), 1, img, f);
    fclose(f);
}

} // namespace

void menu_art_build(void) {
    g_art.clear();
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = hw_cart_read32(kPaletteCartOffset + i * 4);
    for (const Recipe& r : kRecipes) {
        MenuArt a;
        a.name = r.style;
        if (!video_build_sprite_picture(r.pieces, r.count, pal, a.pixels, a.w, a.h)) continue;
        trim(a);
        g_art.push_back(std::move(a));
    }
    fprintf(stderr, "[menu art] built %zu portraits from the sprite ROMs\n", g_art.size());
    if (const char* dir = getenv("BURIKI_DUMP_ART")) {   // diagnostics: save them as .bmp
        for (size_t i = 0; i < g_art.size(); i++) {
            char name[64];
            snprintf(name, sizeof(name), "/portrait_%02zu.bmp", i + 1);
            save_bmp(g_art[i], std::string(dir) + name);
        }
    }
}

const std::vector<MenuArt>& menu_art_all(void) { return g_art; }
