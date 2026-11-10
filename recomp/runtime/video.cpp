// HNG64 2D video: background layers ("tilemaps"), sprites, palette/fades and final mixing.
//
// This is a port of MAME's src/mame/snk/hng64_v.cpp and hng64_sprite.ipp (LGPL-2.1+,
// copyright David Haywood, Angelo Salese, ElSemi, Andrew Gardner), rewritten without MAME's
// tilemap/gfx/bitmap classes. Function and variable names follow MAME so the two can be
// compared side by side. Behaviour that MAME marks as a guess is kept as is.
//
// The 3D layer is composited here too (see draw_3d_layer); its renderer lives in video3d.cpp.
#include "video.h"
#include "hw.h"
#include "frontend/frontend.h"
#include "frontend/hd_hook.h"
#include "video_layers.h"
#include "hd3d.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

VideoConfig g_video_cfg;

// 3D colour buffer (512x512, MAME format llll appp pppp pppp), filled by video3d.cpp.
extern uint16_t g_color_buffer_3d[512 * 512];

namespace {

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int16_t s16; typedef int32_t s32;

// ---- graphics ROMs ----------------------------------------------------------------------
std::vector<u8> g_scrtile;   // 64 MB tilemap characters (after hng64_reorder)
std::vector<u8> g_sprtile;   // 64 MB sprite characters

struct Layout {
    int w, h, bpp;
    u32 charinc;                // bits per character
    u32 xoffs[16], yoffs[16];   // bit offsets (MAME gfx_layout)
};

const Layout hng64_8x8x4_tilelayout = {8, 8, 4, 8 * 32,
    {24, 28, 8, 12, 16, 20, 0, 4}, {0 * 32, 1 * 32, 2 * 32, 3 * 32, 4 * 32, 5 * 32, 6 * 32, 7 * 32}};
const Layout hng64_8x8x8_tilelayout = {8, 8, 8, 16 * 32,
    {24, 8, 16, 0, 256 + 24, 256 + 8, 256 + 16, 256 + 0}, {0 * 32, 1 * 32, 2 * 32, 3 * 32, 4 * 32, 5 * 32, 6 * 32, 7 * 32}};
const Layout hng64_16x16x4_tilelayout = {16, 16, 4, 32 * 32,
    {24, 28, 8, 12, 16, 20, 0, 4, 256 + 24, 256 + 28, 256 + 8, 256 + 12, 256 + 16, 256 + 20, 256 + 0, 256 + 4},
    {0 * 32, 1 * 32, 2 * 32, 3 * 32, 4 * 32, 5 * 32, 6 * 32, 7 * 32, 16 * 32, 17 * 32, 18 * 32, 19 * 32, 20 * 32, 21 * 32, 22 * 32, 23 * 32}};
const Layout hng64_16x16x8_tilelayout = {16, 16, 8, 64 * 32,
    {24, 8, 16, 0, 256 + 24, 256 + 8, 256 + 16, 256 + 0, 1024 + 24, 1024 + 8, 1024 + 16, 1024 + 0, 1280 + 24, 1280 + 8, 1280 + 16, 1280 + 0},
    {0 * 32, 1 * 32, 2 * 32, 3 * 32, 4 * 32, 5 * 32, 6 * 32, 7 * 32, 16 * 32, 17 * 32, 18 * 32, 19 * 32, 20 * 32, 21 * 32, 22 * 32, 23 * 32}};
const Layout hng64_16x16x4_spritelayout = {16, 16, 4, 16 * 64,
    {56, 60, 24, 28, 48, 52, 16, 20, 40, 44, 8, 12, 32, 36, 0, 4},
    {0 * 64, 1 * 64, 2 * 64, 3 * 64, 4 * 64, 5 * 64, 6 * 64, 7 * 64, 8 * 64, 9 * 64, 10 * 64, 11 * 64, 12 * 64, 13 * 64, 14 * 64, 15 * 64}};
const Layout hng64_16x16x8_spritelayout = {16, 16, 8, 32 * 64,
    {56, 24, 48, 16, 40, 8, 32, 0, 1024 + 56, 1024 + 24, 1024 + 48, 1024 + 16, 1024 + 40, 1024 + 8, 1024 + 32, 1024 + 0},
    {0 * 64, 1 * 64, 2 * 64, 3 * 64, 4 * 64, 5 * 64, 6 * 64, 7 * 64, 8 * 64, 9 * 64, 10 * 64, 11 * 64, 12 * 64, 13 * 64, 14 * 64, 15 * 64}};

struct Gfx {
    const u8* data;
    const Layout* lay;
    u32 elements;
    u32 granularity;   // colours per palette bank
    u32 colors;        // number of banks
};

// MAME gfx_hng64 order: 0-3 tilemap characters, 4-5 sprites.
Gfx g_gfx[6];

inline u8 gfx_pixel(const Gfx& g, u32 code, int x, int y) {
    u32 bit = code * g.lay->charinc + g.lay->yoffs[y] + g.lay->xoffs[x];
    u8 b = g.data[bit >> 3];
    if (g.lay->bpp == 8) return b;
    return (bit & 4) ? (b & 0x0F) : (b >> 4);   // MAME numbers bits from the MSB
}

std::vector<u8> read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) hw_fatal("can't open %s", path.c_str());
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<u8> d((size_t)n);
    if (fread(d.data(), 1, d.size(), f) != d.size()) hw_fatal("can't read %s", path.c_str());
    fclose(f);
    return d;
}

// ROM_LOAD16_BYTE / ROM_LOAD32_BYTE: file bytes go to every `stride`th byte from `offset`.
void load_interleaved(std::vector<u8>& region, const std::string& path, size_t offset, size_t stride) {
    auto d = read_file(path);
    for (size_t i = 0; i < d.size(); i++) region[offset + i * stride] = d[i];
}

