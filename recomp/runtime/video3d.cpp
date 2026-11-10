// HNG64 3D: display-list commands, geometry from the vertex ROMs, transform/light/clip, and a
// perspective-correct textured rasteriser into a 512x512 colour + depth buffer.
//
// Port of MAME src/mame/snk/hng64_3d.ipp (LGPL-2.1+, David Haywood, Angelo Salese, ElSemi,
// Andrew Gardner, Andrew Zaferakis) and the triangle/clipper parts of src/devices/video/poly.h
// (BSD-3-Clause, Ville Linde, Aaron Giles). Names follow MAME for side-by-side comparison.
// MAME renders each display list as soon as it's uploaded (dl_upload_w); so do we.
#include "hw.h"
#include "video.h"
#include "hd3d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern uint16_t g_color_buffer_3d[512 * 512];

namespace {
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int16_t s16; typedef int32_t s32;

std::vector<u8> m_texturerom;   // 16 MB: 16 pages of 1024x1024 8-bit texels
std::vector<u16> m_vertsrom;    // vt01/vt02/vt03 interleaved 16-bit words
float g_depth_buffer_3d[512 * 512];

struct polyVert {
    float worldCoords[4]{};
    float texCoords[2]{};
    float normal[4]{};
    float clipCoords[4]{};
    float light = 0;
};

struct polygon {
    int n = 0;
    polyVert vert[10]{};
    float faceNormal[4]{};
    bool visible = false, flatShade = false, blend = false;
    u8 texIndex = 0, tex4bpp = 0;
    u16 texPageSmall = 0;
    u32 palOffset = 0;
    u16 colorIndex = 0;
    u16 texscrollx = 0, texscrolly = 0;
    u16 tex_mask_x = 1024, tex_mask_y = 1024;
};

const int HNG64_MAX_POLYGONS = 10000;
std::vector<polygon> m_polys(HNG64_MAX_POLYGONS);

// 3D state
u16 m_texturescrollx = 0, m_texturescrolly = 0, m_paletteState3d = 0;
u16 m_modelscalex = 0, m_modelscaley = 0, m_modelscalez = 0;
float m_projectionMatrix[16]{}, m_modelViewMatrix[16]{}, m_cameraMatrix[16]{};
float m_lightStrength = 0, m_lightVector[3]{};
u32 m_dl_upload = 0;

// 3D viewport ("scissor") in 512x512 buffer coordinates, inclusive. Full screen by default;
// set from the projection packet (see setCameraProjectionMatrix).
int g_view_left = 0, g_view_right = 512, g_view_top = 0, g_view_bottom = 512;
// Image offset in buffer pixels, from projection packet words 1-2 (0 except on "how to play").
float g_view_offx = 0.0f, g_view_offy = 0.0f;

// Triangles recorded for the high-resolution renderer (hd3d.h), cleared with the 3D buffer.
std::vector<HdTri> g_hd_tris;

u8 texture_wrapsize(int i) {   // MAME m_texture_wrapsize_table, bytes at 0x30000010
    u32 w = hw_phys_words(0x30000010 + (i & ~3))[0];
    return (u8)(w >> (24 - 8 * (i & 3)));
}

// ---- math (MAME 3D utility code) --------------------------------------------------------------
void matmul4(float* product, const float* a, const float* b) {
    float tmp[16];
    for (int i = 0; i < 4; i++) {
        const float ai0 = a[0 + i], ai1 = a[4 + i], ai2 = a[8 + i], ai3 = a[12 + i];
        tmp[0 + i] = ai0 * b[0] + ai1 * b[1] + ai2 * b[2] + ai3 * b[3];
        tmp[4 + i] = ai0 * b[4] + ai1 * b[5] + ai2 * b[6] + ai3 * b[7];
        tmp[8 + i] = ai0 * b[8] + ai1 * b[9] + ai2 * b[10] + ai3 * b[11];
        tmp[12 + i] = ai0 * b[12] + ai1 * b[13] + ai2 * b[14] + ai3 * b[15];
    }
    memcpy(product, tmp, sizeof(tmp));   // (MAME writes in place; a == product there is safe per column)
}
void vecmatmul4(float* product, const float* a, const float* b) {
    const float bi0 = b[0], bi1 = b[1], bi2 = b[2], bi3 = b[3];
    product[0] = bi0 * a[0] + bi1 * a[4] + bi2 * a[8] + bi3 * a[12];
    product[1] = bi0 * a[1] + bi1 * a[5] + bi2 * a[9] + bi3 * a[13];
    product[2] = bi0 * a[2] + bi1 * a[6] + bi2 * a[10] + bi3 * a[14];
    product[3] = bi0 * a[3] + bi1 * a[7] + bi2 * a[11] + bi3 * a[15];
}
float vecDotProduct(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void setIdentity(float* m) { for (int i = 0; i < 16; i++) m[i] = 0.0f; m[0] = m[5] = m[10] = m[15] = 1.0f; }
float uToF(u16 input) { return float(s16(input)) / 32768.0f; }
void normalize(float* x) {
    const double l = sqrt((double)x[0] * x[0] + (double)x[1] * x[1] + (double)x[2] * x[2]);
    x[0] = float(x[0] / l); x[1] = float(x[1] / l); x[2] = float(x[2] / l);
}

// ---- clipping (MAME poly.h frustum_clip_*) ------------------------------------------------------
struct ClipVert { float x, y, z, w; float p[5]; };

int frustum_clip_w(const ClipVert* v, int n, ClipVert* out) {
    if (n <= 0) return 0;
    const float W_PLANE = 0.000001f;
    ClipVert clipv[10]; int cv = 0; int previ = n - 1;
    for (int i = 0; i < n; i++) {
        int s1 = (v[i].w < W_PLANE) ? -1 : 1, s2 = (v[previ].w < W_PLANE) ? -1 : 1;
        if (s1 * s2 < 0) {
            float wdiv = v[previ].w - v[i].w;
            if (wdiv == 0.0f) return 0;
            float t = fabsf((W_PLANE - v[previ].w) / wdiv);
            ClipVert& c = clipv[cv++];
            c.x = v[previ].x + (v[i].x - v[previ].x) * t; c.y = v[previ].y + (v[i].y - v[previ].y) * t;
            c.z = v[previ].z + (v[i].z - v[previ].z) * t; c.w = v[previ].w + (v[i].w - v[previ].w) * t;
            for (int p = 0; p < 5; p++) c.p[p] = v[previ].p[p] + (v[i].p[p] - v[previ].p[p]) * t;
            if (cv >= 10) break;
        }
        if (s1 > 0 && cv < 10) clipv[cv++] = v[i];
        previ = i;
    }
    memcpy(out, clipv, sizeof(ClipVert) * cv);
    return cv;
}

int frustum_clip(const ClipVert* v, int n, ClipVert* out, int axis, int sign) {
    if (n <= 0) return 0;
    ClipVert clipv[10]; int cv = 0; int previ = n - 1;
    for (int i = 0; i < n; i++) {
        const float* v1a = &v[i].x; const float* v2a = &v[previ].x;
        float a1 = sign ? v1a[axis] : -v1a[axis], a2 = sign ? v2a[axis] : -v2a[axis];
        int s1 = (a1 <= v[i].w) ? 1 : -1, s2 = (a2 <= v[previ].w) ? 1 : -1;
        if (s1 * s2 < 0) {
            float wdiv = (v[previ].w - a2) - (v[i].w - a1);
            if (wdiv == 0.0f) return 0;
            float t = fabsf((v[previ].w - a2) / wdiv);
            ClipVert& c = clipv[cv++];
            c.x = v[previ].x + (v[i].x - v[previ].x) * t; c.y = v[previ].y + (v[i].y - v[previ].y) * t;
            c.z = v[previ].z + (v[i].z - v[previ].z) * t; c.w = v[previ].w + (v[i].w - v[previ].w) * t;
            for (int p = 0; p < 5; p++) c.p[p] = v[previ].p[p] + (v[i].p[p] - v[previ].p[p]) * t;
            if (cv >= 10) break;
        }
        if (s1 > 0 && cv < 10) clipv[cv++] = v[i];
        previ = i;
    }
    memcpy(out, clipv, sizeof(ClipVert) * cv);
    return cv;
}

int frustum_clip_all(ClipVert* v, int n) {
    n = frustum_clip_w(v, n, v);
    for (int axis = 0; axis < 3; axis++)
        for (int sign = 0; sign < 2; sign++) n = frustum_clip(v, n, v, axis, sign);
    return n;
}

// ---- rasteriser (MAME poly.h render_triangle, single-threaded) ------------------------------------
struct RVert { float x, y; float p[7]; };
struct RenderData {
    u8 tex4bpp, texIndex; u16 texPageSmall; u32 palOffset; u16 colorIndex; bool blend;
    u16 texscrollx, texscrolly, tex_mask_x, tex_mask_y;
};

inline s32 round_coordinate(float value) {
    if (value >= 2147483647.0f) return 2147483647;
    const float ipart = floorf(value);
    if (ipart < -2147483648.0f) return (s32)0x80000000;
    return s32(ipart) + ((value - ipart > 0.5f) ? 1 : 0);
}

void render_texture_scanline(s32 scanline, s32 startx, s32 stopx, const float* start, const float* dpdx, const RenderData& rd) {
    if (scanline > 511 || scanline < 0) return;
    float z = start[0], w = start[1], light = start[2], s = start[5], t = start[6];
    const float dz = dpdx[0], dw = dpdx[1], dlight = dpdx[2], ds = dpdx[5], dt = dpdx[6];
    u16* colorBuffer = &g_color_buffer_3d[scanline * 512];
    float* depthBuffer = &g_depth_buffer_3d[scanline * 512];
    const u8* textureOffset = &m_texturerom[(size_t)rd.texIndex * 1024 * 1024];
    const int textPageSub = (rd.texPageSmall & 0xc000) >> 14;
    const int texPageHorizOffset = (rd.texPageSmall & 0x3f80) >> 7;
    const int texPageVertOffset = rd.texPageSmall & 0x007f;

    for (int x = startx; x < stopx; x++) {
        if (z < depthBuffer[x & 511]) {
            const float sCorrect = s / w, tCorrect = t / w, rCorrect = light / w;
            float textureS = sCorrect * 1024.0f, textureT = tCorrect * 1024.0f;
            textureS += (rd.texscrolly & 0x3fff) >> 5;
            textureT += (rd.texscrollx & 0x3fff) >> 5;
            if (textPageSub & 2) {
                textureT = fmodf(textureT, (float)rd.tex_mask_x); textureT += 8.0f * texPageHorizOffset;
                textureS = fmodf(textureS, (float)rd.tex_mask_y); textureS += 8.0f * texPageVertOffset;
            }
            int ti = (int)textureT & 1023, si = (int)textureS & 1023;
            u8 paletteEntry;
            if (rd.tex4bpp) {
                paletteEntry = textureOffset[si * 512 + (ti >> 1)];
                paletteEntry = (ti & 1) ? ((paletteEntry >> 4) & 0x0f) : (paletteEntry & 0x0f);
            } else paletteEntry = textureOffset[si * 1024 + ti];
            if (paletteEntry != 0) {
                const u8 lightval = (u8)(rCorrect / 16.0f);
                u16 color = ((rd.palOffset + paletteEntry) & 0x7ff) | (lightval << 12);
                if (rd.blend) color |= 0x800;
                colorBuffer[x & 511] = color;
                depthBuffer[x & 511] = z;
            }
        }
        z += dz; w += dw; light += dlight; s += ds; t += dt;
    }
}

void render_flat_scanline(s32 scanline, s32 startx, s32 stopx, const float* start, const float* dpdx, const RenderData& rd) {
    if (scanline > 511 || scanline < 0) return;
    float z = start[0];
    const float dz = dpdx[0];
    u16* colorBuffer = &g_color_buffer_3d[scanline * 512];
    float* depthBuffer = &g_depth_buffer_3d[scanline * 512];
    for (int x = startx; x < stopx; x++) {
        if (z < depthBuffer[x & 511]) {
            colorBuffer[x & 511] = (u16)(rd.palOffset + rd.colorIndex);
            depthBuffer[x & 511] = z;
        }
        z += dz;
    }
}

template <int ParamCount, bool Textured>
void render_triangle(const RVert& _v1, const RVert& _v2, const RVert& _v3, const RenderData& rd) {
    // MAME uses visibleArea.set(0, 512, 0, 512) (inclusive); we use the projection window,
    // which is that same rectangle except on the "how to play" screen (see setCameraProjectionMatrix).
    const int clip_left = g_view_left, clip_right = g_view_right, clip_top = g_view_top, clip_bottom = g_view_bottom;
    const RVert *v1 = &_v1, *v2 = &_v2, *v3 = &_v3;
    if (v2->y < v1->y) std::swap(v1, v2);
    if (v3->y < v2->y) { std::swap(v2, v3); if (v2->y < v1->y) std::swap(v1, v2); }

    s32 v1yclip = std::max(round_coordinate(v1->y), clip_top);
    s32 v3yclip = std::min(round_coordinate(v3->y), clip_bottom + 1);
    if (v3yclip - v1yclip <= 0) return;

    const float dxdy_v1v2 = (v2->y == v1->y) ? 0.0f : (v2->x - v1->x) / (v2->y - v1->y);
    const float dxdy_v1v3 = (v3->y == v1->y) ? 0.0f : (v3->x - v1->x) / (v3->y - v1->y);
    const float dxdy_v2v3 = (v3->y == v2->y) ? 0.0f : (v3->x - v2->x) / (v3->y - v2->y);

    float param_start[7] = {}, param_dpdx[7] = {}, param_dpdy[7] = {};
    {
        float a00 = v2->y - v3->y, a01 = v3->x - v2->x, a02 = v2->x * v3->y - v3->x * v2->y;
        float a10 = v3->y - v1->y, a11 = v1->x - v3->x, a12 = v3->x * v1->y - v1->x * v3->y;
        float a20 = v1->y - v2->y, a21 = v2->x - v1->x, a22 = v1->x * v2->y - v2->x * v1->y;
        float det = a02 + a12 + a22;
        if (fabsf(det) < 0.00001f) {
            for (int p = 0; p < ParamCount; p++) param_start[p] = v1->p[p];
        } else {
            float idet = 1.0f / det;
            for (int p = 0; p < ParamCount; p++) {
                param_dpdx[p] = idet * (v1->p[p] * a00 + v2->p[p] * a10 + v3->p[p] * a20);
                param_dpdy[p] = idet * (v1->p[p] * a01 + v2->p[p] * a11 + v3->p[p] * a21);
                param_start[p] = idet * (v1->p[p] * a02 + v2->p[p] * a12 + v3->p[p] * a22);
            }
        }
    }

    for (s32 curscan = v1yclip; curscan < v3yclip; curscan++) {
        const float fully = float(curscan) + 0.5f;
        const float startx = v1->x + (fully - v1->y) * dxdy_v1v3;
        const float stopx = (fully < v2->y) ? v1->x + (fully - v1->y) * dxdy_v1v2 : v2->x + (fully - v2->y) * dxdy_v2v3;
        s32 istartx = round_coordinate(startx), istopx = round_coordinate(stopx);
        if (istartx > istopx) std::swap(istartx, istopx);
        istartx = std::max(istartx, clip_left);
        istopx = std::min(istopx, clip_right + 1);
        if (istartx >= istopx) continue;
        const float fullstartx = float(istartx) + 0.5f;
        float start[7], dpdx[7];
        for (int p = 0; p < ParamCount; p++) {
            start[p] = param_start[p] + fullstartx * param_dpdx[p] + fully * param_dpdy[p];
            dpdx[p] = param_dpdx[p];
        }
        if (Textured) render_texture_scanline(curscan, istartx, istopx, start, dpdx, rd);
        else render_flat_scanline(curscan, istartx, istopx, start, dpdx, rd);
    }
}

// Record a polygon (as a fan) for the high-resolution renderer, before the software rasteriser
// modifies its vertices. Same vertices, same order, same clip rectangle.
void record_hd(const polygon* p) {
    if (p->n < 3 || g_hd_tris.size() > 200000) return;
    for (int j = 1; j < p->n - 1; j++) {
        HdTri t{};
        const polyVert* src[3] = {&p->vert[0], &p->vert[j], &p->vert[j + 1]};
        for (int k = 0; k < 3; k++) {
            t.v[k] = {src[k]->clipCoords[0], src[k]->clipCoords[1], src[k]->clipCoords[2], src[k]->clipCoords[3],
                      src[k]->texCoords[0], src[k]->texCoords[1], src[k]->light};
        }
        t.flat = p->flatShade;
        t.tex4bpp = p->tex4bpp; t.texIndex = p->texIndex; t.blend = p->blend;
        t.texPageSmall = p->texPageSmall; t.colorIndex = p->colorIndex; t.palOffset = p->palOffset;
        t.texscrollx = p->texscrollx; t.texscrolly = p->texscrolly;
        t.tex_mask_x = p->tex_mask_x; t.tex_mask_y = p->tex_mask_y;
        t.clip_l = (int16_t)g_view_left; t.clip_t = (int16_t)g_view_top;
        t.clip_r = (int16_t)g_view_right; t.clip_b = (int16_t)g_view_bottom;
        g_hd_tris.push_back(t);
    }
}

void drawShaded(polygon* p) {
    record_hd(p);
    RenderData rd{p->tex4bpp, p->texIndex, p->texPageSmall, p->palOffset, p->colorIndex, p->blend,
                  p->texscrollx, p->texscrolly, p->tex_mask_x, p->tex_mask_y};
    if (p->flatShade) {
        for (int j = 1; j < p->n - 1; j++) {
            RVert v[3] = {};
            const polyVert* src[3] = {&p->vert[0], &p->vert[j], &p->vert[j + 1]};
            for (int k = 0; k < 3; k++) { v[k].x = src[k]->clipCoords[0]; v[k].y = src[k]->clipCoords[1]; v[k].p[0] = src[k]->clipCoords[2]; }
            render_triangle<4, false>(v[0], v[1], v[2], rd);
        }
    } else {
        for (int j = 0; j < p->n; j++) {
            p->vert[j].clipCoords[3] = 1.0f / p->vert[j].clipCoords[3];
            p->vert[j].light *= p->vert[j].clipCoords[3];
            p->vert[j].texCoords[0] *= p->vert[j].clipCoords[3];
            p->vert[j].texCoords[1] *= p->vert[j].clipCoords[3];
        }
        for (int j = 1; j < p->n - 1; j++) {
            RVert v[3] = {};
            const polyVert* src[3] = {&p->vert[0], &p->vert[j], &p->vert[j + 1]};
            for (int k = 0; k < 3; k++) {
                v[k].x = src[k]->clipCoords[0]; v[k].y = src[k]->clipCoords[1];
                v[k].p[0] = src[k]->clipCoords[2]; v[k].p[1] = src[k]->clipCoords[3]; v[k].p[2] = src[k]->light;
                v[k].p[5] = src[k]->texCoords[0]; v[k].p[6] = src[k]->texCoords[1];
            }
            render_triangle<7, true>(v[0], v[1], v[2], rd);
        }
    }
}

// ---- display list commands -------------------------------------------------------------------------
void setCameraTransformation(const u16* packet) {
    m_cameraMatrix[0] = uToF(packet[1]); m_cameraMatrix[4] = uToF(packet[2]); m_cameraMatrix[8] = uToF(packet[3]); m_cameraMatrix[3] = 0.0f;
    m_cameraMatrix[1] = uToF(packet[4]); m_cameraMatrix[5] = uToF(packet[5]); m_cameraMatrix[9] = uToF(packet[6]); m_cameraMatrix[7] = 0.0f;
    m_cameraMatrix[2] = uToF(packet[7]); m_cameraMatrix[6] = uToF(packet[8]); m_cameraMatrix[10] = uToF(packet[9]); m_cameraMatrix[11] = 0.0f;
    m_cameraMatrix[12] = uToF(packet[10]); m_cameraMatrix[13] = uToF(packet[11]); m_cameraMatrix[14] = uToF(packet[12]); m_cameraMatrix[15] = 1.0f;
}

void setLighting(const u16* packet) {
    m_lightVector[0] = uToF(packet[3]); m_lightVector[1] = uToF(packet[4]); m_lightVector[2] = uToF(packet[5]);
    m_lightStrength = uToF(packet[9]);
}

void set3dFlags(const u16* packet) {
    m_texturescrollx = packet[1]; m_texturescrolly = packet[2]; m_paletteState3d = packet[8];
    m_modelscalex = packet[5]; m_modelscaley = packet[6]; m_modelscalez = packet[7];
}

// Debug: with BURIKI_LOG3D set, print each distinct projection packet the game sends.
void log_projection(const u16* packet) {
    static bool enabled = getenv("BURIKI_LOG3D") != nullptr;
    static u16 last[16] = {};
    if (!enabled || !memcmp(last, packet, sizeof(last))) return;
    memcpy(last, packet, sizeof(last));
    fprintf(stderr, "[3d] frame %u projection:", hw_frame_number());
    for (int i = 0; i < 16; i++) fprintf(stderr, " %04X", packet[i]);
    fprintf(stderr, "\n");
}

void setCameraProjectionMatrix(const u16* packet) {
    log_projection(packet);
    // DEVIATION FROM MAME. Packet words 10-13 give the projection window in screen pixels
    // relative to the screen centre. Every scene uses the full screen (+-256 x +-224), except
    // Buriki's "how to play" screen: right 0x60, left -0xE0, top 0x90, bottom -0x60, i.e.
    // screen x 32-352, y 80-320, which is exactly the framed monitor on that screen. (Words 1-2,
    // -0x40 and +0x18, are that window's centre.) MAME stretches the window over the whole
    // screen, so the fighters come out ~1.7x too big and outside the frame. Instead we project
    // at the normal full-screen scale (1 unit = 1 pixel) and only draw inside the window, the
    // way a viewport works. Words 1-2 then move the image (x, and y upwards) so the scene is
    // centred in the window; without that the fighters sit 64 px right and 24 px low of the
    // frame compared with hardware footage. For full-screen windows (words 1-2 = 0) this is
    // identical to MAME.
    const int win_r = s16(packet[10]), win_l = s16(packet[11]), win_t = s16(packet[12]), win_b = s16(packet[13]);
    const bool full_screen = win_l == -256 && win_r == 256 && win_t == 224 && win_b == -224;
    const bool sane = win_r > win_l && win_t > win_b;
    if (!full_screen && sane) {
        const float ky = 512.0f / 448.0f;   // the 512-line buffer is shown on 448 screen lines
        g_view_left = std::max(0, 256 + win_l);
        g_view_right = std::min(512, 256 + win_r);
        g_view_top = std::max(0, (int)lrintf(256.0f - win_t * ky));
        g_view_bottom = std::min(512, (int)lrintf(256.0f - win_b * ky));
        g_view_offx = (float)s16(packet[1]);
        g_view_offy = -(float)s16(packet[2]) * ky;
    } else {
        g_view_left = 0; g_view_right = 512; g_view_top = 0; g_view_bottom = 512;
        g_view_offx = g_view_offy = 0.0f;
    }
    const float left = sane ? uToF(0xFF00) : uToF(packet[11]), right = sane ? uToF(0x0100) : uToF(packet[10]);
    const float top = sane ? uToF(0x00E0) : uToF(packet[12]), bottom = sane ? uToF(0xFF20) : uToF(packet[13]);
    const float screenZ = uToF(packet[6]) * uToF(packet[4]) + uToF(packet[6]);
    const float near_ = uToF(packet[5]) * uToF(packet[4]) + uToF(packet[5]);
    const float far_ = -(screenZ * near_) / (screenZ - 2.0f * near_);
    float* m = m_projectionMatrix;
    m[0] = (2.0f * screenZ) / (right - left); m[1] = 0.0f; m[2] = 0.0f; m[3] = 0.0f;
    m[4] = 0.0f; m[5] = (2.0f * screenZ) / (top - bottom); m[6] = 0.0f; m[7] = 0.0f;
    m[8] = (right + left) / (right - left); m[9] = (top + bottom) / (top - bottom);
    m[10] = -((far_ + near_) / (far_ - near_)); m[11] = -1.0f;
    m[12] = 0.0f; m[13] = 0.0f; m[14] = -((2.0f * far_ * near_) / (far_ - near_)); m[15] = 0.0f;
}

void recoverStandardVerts(polygon& currentPoly, int m, const u16* chunkOffset_verts, int& counter, const u16* packet) {
    currentPoly.vert[m].worldCoords[0] = uToF(chunkOffset_verts[counter++]);
    currentPoly.vert[m].worldCoords[1] = uToF(chunkOffset_verts[counter++]);
    currentPoly.vert[m].worldCoords[2] = uToF(chunkOffset_verts[counter++]);
    currentPoly.vert[m].worldCoords[3] = 1.0f;
    currentPoly.n = 3;
    counter++;  // "maybe_blend", unused
    currentPoly.vert[m].texCoords[0] = uToF(chunkOffset_verts[counter]);
    if (currentPoly.flatShade) currentPoly.colorIndex = chunkOffset_verts[counter] >> 5;
    counter++;
    currentPoly.vert[m].texCoords[1] = uToF(chunkOffset_verts[counter++]);
    if (packet[1] & 0x0040) {
        currentPoly.vert[m].worldCoords[0] = (currentPoly.vert[m].worldCoords[0] * m_modelscalez) / 0x100;
        currentPoly.vert[m].worldCoords[1] = (currentPoly.vert[m].worldCoords[1] * m_modelscaley) / 0x100;
        currentPoly.vert[m].worldCoords[2] = (currentPoly.vert[m].worldCoords[2] * m_modelscalex) / 0x100;
    }
}

uint64_t g_bad_geometry = 0;

void recoverPolygonBlock(const u16* packet, int& numPolys) {
    float objectMatrix[16];
    setIdentity(objectMatrix);
    objectMatrix[8] = uToF(packet[7]); objectMatrix[4] = uToF(packet[8]); objectMatrix[0] = uToF(packet[9]); objectMatrix[3] = 0.0f;
    objectMatrix[9] = uToF(packet[10]); objectMatrix[5] = uToF(packet[11]); objectMatrix[1] = uToF(packet[12]); objectMatrix[7] = 0.0f;
    objectMatrix[10] = uToF(packet[13]); objectMatrix[6] = uToF(packet[14]); objectMatrix[2] = uToF(packet[15]); objectMatrix[11] = 0.0f;
    objectMatrix[12] = uToF(packet[4]); objectMatrix[13] = uToF(packet[5]); objectMatrix[14] = uToF(packet[6]); objectMatrix[15] = 1.0f;

    const u32 threeDOffset = (u32(packet[2]) << 16) | u32(packet[3]);
    if ((uint64_t)threeDOffset * 3 + 21 > m_vertsrom.size()) { g_bad_geometry++; return; }
    const u16* threeDPointer = &m_vertsrom[(size_t)threeDOffset * 3];

    u32 address[4], size[4];
    address[0] = threeDPointer[0]; address[1] = threeDPointer[1];
    const u32 megaOffset = threeDPointer[2];
    address[2] = threeDPointer[3]; address[3] = threeDPointer[4];
    size[0] = threeDPointer[6]; size[1] = threeDPointer[7];
    size[2] = threeDPointer[9]; size[3] = threeDPointer[10];
    for (auto& a : address) a |= (megaOffset << 16);

    polygon lastPoly;
    for (int k = 0; k < 4; k++) {
        if ((uint64_t)address[k] * 3 >= m_vertsrom.size()) { g_bad_geometry++; continue; }
        const u16* chunkOffset = &m_vertsrom[(size_t)address[k] * 3];
        for (u32 l = 0; l < size[k]; l++) {
            if (chunkOffset + 40 > m_vertsrom.data() + m_vertsrom.size()) { g_bad_geometry++; break; }
            if (numPolys >= HNG64_MAX_POLYGONS) return;
            const u8 chunkType = chunkOffset[0] & 0x00ff;
            if (chunkOffset[0] & 0xff00) continue;   // MAME: "Weird!" (note: does not advance either)

            polygon& currentPoly = m_polys[numPolys];
            currentPoly.tex4bpp = (chunkOffset[1] & 0x1000) ? 1 : 0;
            currentPoly.texPageSmall = chunkOffset[2];
            currentPoly.texIndex = chunkOffset[1] & 0x000f;
            currentPoly.tex_mask_x = 1 << texture_wrapsize(currentPoly.texIndex * 2 + 0);
            currentPoly.tex_mask_y = 1 << texture_wrapsize(currentPoly.texIndex * 2 + 1);
            currentPoly.flatShade = !(chunkOffset[1] & 0x8000);
            currentPoly.blend = false;
            currentPoly.palOffset = 0;
            u16 explicitPaletteValue = (chunkOffset[1] & 0x0ff0) >> 4;
            explicitPaletteValue = explicitPaletteValue << 3;
            if (packet[1] & 0x0100) explicitPaletteValue |= ((m_paletteState3d >> 8) & 0x3f) * 0x80;
            currentPoly.palOffset += explicitPaletteValue;
            if (chunkOffset[1] & 0x4000) currentPoly.blend = true;
            if (packet[1] & 0x0080) { currentPoly.texscrollx = m_texturescrollx; currentPoly.texscrolly = m_texturescrolly; }
            else { currentPoly.texscrollx = 0; currentPoly.texscrolly = 0; }

            int chunkLength = 0, counter = 3;
            switch (chunkType) {
            case 0x05: case 0x0f:   // 3 vertices, per-vertex UVs & normals, face normal
                for (int m = 0; m < 3; m++) {
                    recoverStandardVerts(currentPoly, m, chunkOffset, counter, packet);
                    for (int c = 0; c < 3; c++) currentPoly.vert[m].normal[c] = uToF(chunkOffset[counter++]);
                    currentPoly.vert[m].normal[3] = 0.0f;
                }
                for (int c = 0; c < 3; c++) currentPoly.faceNormal[c] = uToF(chunkOffset[counter++]);
                currentPoly.faceNormal[3] = 0.0f;
                chunkLength = counter;
                break;
            case 0x04: case 0x0e: case 0x24: case 0x2e:   // 3 vertices, per-vertex UVs
                for (int m = 0; m < 3; m++) recoverStandardVerts(currentPoly, m, chunkOffset, counter, packet);
                for (int c = 0; c < 3; c++) {
                    const float n = uToF(chunkOffset[counter++]);
                    currentPoly.vert[0].normal[c] = currentPoly.vert[1].normal[c] = currentPoly.vert[2].normal[c] = n;
                }
                currentPoly.vert[0].normal[3] = currentPoly.vert[1].normal[3] = currentPoly.vert[2].normal[3] = 0.0f;
                for (int c = 0; c < 3; c++) currentPoly.faceNormal[c] = currentPoly.vert[2].normal[c];
                currentPoly.faceNormal[3] = 0.0f;
                chunkLength = counter;
                break;
            case 0x87: case 0x97: case 0xd7: case 0xc7:   // 1 vertex (strip), UVs & normals, face normal
                currentPoly.vert[1] = lastPoly.vert[0];
                currentPoly.vert[2] = lastPoly.vert[2];
                recoverStandardVerts(currentPoly, 0, chunkOffset, counter, packet);
                for (int c = 0; c < 3; c++) currentPoly.vert[0].normal[c] = uToF(chunkOffset[counter++]);
                currentPoly.vert[0].normal[3] = 0.0f;
                for (int c = 0; c < 3; c++) currentPoly.faceNormal[c] = uToF(chunkOffset[counter++]);
                currentPoly.faceNormal[3] = 0.0f;
                chunkLength = counter;
                break;
            case 0x86: case 0x96: case 0xb6: case 0xc6: case 0xd6:   // 1 vertex (strip), UVs
                currentPoly.vert[1] = lastPoly.vert[0];
                currentPoly.vert[2] = lastPoly.vert[2];
                recoverStandardVerts(currentPoly, 0, chunkOffset, counter, packet);
                for (int c = 0; c < 4; c++) { currentPoly.vert[0].normal[c] = lastPoly.faceNormal[c]; currentPoly.faceNormal[c] = lastPoly.faceNormal[c]; }
                counter += 3;
                chunkLength = counter;
                break;
            default:
                chunkLength = counter;
                break;
            }

            currentPoly.visible = true;
            lastPoly = currentPoly;

            setIdentity(m_modelViewMatrix);
            if ((m_dl_upload >> 10) & 1) matmul4(m_modelViewMatrix, m_modelViewMatrix, m_cameraMatrix);
            matmul4(m_modelViewMatrix, m_modelViewMatrix, objectMatrix);

            if ((packet[1] & 0x0008) && m_lightStrength > 0.0f) {
                for (int v = 0; v < 3; v++) {
                    float transformedNormal[4];
                    vecmatmul4(transformedNormal, objectMatrix, currentPoly.vert[v].normal);
                    normalize(transformedNormal);
                    normalize(m_lightVector);
                    float intensity = vecDotProduct(transformedNormal, m_lightVector) * -1.0f;
                    intensity = (intensity <= 0.0f) ? 0.0f : intensity;
                    intensity *= m_lightStrength * 128.0f;
                    intensity *= 128.0f;
                    if (intensity >= 255.0f) intensity = 255.0f;
                    currentPoly.vert[v].light = intensity;
                }
            } else {
                for (int v = 0; v < 3; v++) currentPoly.vert[v].light = 0;
            }

            float cullRay[4], cullNorm[4];
            vecmatmul4(cullRay, m_modelViewMatrix, currentPoly.vert[0].worldCoords);
            normalize(cullRay);
            vecmatmul4(cullNorm, m_modelViewMatrix, currentPoly.faceNormal);
            if (packet[1] & 0x0010) currentPoly.visible = vecDotProduct(cullRay, cullNorm) < 0.0f;
            vecmatmul4(cullRay, m_modelViewMatrix, currentPoly.vert[0].worldCoords);
            if (cullRay[2] > 0.0f) currentPoly.visible = false;

            if (currentPoly.visible) {
                ClipVert clipVerts[10];
                for (int m = 0; m < currentPoly.n; m++) {
                    float eyeCoords[4];
                    vecmatmul4(eyeCoords, m_modelViewMatrix, currentPoly.vert[m].worldCoords);
                    vecmatmul4(currentPoly.vert[m].clipCoords, m_projectionMatrix, eyeCoords);
                    clipVerts[m] = {currentPoly.vert[m].clipCoords[0], currentPoly.vert[m].clipCoords[1],
                                    currentPoly.vert[m].clipCoords[2], currentPoly.vert[m].clipCoords[3],
                                    {currentPoly.vert[m].texCoords[0], currentPoly.vert[m].texCoords[1], currentPoly.vert[m].light, 0, 0}};
                }
                const int num_vertices = frustum_clip_all(clipVerts, currentPoly.n);
                currentPoly.n = num_vertices;
                for (int m = 0; m < num_vertices; m++) {
                    polyVert& pv = currentPoly.vert[m];
                    pv.clipCoords[0] = clipVerts[m].x; pv.clipCoords[1] = clipVerts[m].y;
                    pv.clipCoords[2] = clipVerts[m].z; pv.clipCoords[3] = clipVerts[m].w;
                    pv.texCoords[0] = clipVerts[m].p[0]; pv.texCoords[1] = clipVerts[m].p[1]; pv.light = clipVerts[m].p[2];
                }
                for (int m = 0; m < currentPoly.n; m++) {
                    polyVert& pv = currentPoly.vert[m];
                    const float ndx = pv.clipCoords[0] / pv.clipCoords[3];
                    const float ndy = pv.clipCoords[1] / pv.clipCoords[3];
                    const float ndz = pv.clipCoords[2] / pv.clipCoords[3];
                    const float ndw = pv.clipCoords[3];
                    pv.clipCoords[0] = (ndx + 1.0f) * (512.0f / 2.0f) + g_view_offx;
                    pv.clipCoords[1] = 512.0f - (ndy + 1.0f) * (512.0f / 2.0f) + g_view_offy;
                    pv.clipCoords[2] = (ndz + 1.0f) * 0.5f;
                    pv.clipCoords[3] = ndw;
                }
            }

            chunkOffset += chunkLength;
            numPolys++;
        }
    }
}

bool command3d(const u16* packet) {
    int numPolys = 0;
    switch (packet[0]) {
    case 0x0000: return false;
    case 0x0001: setCameraTransformation(packet); break;
    case 0x0010: setLighting(packet); break;
    case 0x0011: set3dFlags(packet); break;
    case 0x0012: setCameraProjectionMatrix(packet); break;
    case 0x0100: case 0x0101: recoverPolygonBlock(packet, numPolys); break;
    case 0x0102: {
        u16 mini[16] = {};
        std::copy_n(&packet[0], 7, mini); mini[7] = mini[11] = mini[15] = 0x7fff;
        recoverPolygonBlock(mini, numPolys);
        if (packet[7] == 1 && packet[8] == 0x0102) {
            u16 mini2[16] = {};
            std::copy_n(&packet[8], 7, mini2); mini2[7] = mini2[11] = mini2[15] = 0x7fff;
            recoverPolygonBlock(mini2, numPolys);
        }
        break;
    }
    default: break;   // 0x1000 / 0x1001: unknown global flags
    }
    for (int i = 0; i < numPolys; i++)
        if (m_polys[i].visible) drawShaded(&m_polys[i]);
    return true;
}

} // namespace

extern "C" void video3d_init(const char* rom_dir) {
    std::string d = std::string(rom_dir) + "/buriki/";
    auto read = [](const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) hw_fatal("can't open %s", path.c_str());
        std::vector<u8> v(0x400000);
        if (fread(v.data(), 1, v.size(), f) != v.size()) hw_fatal("can't read %s", path.c_str());
        fclose(f);
        return v;
    };
    m_texturerom.resize(0x1000000);
    const char* tx[4] = {"007tx01a.1", "007tx02a.2", "007tx03a.3", "007tx04a.4"};
    for (int i = 0; i < 4; i++) { auto v = read(d + tx[i]); memcpy(&m_texturerom[i * 0x400000], v.data(), v.size()); }