// MAME hng64_reorder: two 4bpp tiles share each 8bpp tile; put them next to each other.
void hng64_reorder(std::vector<u8>& r) {
    const size_t tilesize = 4 * 8, half = r.size() / 2;
    std::vector<u8> buffer(r.size());
    for (size_t i = 0; i < half; i += tilesize) {
        memcpy(&buffer[i * 2 + tilesize], &r[i], tilesize);
        memcpy(&buffer[i * 2], &r[i + half], tilesize);
    }
    r.swap(buffer);
}

// ---- hardware state views ---------------------------------------------------------------
u32* m_videoram;    // 0x20100000
u32* m_videoregs;   // 0x20190000
u32* m_tcram;       // 0x20208000
u32* m_paletteram;  // 0x20200000
u32* m_spriteram;   // 0x20000000
u32* m_spriteregs;  // 0x20010000

u8 fbcontrol(int i) { return (u8)(hw_phys_words(0x30000000)[0] >> (24 - 8 * i)); }

// ---- palettes ---------------------------------------------------------------------------
u32 pal_main[0x1000], pal_fade0[0x1000], pal_fade1[0x1000];
u32 pal_3d[0x10000];   // 16 brightness copies of the palette, for the 3D layer
constexpr u32 BLACK = 0;

inline u32 rgb(int r, int g, int b) { return (u32)r << 16 | (u32)g << 8 | (u32)b; }

void set_palette_entry_with_faderegs(u32* pal, int entry, int r, int g, int b, u32 rgbfade, u8 r_mode, u8 g_mode, u8 b_mode) {
    auto apply = [](int c, int fade, u8 mode) {
        if (mode == 1) { c += fade; if (c > 255) c = 255; }        // additive
        else if (mode == 3) { c -= fade; if (c < 0) c = 0; }       // subtractive
        return c;
    };
    pal[entry] = rgb(apply(r, (rgbfade >> 16) & 0xff, r_mode), apply(g, (rgbfade >> 8) & 0xff, g_mode),
                     apply(b, rgbfade & 0xff, b_mode));
}

void set_single_palette_entry(int entry, int r, int g, int b) {
    pal_main[entry] = rgb(r, g, b);
    u32 m = m_tcram[0x14 / 4];
    set_palette_entry_with_faderegs(pal_fade0, entry, r, g, b, m_tcram[0x18 / 4], (m >> 10) & 3, (m >> 8) & 3, (m >> 6) & 3);
    set_palette_entry_with_faderegs(pal_fade1, entry, r, g, b, m_tcram[0x1c / 4], (m >> 4) & 3, (m >> 2) & 3, (m >> 0) & 3);
    for (int intensity = 0; intensity < 0x10; intensity++) {
        int nr = r + (intensity << 2), ng = g + (intensity << 2), nb = b + (intensity << 2);
        if (nr > 255) nr = 255;
        if (ng > 255) ng = 255;
        if (nb > 255) nb = 255;
        set_palette_entry_with_faderegs(pal_3d, intensity * 0x1000 + entry, nr, ng, nb, m_tcram[0x18 / 4], (m >> 10) & 3, (m >> 8) & 3, (m >> 6) & 3);
    }
}

void update_palette_entry(int entry) {
    const u32 data = m_paletteram[entry];
    s16 r = (data >> 16) & 0xff, g = (data >> 8) & 0xff, b = data & 0xff;
    for (int i = 0; i < 8; i++) {
        const u32 tcdata = m_tcram[0x28 / 4 + i];
        if (((tcdata & 0x0f000000) >> 24) == (u32)(entry >> 8)) {
            const u8 bmod = (tcdata >> 16) & 0xff, gmod = (tcdata >> 8) & 0xff, rmod = tcdata & 0xff;
            switch ((m_tcram[0x24 / 4] >> (i << 1)) & 3) {
            case 2: r = r + rmod > 255 ? 255 : r + rmod; g = g + gmod > 255 ? 255 : g + gmod; b = b + bmod > 255 ? 255 : b + bmod; break;
            case 3: r = r - rmod < 0 ? 0 : r - rmod; g = g - gmod < 0 ? 0 : g - gmod; b = b - bmod < 0 ? 0 : b - bmod; break;
            }
        }
    }
    set_single_palette_entry(entry, r, g, b);
}

// MAME updates entries on palette/tcram writes; recomputing everything once a frame gives
// the same result for a whole-frame renderer.
void update_all_palettes() {
    for (int i = 0; i < 0x1000; i++) update_palette_entry(i);
}

inline u32 add_blend_r32(u32 d, u32 s) {
    u32 r = ((d >> 16) & 0xff) + ((s >> 16) & 0xff), g = ((d >> 8) & 0xff) + ((s >> 8) & 0xff), b = (d & 0xff) + (s & 0xff);
    return rgb(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}
inline u32 alpha_blend_r32(u32 d, u32 s, u32 level) {
    auto ch = [&](int sh) { return ((((s >> sh) & 0xff) * level + ((d >> sh) & 0xff) * (256 - level)) >> 8) & 0xff; };
    return rgb(ch(16), ch(8), ch(0));
}

// ---- frame buffers ------------------------------------------------------------------------
u32 g_bitmap[VIDEO_W * VIDEO_H];

// For the high-resolution renderer (video_layers.h): the picture split around the 3D layer.
// g_below = everything drawn before the 3D; g_over = everything drawn after it, recorded with
// how each pixel combines with what's behind (alpha byte: FF opaque, C0 50% blend, 80 add).
u32 g_below[VIDEO_W * VIDEO_H];
u32 g_over[VIDEO_W * VIDEO_H];
bool g_layers_ok = false;
u32* g_draw = g_bitmap;        // where tilemaps and sprites draw
bool g_over_mode = false;      // drawing into g_over

constexpr u32 OVER_OPAQUE = 0xFF000000u, OVER_HALF = 0xC0000000u, OVER_ADD = 0x80000000u;

// mode 1 = opaque, 2 = additive, 3 = 50% blend (MAME HNG64_ROZ_PLOT_PIXEL / mixsprites).
inline void plot(u32* d, u32 c, int mode);
u16 m_sprite_bitmap[VIDEO_W * VIDEO_H];
u16 m_sprite_zbuffer[VIDEO_W * VIDEO_H];

inline void plot(u32* d, u32 c, int mode) {
    if (!g_over_mode) {
        if (mode == 1) *d = c;
        else if (mode == 2) *d = add_blend_r32(*d, c);
        else *d = alpha_blend_r32(*d, c, 0x80);
        return;
    }
    // In-front layer: combine with what's already in it; anything still depending on the
    // picture behind (3D, background) keeps its blend mode for the GPU compositor.
    const u32 a = *d & 0xFF000000u, rgbv = *d & 0xFFFFFFu;
    if (mode == 1) *d = OVER_OPAQUE | c;
    else if (mode == 2) *d = (a == OVER_OPAQUE || a == OVER_ADD) ? (a | add_blend_r32(rgbv, c))
                           : (a == OVER_HALF) ? (OVER_HALF | add_blend_r32(rgbv, c)) : (OVER_ADD | c);
    else *d = (a == OVER_OPAQUE) ? (OVER_OPAQUE | alpha_blend_r32(rgbv, c, 0x80))
            : (a == 0) ? (OVER_HALF | c) : (OVER_HALF | alpha_blend_r32(rgbv, c, 0x80));
}

struct Rect { int min_x, max_x, min_y, max_y; };
Rect g_visarea = {0, VIDEO_W - 1, 0, VIDEO_H - 1};
bool m_screen_dis = false;

// ---- tilemaps -----------------------------------------------------------------------------
u16 get_tileregs(int tm) { return (m_videoregs[0x02 + ((tm >> 1) & 1)] >> ((tm & 1) ? 0 : 16)) & 0xffff; }
u16 get_scrollbase(int tm) { return (m_videoregs[0x04 + ((tm >> 1) & 1)] >> ((tm & 1) ? 0 : 16)) & 0x3fff; }

int get_blend_mode(int tm) {
    u8 blendmode = 1;
    if ((get_tileregs(tm) >> 5) & 1) { if ((m_tcram[0x0c / 4] >> 2) & 1) blendmode = 2; }
    else { if ((m_tcram[0x0c / 4] >> 26) & 1) blendmode = 2; }
    return blendmode;
}

// One of MAME's 3 tilemaps per layer: 8x8 (128x128 tiles), 16x16 (128x128), 16x16 alt (256x64).
struct TilemapKind { int tile_w, tile_h, cols, rows, size; };
const TilemapKind TM_8x8 = {8, 8, 128, 128, 0}, TM_16x16 = {16, 16, 128, 128, 1}, TM_16x16_ALT = {16, 16, 256, 64, 1};

// The decoded MAME get_tile_info for the last tile looked up. Neighbouring pixels are usually
// in the same tile, so this skips re-decoding the tile entry for each pixel. (Speed only: the
// result is identical.) One per drawing call, so it's safe with lines drawn in parallel.
struct TileCache {
    u32 index = 0xFFFFFFFFu;
    const Gfx* g = nullptr;
    u32 code = 0, colorbase = 0;
    int flip = 0;
};

// MAME get_tile_info + pixmap lookup. Returns false for a transparent pixel.
inline bool tilemap_pixel(int tm, const TilemapKind& k, u32 px, u32 py, u16& pen, TileCache& tc) {
    const u32 col = px / k.tile_w, row = py / k.tile_h;
    const u32 tile_index = row * k.cols + col;
    if (tile_index != tc.index) {
        tc.index = tile_index;
        u32 tileno = m_videoram[tile_index + (tm << 14)];
        const u32 pal = (tileno & 0xff000000) >> 24;
        tc.flip = (tileno & 0x00c00000) >> 22;
        if ((tileno >> 21) & 1)
            if ((m_videoregs[0x01] >> 16) & 1) tileno = (tileno & m_videoregs[0x0b]) | m_videoregs[0x0c];
        tileno &= 0x1fffff;

        const bool bpp8 = (get_tileregs(tm) >> 10) & 1;
        int gfxn; u32 code, color;
        if (k.size == 0) { if (bpp8) { gfxn = 1; code = tileno >> 1; color = pal >> 4; } else { gfxn = 0; code = tileno; color = pal; } }
        else             { if (bpp8) { gfxn = 3; code = tileno >> 3; color = pal >> 4; } else { gfxn = 2; code = tileno >> 2; color = pal; } }
        tc.g = &g_gfx[gfxn];
        tc.code = code % tc.g->elements;
        tc.colorbase = tc.g->granularity * (color % tc.g->colors);
    }
    const Gfx& g = *tc.g;
    int x = px % k.tile_w, y = py % k.tile_h;
    if (tc.flip & 1) x = k.tile_w - 1 - x;    // TILE_FLIPX
    if (tc.flip & 2) y = k.tile_h - 1 - y;    // TILE_FLIPY
    const u8 p = gfx_pixel(g, tc.code, x, y);
    if (p == 0) return false;                  // transparent pen 0
    pen = (u16)(tc.colorbase + p);
    return true;
}

void tilemap_draw_roz_core_line(const Rect& cliprect, int tm, const TilemapKind& k, int wraparound,
                                u8 drawformat, u8 alpha, u8 mosaic, int splitside) {
    int source_line_to_use = cliprect.min_y;
    source_line_to_use = (source_line_to_use / (mosaic + 1)) * (mosaic + 1);

    int xinc, xinc2, yinc, yinc2;
    s32 xtopleft, ytopleft;
    const u16 scrollbase = get_scrollbase(tm);
    const u16 tileregs = get_tileregs(tm);
    const u8 not_line_mode = (tileregs & 0x0800) >> 11;
    auto vr = [&](u32 byte_off) { return (s32)m_videoram[byte_off / 4]; };

    if (not_line_mode) {
        if (m_videoregs[0x00] & 0x04000000) {
            xtopleft = vr(0x40000 + (scrollbase << 4));
            const s32 xalt = vr(0x40004 + (scrollbase << 4));
            const s32 xmiddle = vr(0x40010 + (scrollbase << 4));
            ytopleft = vr(0x40008 + (scrollbase << 4));
            const s32 yalt = vr(0x40018 + (scrollbase << 4));
            const s32 ymiddle = vr(0x4000c + (scrollbase << 4));
            xinc = (xmiddle - xtopleft) / 512;
            yinc = (ymiddle - ytopleft) / 512;
            xinc2 = (xalt - xtopleft) / 512;
            yinc2 = (yalt - ytopleft) / 512;
        } else {
            s32 xmiddle, ymiddle;
            if (m_videoregs[0x00] & 0x00010000) {
                xtopleft = 0; xmiddle = 256 << 16; ytopleft = 0; ymiddle = 256 << 16;
            } else {
                xtopleft = vr(0x40000 + (scrollbase << 4));
                xmiddle = vr(0x40004 + (scrollbase << 4));
                ytopleft = vr(0x40008 + (scrollbase << 4));
                ymiddle = vr(0x4000c + (scrollbase << 4));
                if (splitside == 1) {
                    xtopleft += ((m_videoregs[0x0a] >> 0) & 0xffff) << 16; ytopleft += ((m_videoregs[0x0a] >> 16) & 0xffff) << 16;
                    xmiddle += ((m_videoregs[0x0a] >> 0) & 0xffff) << 16;  ymiddle += ((m_videoregs[0x0a] >> 16) & 0xffff) << 16;
                } else if (splitside == 2) {
                    xtopleft += ((m_videoregs[0x09] >> 0) & 0xffff) << 16; ytopleft += ((m_videoregs[0x09] >> 16) & 0xffff) << 16;
                    xmiddle += ((m_videoregs[0x09] >> 0) & 0xffff) << 16;  ymiddle += ((m_videoregs[0x09] >> 16) & 0xffff) << 16;
                }
            }
            xinc = (xmiddle - xtopleft) / 512;
            yinc = (ymiddle - ytopleft) / 512;
            xinc2 = 0; yinc2 = 0;
        }
    } else {  // line ("floor") mode
        s32 xmiddle, ymiddle;
        if (m_videoregs[0x00] & 0x00010000) {
            xtopleft = 0; xmiddle = 256 << 16; ytopleft = 0; ymiddle = 256 << 16;
        } else {
            u16 line = (u16)source_line_to_use;
            xtopleft = vr(0x40000 + (scrollbase << 4));
            xmiddle = vr(0x40004 + (scrollbase << 4));
            ytopleft = vr(0x40008 + (scrollbase << 4));
            ymiddle = vr(0x4000c + (scrollbase << 4));
            u32 xtopleft2 = vr(0x40000 + (line * 0x10) + (scrollbase << 4));
            u32 xmiddle2 = vr(0x40004 + (line * 0x10) + (scrollbase << 4));
            u32 ytopleft2 = vr(0x40008 + (line * 0x10) + (scrollbase << 4));
            u32 ymiddle2 = vr(0x4000c + (line * 0x10) + (scrollbase << 4));
            auto apply_line = [](u32 base, u32 value, u32 step) -> u32 {
                return ((value & 0xff) == 0x00) ? value : (base + (value & ~u32(0xff)) + step);
            };
            xtopleft = apply_line(xtopleft, xtopleft2, 0);
            xmiddle = apply_line(xmiddle, xmiddle2, 0);
            ytopleft = apply_line(ytopleft, ytopleft2, u32(line) << 16);
            ymiddle = apply_line(ymiddle, ymiddle2, u32(line) << 16);
        }
        if (m_videoregs[0x00] & 0x04000000) {
            xinc = (xmiddle - xtopleft) / 512; yinc = 0; xinc2 = 0; yinc2 = (ymiddle - ytopleft) / 512;
        } else {
            xinc = (xmiddle - xtopleft) / 512; yinc = (ymiddle - ytopleft) / 512; xinc2 = 0; yinc2 = 0;
        }
    }

    u32 startx = xtopleft, starty = ytopleft;
    const int incxx = xinc << 1, incxy = yinc2 << 1, incyx = xinc2 << 1, incyy = yinc << 1;

    const u32* clut;
    if ((get_tileregs(tm) >> 7) & 1) clut = ((get_tileregs(tm) >> 5) & 1) ? pal_fade0 : pal_fade1;
    else clut = pal_main;

    const u32 width = k.cols * k.tile_w, height = k.rows * k.tile_h;
    const u32 xmask = width - 1, ymask = height - 1;
    const u32 widthshifted = width << 16, heightshifted = height << 16;

    startx += cliprect.min_x * incxx + source_line_to_use * incyx;
    starty += cliprect.min_x * incxy + source_line_to_use * incyy;

    u32* dest = &g_draw[cliprect.min_y * VIDEO_W + cliprect.min_x];
    u32 cx = startx, cy = starty;
    int mosaic_counter = 0;
    bool opaque = false;
    u16 datasrc = 0;
    TileCache tc;
    for (int x = cliprect.min_x; x <= cliprect.max_x; x++, dest++, cx += incxx, cy += incxy) {
        u32 sx, sy;
        if (wraparound) { sx = (cx >> 16) & xmask; sy = (cy >> 16) & ymask; }
        else {
            if (!(cx < widthshifted && cy < heightshifted)) continue;
            sx = cx >> 16; sy = cy >> 16;
        }
        if (mosaic_counter == 0) {
            opaque = tilemap_pixel(tm, k, sx, sy, datasrc, tc);
            mosaic_counter = mosaic;
        } else mosaic_counter--;
        if (opaque) {
            // (alpha is always 0x80 on this hardware: plot's mode 3 is the 50% blend)
            if (drawformat >= 1 && drawformat <= 3) plot(dest, clut[datasrc], drawformat);
        }
    }
}

void draw_tilemap(int tm, int line) {
    const int global_dimensions = (m_videoregs[0x00] & 0x03000000) >> 24;
    const u16 tileregs = get_tileregs(tm);
    const u8 mosaic = (tileregs & 0xf000) >> 12;
    const u8 big = (tileregs >> 9) & 1;
    const u8 enable = (tileregs >> 6) & 1;
    const u8 wrap = (tileregs >> 8) & 1;
    if (!enable) return;

    const TilemapKind& k = global_dimensions == 0 ? (big ? TM_16x16 : TM_8x8) : (big ? TM_16x16_ALT : TM_8x8);
    Rect clip = g_visarea;
    clip.min_y = clip.max_y = line;
    if (m_videoregs[0x00] & 1) {   // "split" mode: two halves with different scroll offsets
        Rect r = clip; r.min_x = 256; r.max_x = std::min(511, g_visarea.max_x);
        tilemap_draw_roz_core_line(r, tm, k, wrap, get_blend_mode(tm), 0x80, mosaic, 1);
        r.min_x = 0; r.max_x = 255;
        tilemap_draw_roz_core_line(r, tm, k, wrap, get_blend_mode(tm), 0x80, mosaic, 2);
    } else {
        tilemap_draw_roz_core_line(clip, tm, k, wrap, get_blend_mode(tm), 0x80, mosaic, 0);
    }
}

// ---- sprites --------------------------------------------------------------------------------
void get_tile_details(bool chain, u16 spritenum, u8 xtile, u8 ytile, u8 xsize, u8 ysize, bool xflip, bool yflip,
                      u32& tileno, u16& pal, u8& gfxregion) {
    int offset;
    if (!xflip) offset = !yflip ? (xtile + (ytile * (xsize + 1))) : (xtile + ((ysize - ytile) * (xsize + 1)));
    else        offset = !yflip ? ((xsize - xtile) + (ytile * (xsize + 1))) : ((xsize - xtile) + ((ysize - ytile) * (xsize + 1)));
    const u32 s = chain ? (spritenum + offset) : spritenum;
    tileno = m_spriteram[(s * 8) + 4] & 0x0007ffff;
    pal = (m_spriteram[(s * 8) + 3] & 0x00ff0000) >> 16;
    if ((m_spriteregs[0] >> 23) & 1) gfxregion = 4;
    else { gfxregion = 5; tileno >>= 1; pal &= 0xf; }
    if (!chain) tileno += offset;
}

void zoom_transpen(int gfxn, u32 code, u32 color, int flipx, int flipy, s32 xpos, s32 ypos, s32 dx, u32 dstwidth,
                   u32 zval, bool zrev, bool blend, u16 group, bool checkerboard, u8 mosaic, u8& mosaic_count_x,
                   int curyy, u16& srcpix) {
    const Gfx& g = g_gfx[gfxn];
    code %= g.elements;
    color = g.granularity * (color % g.colors);
    if (blend) color |= 0x8000;
    color |= group;
    if (dstwidth < 1) return;
    if (ypos < g_visarea.min_y || ypos > g_visarea.max_y) return;

    s32 cursrcx = 0;
    if (flipx) { cursrcx = (dstwidth - 1) * dx; dx = -dx; }
    const int row = flipy ? ((15 - curyy) & 0xf) : (curyy & 0xf);
    u16* destptr = &m_sprite_bitmap[ypos * VIDEO_W];
    u16* destzptr = &m_sprite_zbuffer[ypos * VIDEO_W];

    for (s32 curx = 0; curx < (s32)dstwidth; curx++, cursrcx += dx) {
        const int xdrawpos = xpos + curx;
        if (!mosaic) srcpix = gfx_pixel(g, code, (cursrcx >> 16) & 0xf, row);
        else if (mosaic_count_x == 0) { srcpix = gfx_pixel(g, code, (cursrcx >> 16) & 0xf, row); mosaic_count_x = mosaic; }
        else mosaic_count_x--;
        if (checkerboard) {   // like MAME, this overwrites srcpix (matters with mosaic)
            if (curx & 1) { if (!(ypos & 1)) srcpix = 0; }
            else          { if (ypos & 1) srcpix = 0; }
        }
        const u16 pix = srcpix;
        if (xdrawpos > g_visarea.max_x || xdrawpos < g_visarea.min_x) continue;
        if (zrev ? (zval >= destzptr[xdrawpos]) : (zval < destzptr[xdrawpos])) {
            if (pix != 0) { destzptr[xdrawpos] = (u16)zval; destptr[xdrawpos] = (u16)(color + pix); }
        }
    }
}

void draw_sprite_line(s32 curyy, s16 ypos, s16 xpos, int chainx, s32 dx, int ytileblock, int chaini, int currentsprite,
                      int chainy, int xflip, int yflip, u16 zval, bool zsort, bool blend, u16 group, bool checkerboard, u8 mosaic) {
    u32 srcpix_x = 0;
    u16 srcpix = 0;
    u8 mosaic_count_x = 0;
    for (int xdrw = 0; xdrw <= chainx; xdrw++) {
        u32 dstwidth = 0;
        do { srcpix_x += dx; dstwidth++; } while (srcpix_x < 0x100000);
        srcpix_x &= 0x0fffff;
        u32 tileno; u16 pal; u8 gfxregion;
        get_tile_details(chaini, currentsprite, xdrw, ytileblock, chainx, chainy, xflip, yflip, tileno, pal, gfxregion);
        zoom_transpen(gfxregion, tileno, pal, xflip, yflip, xpos, ypos, dx, dstwidth, zval, zsort, blend, group,
                      checkerboard, mosaic, mosaic_count_x, curyy, srcpix);
        xpos += dstwidth;
    }
}

inline int sext(int v, int bits) { return (int)((unsigned)v << (32 - bits)) >> (32 - bits); }

// Draw lines y0..y1 in parallel (LinePool, below).
void run_lines(int y0, int y1, const std::function<void(int)>& fn);

struct SpriteJob {
    int currentsprite;
    int ystart, xpos, height;
    s32 dx, dy;
    int chainx, chainy, chaini, xflip, yflip;
    u16 zval;
    bool blend;
    u16 group;
    bool checkerboard;
    u8 mosaic;
};
std::vector<SpriteJob> g_sprite_jobs;
bool g_sprite_zsort = false;

// One screen line of every queued sprite, in sprite-list order (MAME's per-sprite row loop).
void draw_sprites_line(int y) {
    for (const SpriteJob& j : g_sprite_jobs) {
        const int curyy = y - j.ystart;
        if (curyy < 0 || curyy >= j.height) continue;
        // MAME's mosaic row counter: realline repeats each (mosaic + 1) rows.
        const int realline = j.mosaic ? curyy - curyy % (j.mosaic + 1) : curyy;
        const u32 full_srcpix_y2 = realline * j.dy;
        const int used_ysource_pos = full_srcpix_y2 >> 16;
        draw_sprite_line(used_ysource_pos & 0xf, (s16)y, (s16)j.xpos, j.chainx, j.dx, used_ysource_pos / 0x10, j.chaini,
                         j.currentsprite, j.chainy, j.xflip, j.yflip, j.zval, g_sprite_zsort, j.blend, j.group,
                         j.checkerboard, j.mosaic);
    }
}

void draw_sprites_buffer() {
    memset(m_sprite_bitmap, 0, sizeof(m_sprite_bitmap));
    const int spriteoffsx = (m_spriteregs[1] >> 0) & 0xffff;
    const int spriteoffsy = (m_spriteregs[1] >> 16) & 0xffff;
    const bool zsort = !((m_spriteregs[0] >> 24) & 1);
    for (auto& z : m_sprite_zbuffer) z = zsort ? 0x0000 : 0x07ff;
    g_sprite_jobs.clear();
    g_sprite_zsort = zsort;

    int nextsprite = 0, currentsprite = 0;
    while (currentsprite < (0xc000 / 4) / 8) {
        const u32* s = &m_spriteram[currentsprite * 8];
        const u16 zval = (s[2] & 0x07ff0000) >> 16;
        s16 ypos = (s[0] & 0xffff0000) >> 16;
        s16 xpos = (s[0] & 0x0000ffff) >> 0;
        xpos += spriteoffsx;
        ypos += spriteoffsy;
        xpos = sext(xpos, 10);
        ypos = sext(ypos, 10);

        const bool blend = s[4] & 0x00800000;
        const u16 group = (s[4] & 0x00700000) >> 8;
        const bool checkerboard = s[4] & 0x04000000;
        const u8 mosaic = (s[4] & 0xf0000000) >> 28;
        const int yflip = (s[4] & 0x01000000) >> 24;
        const int xflip = (s[4] & 0x02000000) >> 25;
        const int chainy = s[2] & 0x0000000f;
        const int chainx = (s[2] & 0x000000f0) >> 4;
        const int chaini = s[2] & 0x00000100;
        nextsprite = !chaini ? currentsprite + 1 : currentsprite + ((chainx + 1) * (chainy + 1));

        const u32 zoomy = (s[1] & 0xffff0000) >> 16;
        const u32 zoomx = (s[1] & 0x0000ffff) >> 0;
        const u8 zoom_shift = ((m_spriteregs[0] >> 27) & 1) ? 4 : 8;
        if (!zoomx || !zoomy || (zsort && zval == 0)) { currentsprite = nextsprite; continue; }

        const s32 dx = zoomx << zoom_shift;
        const s32 dy = zoomy << zoom_shift;
        u32 full_srcpix_y = 0, full_dstheight = 0;
        do { full_srcpix_y += dy; full_dstheight++; } while (full_srcpix_y < (u32)((chainy + 1) * 0x100000) && full_dstheight < 4096);

        // MAME draws each sprite's rows here, top to bottom. We queue the sprite and draw all
        // sprites line by line below (in parallel); per line they're still drawn in this order,
        // so the z-buffer results are identical.
        g_sprite_jobs.push_back({currentsprite, ypos, xpos, (int)full_dstheight, dx, dy, chainx, chainy, chaini,
                                 xflip, yflip, zval, blend, group, checkerboard, mosaic});
        currentsprite = nextsprite;
    }
    run_lines(g_visarea.min_y, g_visarea.max_y, draw_sprites_line);
}

void mixsprites_test(u16 priority, int y) {
    const u8 spriteblendtype = (m_tcram[0x4c / 4] >> 16) & 1;
    const u16* src = &m_sprite_bitmap[y * VIDEO_W];
    u32* dst = &g_draw[y * VIDEO_W];
    for (int x = g_visarea.min_x; x <= g_visarea.max_x; x++) {
        const u16 srcpix = src[x];
        if ((srcpix & 0x0fff) && (srcpix & 0x7000) == priority) {
            if (srcpix & 0x8000) plot(&dst[x], pal_main[srcpix & 0x0fff], spriteblendtype ? 3 : 2);
            else plot(&dst[x], pal_main[srcpix & 0x0fff], 1);
        }
    }
}

// ---- 3D layer composite (MAME screen_update middle part) ----------------------------------
u32 m_fbscroll0() { return hw_phys_words(0x30000008)[0]; }

void draw_3d_layer() {
    const u16 palbase = ((m_tcram[0x4c / 4] >> 16) & 1) ? 0x800 : 0x000;
    const int ysize = g_visarea.max_y - g_visarea.min_y;
    if (!ysize || (fbcontrol(0) & 0x01)) return;
    const int yinc = (512 << 16) / ysize;
    int xscroll = (m_fbscroll0() >> 21);
    if (xscroll & 0x400) xscroll -= 0x800;
    xscroll += 256;
    for (int y = g_visarea.min_y; y <= g_visarea.max_y; y++) {
        const int realy = ((y - g_visarea.min_y) * yinc) >> 16;
        const u16* src = &g_color_buffer_3d[(realy & 0x1ff) * 512];
        u32* dst = &g_bitmap[y * VIDEO_W + g_visarea.min_x];
        for (int x = g_visarea.min_x; x <= g_visarea.max_x; x++, dst++) {
            const u16 srcpix = src[((g_visarea.min_x + x) + xscroll) & 0x1ff];
            if (srcpix & 0x07ff) {
                if (srcpix & 0x0800) *dst = alpha_blend_r32(*dst, pal_3d[(srcpix & 0xf7ff) | palbase], 0x80);
                else *dst = pal_3d[(srcpix & 0xf7ff) | palbase];
            }
        }
    }
}

// ---- whole frame (MAME screen_update) ----------------------------------------------------------
void update_visarea() {
    const u16 min_x = (m_tcram[1] & 0xffff0000) >> 16, min_y = m_tcram[1] & 0xffff;
    const u16 max_x = (m_tcram[2] & 0xffff0000) >> 16, max_y = m_tcram[2] & 0xffff;
    if (max_x == 0 || max_y == 0) { m_screen_dis = true; return; }
    m_screen_dis = false;
    g_visarea = {min_x, std::min<int>(min_x + max_x - 1, VIDEO_W - 1), min_y, std::min<int>(min_y + max_y - 1, VIDEO_H - 1)};
}

// ---- drawing lines in parallel -----------------------------------------------------------------
// Each screen line only reads shared state and writes its own line, so lines can be drawn on
// several CPU cores at once with exactly the same result.
class LinePool {
public:
    void run(int y0, int y1, const std::function<void(int)>& fn) {
        if (y1 < y0) return;
        start_workers();
        if (m_workers.empty()) { for (int y = y0; y <= y1; y++) fn(y); return; }
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_fn = &fn; m_next = y0; m_last = y1; m_busy = (int)m_workers.size(); m_gen++;
        }
        m_cv.notify_all();
        work();
        std::unique_lock<std::mutex> lk(m_mu);
        m_done_cv.wait(lk, [&] { return m_busy == 0; });
        m_fn = nullptr;
    }
    ~LinePool() {
        { std::lock_guard<std::mutex> lk(m_mu); m_quit = true; m_gen++; }
        m_cv.notify_all();
        for (auto& t : m_workers) t.join();
    }
private:
    void start_workers() {
        if (m_started) return;
        m_started = true;
        unsigned n = std::thread::hardware_concurrency();
        n = n > 1 ? std::min(n - 1, 7u) : 0;    // the calling thread works too
        for (unsigned i = 0; i < n; i++) m_workers.emplace_back([this] { loop(); });
    }
    void work() {
        for (;;) {
            const int y = m_next.fetch_add(1);
            if (y > m_last) break;
            (*m_fn)(y);
        }
    }
    void loop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_cv.wait(lk, [&] { return m_gen != seen; });
                seen = m_gen;
                if (m_quit) return;
            }
            work();
            std::lock_guard<std::mutex> lk(m_mu);
            if (--m_busy == 0) m_done_cv.notify_one();
        }
    }
    std::vector<std::thread> m_workers;
    std::mutex m_mu;
    std::condition_variable m_cv, m_done_cv;
    const std::function<void(int)>* m_fn = nullptr;
    std::atomic<int> m_next{0};
    int m_last = 0, m_busy = 0;
    uint64_t m_gen = 0;
    bool m_quit = false, m_started = false;
};
LinePool g_lines;
void run_lines(int y0, int y1, const std::function<void(int)>& fn) { g_lines.run(y0, y1, fn); }