    // ROM_REGION16_LE + ROM_GROUPWORD | ROM_SKIP(4): one 16-bit word from each chip in turn.
    const char* vt[3] = {"007vt01a.17", "007vt02a.18", "007vt03a.19"};
    m_vertsrom.assign(0x0c00000 / 2, 0);
    for (int c = 0; c < 3; c++) {
        auto v = read(d + vt[c]);
        for (size_t w = 0; w < v.size() / 2; w++) m_vertsrom[w * 3 + c] = (u16)(v[w * 2] | v[w * 2 + 1] << 8);
    }

    // MAME machine_start: texture wrap table defaults to 8 (256 texels).
    uint32_t* wrap = hw_phys_words(0x30000010);
    for (int i = 0; i < 8; i++) wrap[i] = 0x08080808;

    for (auto& z : g_depth_buffer_3d) z = 100.0f;
    setIdentity(m_projectionMatrix); setIdentity(m_modelViewMatrix); setIdentity(m_cameraMatrix);
}

// Display list upload trigger (write to 0x20300200): run the 16 packets at 0x20300000.
extern "C" void video3d_dl_upload(uint32_t data) {
    m_dl_upload = data;
    const uint32_t* w = hw_phys_words(0x20300000);
    u16 dl[0x100];
    for (int i = 0; i < 0x100; i++) dl[i] = (u16)(w[i / 2] >> ((i & 1) ? 0 : 16));
    for (int packetStart = 0; packetStart < 0x100; packetStart += 16)
        if (!command3d(&dl[packetStart])) break;
}

// MAME clear3d (called at vblank when tcram 0x50 bit 16 is set).
extern "C" void video3d_clear(void) {
    for (int i = 0; i < 512 * 512; i++) { g_depth_buffer_3d[i] = 100.0f; g_color_buffer_3d[i] = 0; }
    m_paletteState3d = 0;
    setIdentity(m_projectionMatrix); setIdentity(m_modelViewMatrix); setIdentity(m_cameraMatrix);
    g_view_left = 0; g_view_right = 512; g_view_top = 0; g_view_bottom = 512;
    g_view_offx = g_view_offy = 0.0f;
    g_hd_tris.clear();
}

void video3d_live_tri_count(size_t* n) { *n = g_hd_tris.size(); }
static std::vector<HdTri> g_hd_frame;
void video3d_hd_snapshot(void) { g_hd_frame = g_hd_tris; }
const HdTri* video3d_hd_tris(size_t* count) { *count = g_hd_frame.size(); return g_hd_frame.data(); }
const uint8_t* video3d_texture_rom(size_t* size) { *size = m_texturerom.size(); return m_texturerom.data(); }

extern "C" uint64_t video3d_bad_geometry(void) { return g_bad_geometry; }