void draw_back_line(int y) {
    for (int i = 0x1f; i >= 0x10; i--) {
        for (int j = 0; j < 4; j++) if ((get_tileregs(j) & 0x1f) == i) draw_tilemap(j, y);
        if ((i & 3) == 0) mixsprites_test((u16)((i / 4) << 12), y);
    }
}

void draw_front_line(int y) {
    for (int i = 0x0f; i >= 0x00; i--) {
        for (int j = 0; j < 4; j++) if ((get_tileregs(j) & 0x1f) == i) draw_tilemap(j, y);
        if ((i & 3) == 0) mixsprites_test((u16)((i / 4) << 12), y);
    }
}

void draw_front_layers() { g_lines.run(g_visarea.min_y, g_visarea.max_y, draw_front_line); }

void screen_update(bool split) {
    g_layers_ok = false;
    update_visarea();
    update_all_palettes();
    const u32 fill = (fbcontrol(0) & 0x01) ? pal_main[0] : BLACK;
    for (auto& p : g_bitmap) p = fill;
    if (m_screen_dis) return;

    draw_sprites_buffer();
    if ((m_tcram[0x24 / 4] >> 17) & 1) return;

    g_lines.run(g_visarea.min_y, g_visarea.max_y, draw_back_line);
    if (split) memcpy(g_below, g_bitmap, sizeof(g_below));
    draw_3d_layer();
    draw_front_layers();

    if (split) {
        // Same front layers again, into the separate layer for the high-resolution compositor.
        memset(g_over, 0, sizeof(g_over));
        g_draw = g_over; g_over_mode = true;
        draw_front_layers();
        g_draw = g_bitmap; g_over_mode = false;
        g_layers_ok = true;
    }
}

// ---- output: frame dumps (the window, Direct3D and the menu are in frontend/) ----------------------------------------------------------
void save_bmp(const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "[video] can't write %s\n", path.c_str()); return; }
    const u32 img = VIDEO_W * VIDEO_H * 4;
    u8 h[54] = {'B', 'M'};
    auto put32 = [&](int at, u32 v) { memcpy(h + at, &v, 4); };
    put32(2, 54 + img); put32(10, 54); put32(14, 40); put32(18, VIDEO_W); put32(22, (u32)-VIDEO_H);
    h[26] = 1; h[28] = 32; put32(34, img);
    fwrite(h, 1, 54, f);
    fwrite(g_bitmap, 1, img, f);
    fclose(f);
}


} // namespace

uint16_t g_color_buffer_3d[512 * 512];
const uint32_t* video_framebuffer() { return g_bitmap; }

extern "C" void video_init(const char* rom_dir) {
    std::string d = std::string(rom_dir) + "/buriki/";
    g_scrtile.assign(0x4000000, 0);
    load_interleaved(g_scrtile, d + "007sc01a.97", 0x0000000, 2);
    load_interleaved(g_scrtile, d + "007sc02a.99", 0x0000001, 2);
    load_interleaved(g_scrtile, d + "007sc05a.98", 0x1000000, 2);
    load_interleaved(g_scrtile, d + "007sc06a.100", 0x1000001, 2);
    load_interleaved(g_scrtile, d + "007sc03a.101", 0x2000000, 2);
    load_interleaved(g_scrtile, d + "007sc04a.103", 0x2000001, 2);
    load_interleaved(g_scrtile, d + "007sc07a.102", 0x3000000, 2);
    load_interleaved(g_scrtile, d + "007sc08a.104", 0x3000001, 2);
    hng64_reorder(g_scrtile);

    g_sprtile.assign(0x4000000, 0);
    const char* sp[8] = {"007sp01a.105", "007sp02a.109", "007sp03a.113", "007sp04a.117",
                         "007sp05a.106", "007sp06a.110", "007sp07a.114", "007sp08a.118"};
    for (int i = 0; i < 8; i++) load_interleaved(g_sprtile, d + sp[i], (i < 4 ? 0 : 0x2000000) + (i & 3), 4);

    auto mk = [](const std::vector<u8>& r, const Layout& l, u32 gran, u32 colors) {
        return Gfx{r.data(), &l, (u32)((uint64_t)r.size() * 8 / l.charinc), gran, colors};
    };
    g_gfx[0] = mk(g_scrtile, hng64_8x8x4_tilelayout, 16, 0x100);
    g_gfx[1] = mk(g_scrtile, hng64_8x8x8_tilelayout, 256, 0x10);
    g_gfx[2] = mk(g_scrtile, hng64_16x16x4_tilelayout, 16, 0x100);
    g_gfx[3] = mk(g_scrtile, hng64_16x16x8_tilelayout, 256, 0x10);
    g_gfx[4] = mk(g_sprtile, hng64_16x16x4_spritelayout, 16, 0x100);
    g_gfx[5] = mk(g_sprtile, hng64_16x16x8_spritelayout, 256, 0x10);

    m_videoram = hw_phys_words(0x20100000);
    m_videoregs = hw_phys_words(0x20190000);
    m_tcram = hw_phys_words(0x20208000);
    m_paletteram = hw_phys_words(0x20200000);
    m_spriteram = hw_phys_words(0x20000000);
    m_spriteregs = hw_phys_words(0x20010000);

    video3d_init(rom_dir);

    if (!g_video_cfg.headless) { fe_window_open(); hd3d_install(); }
    fprintf(stderr, "[video] graphics ROMs loaded, %s\n", g_video_cfg.headless ? "headless" : "window open");
}

bool video_hd_layers(HdLayers* out) {
    if (!g_layers_ok) return false;
    out->below = g_below;
    out->over = g_over;
    out->pal_3d = pal_3d;
    out->palbase = ((m_tcram[0x4c / 4] >> 16) & 1) ? 0x800 : 0x000;
    int xscroll = (m_fbscroll0() >> 21);
    if (xscroll & 0x400) xscroll -= 0x800;
    out->xscroll = xscroll + 256;
    out->enable3d = !(fbcontrol(0) & 0x01) && (g_visarea.max_y - g_visarea.min_y) > 0;
    return true;
}

extern "C" void video_frame(void) {
    // The high-resolution renderer needs the picture split around the 3D layer, plus this
    // frame's 3D triangles (taken before the vblank clear below). Rendering only: game state
    // and the 512x448 picture are unchanged either way.
    const bool hd = !g_video_cfg.headless && fe_setting_enhanced();
    {
        // Diagnostics: average time per frame for the 2D/3D picture, and how many 3D triangles
        // were drawn since the last 3D clear, every 600 frames.
        static double t_total = 0; static uint64_t tris_total = 0;
        auto t0 = std::chrono::steady_clock::now();
        screen_update(hd);
        t_total += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        size_t n = 0; video3d_live_tri_count(&n); tris_total += n;
        if (hw_frame_number() % 600 == 0) {
            fprintf(stderr, "[perf] picture %.2f ms/frame, 3D triangles %llu/frame\n", t_total * 1000 / 600, (unsigned long long)(tris_total / 600));
            t_total = 0; tris_total = 0;
        }
    }
    if (hd) video3d_hd_snapshot();
    // MAME screen_vblank: at the start of vblank, clear the 3D buffers if tcram 0x50 bit 16 is set.
    if ((m_tcram[0x50 / 4] >> 16) & 1) video3d_clear();

    const uint32_t frame = hw_frame_number();
    bool dump = g_video_cfg.dump_dir && ((g_video_cfg.dump_every && frame % g_video_cfg.dump_every == 0) ||
        std::find(g_video_cfg.dump_frames.begin(), g_video_cfg.dump_frames.end(), frame) != g_video_cfg.dump_frames.end());
    if (dump) {
        char name[64];
        snprintf(name, sizeof(name), "/frame_%05u.bmp", frame);
        save_bmp(std::string(g_video_cfg.dump_dir) + name);
    }

    // Show it (paced to ~59.43 Hz). While the pause menu is open this doesn't return, so the game stops.
    if (!g_video_cfg.headless) fe_present_frame(g_bitmap, VIDEO_W, VIDEO_H);
}
