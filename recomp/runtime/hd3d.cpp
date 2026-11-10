// High-resolution renderer (Direct3D 11). Redraws the 3D triangles the hardware drew this frame
// (recorded by video3d.cpp) at the output resolution, then composites them with the 2D layers.
//
// It follows the software renderer's rules exactly: the same texture page/wrap/scroll maths, the
// same 16-bit colour value (palette offset + texel, 16 brightness palettes, blend bit), the same
// depth test, the same "colour 0 is see-through" rule and the same 50% blend against the 2D
// picture behind. Only the resolution changes, plus optional texture smoothing and anti-aliasing.
#include "hd3d.h"
#include "video_layers.h"
#include "frontend/hd_hook.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
bool g_failed = false;

ID3D11Texture2D* g_tex_rom = nullptr;        ID3D11ShaderResourceView* g_srv_rom = nullptr;
ID3D11Texture2D* g_tex_pal = nullptr;        ID3D11ShaderResourceView* g_srv_pal = nullptr;
ID3D11Texture2D* g_tex_below = nullptr;      ID3D11ShaderResourceView* g_srv_below = nullptr;
ID3D11Texture2D* g_tex_over = nullptr;       ID3D11ShaderResourceView* g_srv_over = nullptr;
ID3D11Texture2D* g_rt_ms = nullptr;          ID3D11RenderTargetView* g_rtv_ms = nullptr;
ID3D11Texture2D* g_ds_ms = nullptr;          ID3D11DepthStencilView* g_dsv_ms = nullptr;
ID3D11Texture2D* g_rt_res = nullptr;         ID3D11ShaderResourceView* g_srv_res = nullptr;
int g_rt_w = 0, g_rt_h = 0, g_rt_samples = 0;

ID3D11VertexShader* g_vs3d = nullptr;  ID3D11PixelShader* g_ps3d = nullptr;  ID3D11InputLayout* g_il3d = nullptr;
ID3D11VertexShader* g_vsfs = nullptr;  ID3D11PixelShader* g_pscomp = nullptr;
ID3D11Buffer* g_vb = nullptr; size_t g_vb_cap = 0;
ID3D11Buffer* g_cb = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
ID3D11DepthStencilState* g_dss_3d = nullptr;  ID3D11DepthStencilState* g_dss_off = nullptr;
ID3D11BlendState* g_bs_off = nullptr;
ID3D11SamplerState* g_samp_point = nullptr;  ID3D11SamplerState* g_samp_linear = nullptr;
ID3D11SamplerState* g_samp_linear_clamp = nullptr;

struct GpuVert {
    float x, y, z, w;          // buffer px, depth, clip w
    float s, t, light, pad;
    uint32_t p0, p1, p2, p3;   // packed per-triangle parameters (see shader)
};

struct Consts {
    float out_w, out_h, xscroll, enable3d;
    uint32_t palbase, smooth_tex, smooth_2d;
    float wide;                // 1 = the arcade's 4:3 view; 4/3 = 16:9 (more of the scene at the sides)
    float lay_m, lay_w, pad0, pad1;   // 2D layer textures: column of screen x = 0, texture width
};

const char* kShader3D = R"(
Texture2DArray<uint> rom : register(t0);
Texture2D<float4> pal : register(t1);
cbuffer C : register(b0) { float out_w, out_h, xscroll, enable3d; uint palbase, smooth_tex, smooth_2d; float wide; float lay_m, lay_w, cpad0, cpad1; };
struct VI { float4 p : POS; float4 a : ATTR; uint4 q : PARAMS; };
struct VO { float4 pos : SV_Position; float3 a : ATTR; nointerpolation uint4 q : PARAMS; };
VO vs(VI i) {
    VO o;
    float w = i.p.w;
    float nx = (i.p.x / 256.0 - 1.0) / wide, ny = 1.0 - i.p.y / 256.0;
    o.pos = float4(nx * w, ny * w, i.p.z * w, w);
    o.a = i.a.xyz;
    o.q = i.q;
    return o;
}
// q.x: palOffset(16) | colorIndex<<16     q.y: texPageSmall(16) | texIndex<<16 | flags<<24 (1 flat, 2 4bpp, 4 blend)
// q.z: texscrollx | texscrolly<<16         q.w: tex_mask_x | tex_mask_y<<16
uint texel(uint page, bool bpp4, int ti, int si) {
    ti &= 1023; si &= 1023;
    if (bpp4) {
        uint idx = (uint)si * 512u + ((uint)ti >> 1);
        uint b = rom.Load(int4(idx & 1023u, idx >> 10, page, 0));
        return (ti & 1) ? ((b >> 4) & 15u) : (b & 15u);
    }
    return rom.Load(int4(ti, si, page, 0));
}
float4 resolve(uint c16) {
    // MAME composite: colour 0 (low 11 bits) is see-through; bit 11 = 50% blend.
    if ((c16 & 0x7ffu) == 0u) return float4(0, 0, 0, 0);
    float4 c = pal.Load(int3(((c16 & 0xf7ffu) | palbase) & 255u, (((c16 & 0xf7ffu) | palbase) >> 8) & 255u, 0));
    return float4(c.rgb, (c16 & 0x800u) ? 0.5 : 1.0);
}
float4 ps(VO i) : SV_Target {
    uint palOffset = i.q.x & 0xffffu, colorIndex = i.q.x >> 16;
    uint flags = i.q.y >> 24;
    if (flags & 1u) return resolve((palOffset + colorIndex) & 0xffffu);
    // perspective-correct s, t, light (the GPU did the divide)
    float sC = i.a.x, tC = i.a.y, rC = i.a.z;
    uint page = (i.q.y >> 16) & 15u, pageSmall = i.q.y & 0xffffu;
    bool bpp4 = (flags & 2u) != 0u, blend = (flags & 4u) != 0u;
    float S = sC * 1024.0 + (float)(((i.q.z >> 16) & 0x3fffu) >> 5);
    float T = tC * 1024.0 + (float)((i.q.z & 0x3fffu) >> 5);
    uint sub = (pageSmall & 0xc000u) >> 14;
    float hoff = (float)((pageSmall & 0x3f80u) >> 7), voff = (float)(pageSmall & 0x7fu);
    float maskx = (float)(i.q.w & 0xffffu), masky = (float)(i.q.w >> 16);
    uint lightval = (uint)(rC / 16.0);
    uint lightbits = (lightval & 15u) << 12;
    if (!smooth_tex) {
        if (sub & 2u) { T = fmod(T, maskx) + 8.0 * hoff; S = fmod(S, masky) + 8.0 * voff; }
        uint e = texel(page, bpp4, (int)T, (int)S);
        if (e == 0u) discard;                       // texel 0: nothing drawn, no depth write
        return resolve(((palOffset + e) & 0x7ffu) | lightbits | (blend ? 0x800u : 0u));
    }
    // Smooth textures: bilinear over the 4 nearest texels, each resolved through the palette,
    // with transparent texels weighted out. Keeps the same page/wrap rules per texel.
    float fT = T - 0.5, fS = S - 0.5;
    float bT = floor(fT), bS = floor(fS);
    float wT = fT - bT, wS = fS - bS;
    float4 acc = 0; float cov = 0;
    [unroll] for (int k = 0; k < 4; k++) {
        float tt = bT + (k & 1), ss = bS + (k >> 1);
        if (sub & 2u) { tt = fmod(fmod(tt, maskx) + maskx, maskx) + 8.0 * hoff; ss = fmod(fmod(ss, masky) + masky, masky) + 8.0 * voff; }
        float wk = ((k & 1) ? wT : 1.0 - wT) * ((k >> 1) ? wS : 1.0 - wS);
        uint e = texel(page, bpp4, (int)tt, (int)ss);
        if (e != 0u) {
            float4 c = resolve(((palOffset + e) & 0x7ffu) | lightbits | (blend ? 0x800u : 0u));
            if (c.a > 0) { acc += float4(c.rgb, 1) * wk; cov += wk; }
            else { acc.w += 0; }
        }
    }
    if (cov < 0.5) discard;
    return float4(acc.rgb / cov, blend ? 0.5 : 1.0);
}
)";

const char* kShaderComp = R"(
Texture2D<float4> below : register(t0);
Texture2D<float4> over : register(t1);
Texture2D<float4> hd : register(t2);
SamplerState sp : register(s0);
SamplerState sl : register(s1);
SamplerState slc : register(s2);
cbuffer C : register(b0) { float out_w, out_h, xscroll, enable3d; uint palbase, smooth_tex, smooth_2d; float wide; float lay_m, lay_w, cpad0, cpad1; };
struct VO { float4 pos : SV_Position; float2 uv : UV; };
VO vs(uint id : SV_VertexID) {
    VO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VO i) : SV_Target {
    // i.uv: 0..1 over the output. uv: 0..1 over the arcade's 512x448 picture, which is the middle
    // of the output in widescreen. The 2D layer textures hold lay_m extra columns on each side.
    float2 uv = float2((i.uv.x - 0.5) * wide + 0.5, i.uv.y);
    float2 uvc = float2((clamp(uv.x * 512.0, 0.5 - lay_m, 511.5 + lay_m) + lay_m) / lay_w, uv.y);
    float3 b = smooth_2d ? below.Sample(sl, uvc).rgb : below.Sample(sp, uvc).rgb;
    float3 col = b;
    if (enable3d > 0.5) {
        // MAME: 3D buffer row = picture line * 512/448, column = x + xscroll (wrapping at 512)
        float4 c;
        if (wide > 1.001) c = hd.Sample(slc, float2(i.uv.x + xscroll / (512.0 * wide), i.uv.y));
        else c = hd.Sample(sl, float2(frac((uv.x * 512.0 + xscroll) / 512.0), uv.y));
        col = lerp(col, c.rgb, c.a);
    }
    // In-front 2D layer: alpha encodes how it combines with what's behind:
    // 1 = opaque, 0.75 = 50% blend, 0.5 = additive, 0 = nothing.
    float4 o = over.Sample(sp, uvc);
    float3 oc = smooth_2d ? over.Sample(sl, uvc).rgb : o.rgb;
    if (o.a > 0.875) col = oc;
    else if (o.a > 0.625) col = lerp(col, oc, 0.5);
    else if (o.a > 0.25) col = saturate(col + oc);
    return float4(col, 1);
}
)";

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
D3DCompileFn g_compile = nullptr;

ID3DBlob* compile(const char* src, const char* entry, const char* target) {
    if (!g_compile) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        if (m) g_compile = (D3DCompileFn)GetProcAddress(m, "D3DCompile");
        if (!g_compile) { fprintf(stderr, "[hd] d3dcompiler_47.dll not available\n"); return nullptr; }
    }
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = g_compile(src, strlen(src), "hd3d", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[hd] shader %s failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
        release(err);
        return nullptr;
    }
    release(err);
    return code;
}

bool make_tex(int w, int h, DXGI_FORMAT fmt, bool dynamic, ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv, const void* init = nullptr, int pitch = 0) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    D3D11_SUBRESOURCE_DATA sd = {init, (UINT)pitch, 0};
    if (FAILED(g_dev->CreateTexture2D(&d, init ? &sd : nullptr, tex))) return false;
    return SUCCEEDED(g_dev->CreateShaderResourceView(*tex, nullptr, srv));
}

void upload(ID3D11Texture2D* tex, const void* data, int row_bytes, int rows) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    for (int y = 0; y < rows; y++) memcpy((uint8_t*)m.pData + y * m.RowPitch, (const uint8_t*)data + y * row_bytes, row_bytes);
    g_ctx->Unmap(tex, 0);
}

void release_all() {
    release(g_srv_rom); release(g_tex_rom); release(g_srv_pal); release(g_tex_pal);
    release(g_srv_below); release(g_tex_below); release(g_srv_over); release(g_tex_over);
    release(g_rtv_ms); release(g_rt_ms); release(g_dsv_ms); release(g_ds_ms); release(g_srv_res); release(g_rt_res);
    g_rt_w = g_rt_h = g_rt_samples = 0;
    release(g_vs3d); release(g_ps3d); release(g_il3d); release(g_vsfs); release(g_pscomp);
    release(g_vb); g_vb_cap = 0; release(g_cb); release(g_rs); release(g_dss_3d); release(g_dss_off);
    release(g_bs_off); release(g_samp_point); release(g_samp_linear); release(g_samp_linear_clamp);
    g_dev = nullptr; g_ctx = nullptr;
}

bool init_once() {
    // The front end may restart Direct3D (device loss): rebuild on a new device.
    if (g_dev && g_dev != fe_d3d_device()) { release_all(); g_failed = false; }
    if (g_dev) return true;
    if (g_failed) return false;
    g_failed = true;   // until everything below succeeds
    g_dev = fe_d3d_device();
    g_ctx = fe_d3d_context();
    if (!g_dev || !g_ctx) return false;

    // Texture ROM: 16 pages of 1024x1024 bytes, as an R8_UINT array.
    size_t rom_size = 0;
    const uint8_t* rom = video3d_texture_rom(&rom_size);
    if (!rom || rom_size < 16u * 1024 * 1024) return false;
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 1024; d.Height = 1024; d.MipLevels = 1; d.ArraySize = 16; d.Format = DXGI_FORMAT_R8_UINT;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd[16];
        for (int i = 0; i < 16; i++) sd[i] = {rom + (size_t)i * 1024 * 1024, 1024, 0};
        if (FAILED(g_dev->CreateTexture2D(&d, sd, &g_tex_rom))) return false;
        if (FAILED(g_dev->CreateShaderResourceView(g_tex_rom, nullptr, &g_srv_rom))) return false;
    }
    if (!make_tex(256, 256, DXGI_FORMAT_B8G8R8A8_UNORM, true, &g_tex_pal, &g_srv_pal)) return false;
    if (!make_tex(HD_LAYER_MAX_W, 448, DXGI_FORMAT_B8G8R8A8_UNORM, true, &g_tex_below, &g_srv_below)) return false;
    if (!make_tex(HD_LAYER_MAX_W, 448, DXGI_FORMAT_B8G8R8A8_UNORM, true, &g_tex_over, &g_srv_over)) return false;

    ID3DBlob* b = compile(kShader3D, "vs", "vs_5_0");
    if (!b || FAILED(g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs3d))) return false;
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"ATTR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PARAMS", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(g_dev->CreateInputLayout(il, 3, b->GetBufferPointer(), b->GetBufferSize(), &g_il3d))) return false;
    release(b);
    b = compile(kShader3D, "ps", "ps_5_0");
    if (!b || FAILED(g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_ps3d))) return false;
    release(b);
    b = compile(kShaderComp, "vs", "vs_5_0");
    if (!b || FAILED(g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vsfs))) return false;
    release(b);
    b = compile(kShaderComp, "ps", "ps_5_0");
    if (!b || FAILED(g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_pscomp))) return false;
    release(b);

    D3D11_BUFFER_DESC cbd = {sizeof(Consts), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (FAILED(g_dev->CreateBuffer(&cbd, nullptr, &g_cb))) return false;

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE; rd.MultisampleEnable = TRUE;
    if (FAILED(g_dev->CreateRasterizerState(&rd, &g_rs))) return false;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(g_dev->CreateDepthStencilState(&dd, &g_dss_3d))) return false;
    dd.DepthEnable = FALSE;
    if (FAILED(g_dev->CreateDepthStencilState(&dd, &g_dss_off))) return false;
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_dev->CreateBlendState(&bd, &g_bs_off))) return false;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_dev->CreateSamplerState(&sd, &g_samp_point))) return false;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_BORDER; // widescreen: nothing beyond the 3D picture (border = 0,0,0,0)
    if (FAILED(g_dev->CreateSamplerState(&sd, &g_samp_linear_clamp))) return false;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;   // 3D buffer wraps horizontally (xscroll)
    if (FAILED(g_dev->CreateSamplerState(&sd, &g_samp_linear))) return false;

    g_failed = false;
    fprintf(stderr, "[hd] high-resolution renderer ready\n");
    return true;
}

// Can this frame be shown wider than 4:3? Only the arena scenes (fights, style intros): a
// perspective floor is on screen and every 3D polygon uses the whole screen as its window. Menus,
// the title and the other flat screens were drawn for 4:3 and stay that shape.
// Env BURIKI_WIDE_ALWAYS=1 widens everything (diagnostics).
bool wide_scene(const HdLayers& L, const HdTri* tris, size_t ntri) {
    static const bool force = getenv("BURIKI_WIDE_ALWAYS") != nullptr;
    if (force) return true;
    if (!L.arena) return false;
    for (size_t i = 0; i < ntri; i++)
        if (tris[i].clip_l > 0 || tris[i].clip_r < 512) return false;
    return true;
}

bool ensure_targets(int w, int h, int samples) {
    if (w == g_rt_w && h == g_rt_h && samples == g_rt_samples && g_rtv_ms) return true;
    release(g_rtv_ms); release(g_rt_ms); release(g_dsv_ms); release(g_ds_ms); release(g_srv_res); release(g_rt_res);
    UINT q = 0;
    if (samples > 1 && (FAILED(g_dev->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, samples, &q)) || q == 0)) samples = 1;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = samples; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_rt_ms)) || FAILED(g_dev->CreateRenderTargetView(g_rt_ms, nullptr, &g_rtv_ms))) return false;
    d.Format = DXGI_FORMAT_D32_FLOAT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_ds_ms)) || FAILED(g_dev->CreateDepthStencilView(g_ds_ms, nullptr, &g_dsv_ms))) return false;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_rt_res)) || FAILED(g_dev->CreateShaderResourceView(g_rt_res, nullptr, &g_srv_res))) return false;
    g_rt_w = w; g_rt_h = h; g_rt_samples = samples;
    return true;
}

bool hd_draw(ID3D11RenderTargetView* target, int vx, int vy, int vw, int vh) {
    if (!init_once()) return false;
    HdLayers L;
    if (!video_hd_layers(&L)) return false;   // e.g. screen disabled: use the normal picture

    size_t ntri = 0;
    const HdTri* tris = video3d_hd_tris(&ntri);

    // Widescreen (Picture shape): the front end hands over an area wider than 4:3. `wide` is how
    // many times wider than the arcade picture the 3D view becomes; the 2D layers stay in the
    // middle. Screens that aren't a full-screen 3D scene keep the arcade shape, centred.
    float wide = 1.0f;
    if (fe_setting_aspect() == 3 && vh > 0) {
        const float k = ((float)vw / (float)vh) / (4.0f / 3.0f);
        if (k > 1.001f && L.margin > 0 && wide_scene(L, tris, ntri)) wide = std::min(k, 1.0f + L.margin / 256.0f);
        // (while the pause menu is open this frame is drawn again and again: keep what it was made with)
        else {                                   // 4:3 in the middle of the wide area
            const int w43 = vh * 4 / 3;
            if (w43 < vw) { vx += (vw - w43) / 2; vw = w43; }
        }
    }

    // For the next frame the game draws: wide 2D layers when the picture shape is Widescreen, and,
    // with "HUD at the screen edges", how far out to draw the fight display's side parts: the
    // width of the extra picture on each side, in arcade pixels. Only when this frame went wide,
    // so the display isn't pushed out of a 4:3 picture.
    {
        const bool widescreen = fe_setting_aspect() == 3;
        const int shift = (widescreen && wide > 1.001f && fe_setting_hud_edges()) ? (int)((wide - 1.0f) * 256.0f + 0.5f) : 0;
        video_set_wide_layers(widescreen, shift);
    }

    // 3D target: the 512x512 buffer covers the picture width and 512/448 of its height.
    // Quality Auto = the output resolution; presets = a fixed multiple of the arcade's 512 lines.
    const int q = fe_setting_quality_scale();
    const int rw = q ? (int)(512 * q * wide + 0.5f) : vw, rh = q ? 512 * q : (int)(vh * 512.0f / 448.0f + 0.5f);
    if (rw < 64 || rh < 64 || !ensure_targets(rw, rh, 4)) return false;

    upload(g_tex_pal, L.pal_3d, 256 * 4, 256);
    upload(g_tex_below, L.below, L.width * 4, 448);
    upload(g_tex_over, L.over, L.width * 4, 448);

    // Enhanced mode smooths 3D textures; the 2D layers follow the screen filter (Smooth = bilinear).
    Consts c = {(float)vw, (float)vh, (float)L.xscroll, L.enable3d ? 1.0f : 0.0f, L.palbase, 1u, (uint32_t)(fe_setting_filter() == 1), wide,
                (float)L.margin, (float)HD_LAYER_MAX_W, 0, 0};
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &c, sizeof(c)); g_ctx->Unmap(g_cb, 0); }

    // ---- 3D pass ----
    const float clear0[4] = {0, 0, 0, 0};
    g_ctx->ClearRenderTargetView(g_rtv_ms, clear0);
    g_ctx->ClearDepthStencilView(g_dsv_ms, D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (L.enable3d && ntri) {
        std::vector<GpuVert> verts(ntri * 3);
        for (size_t i = 0; i < ntri; i++) {
            const HdTri& t = tris[i];
            const uint32_t flags = (t.flat ? 1u : 0u) | (t.tex4bpp ? 2u : 0u) | (t.blend ? 4u : 0u);
            const uint32_t p0 = (t.palOffset & 0xffff) | ((uint32_t)t.colorIndex << 16);
            const uint32_t p1 = t.texPageSmall | ((uint32_t)(t.texIndex & 15) << 16) | (flags << 24);
            const uint32_t p2 = t.texscrollx | ((uint32_t)t.texscrolly << 16);
            const uint32_t p3 = t.tex_mask_x | ((uint32_t)t.tex_mask_y << 16);
            for (int k = 0; k < 3; k++) {
                const HdVert& v = t.v[k];
                // Flat polygons interpolate z linearly in screen space (MAME): use w = 1.
                const float w = t.flat ? 1.0f : (v.w > 1e-6f ? v.w : 1e-6f);
                verts[i * 3 + k] = {v.x, v.y, v.z, w, v.s, v.t, v.light, 0, p0, p1, p2, p3};
            }
        }
        const size_t bytes = verts.size() * sizeof(GpuVert);
        if (bytes > g_vb_cap) {
            release(g_vb);
            g_vb_cap = bytes * 2;
            D3D11_BUFFER_DESC vbd = {(UINT)g_vb_cap, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
            if (FAILED(g_dev->CreateBuffer(&vbd, nullptr, &g_vb))) { g_vb_cap = 0; return false; }
        }
        if (SUCCEEDED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, verts.data(), bytes); g_ctx->Unmap(g_vb, 0); }

        D3D11_VIEWPORT vp = {0, 0, (float)rw, (float)rh, 0, 1};
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(g_rs);
        g_ctx->OMSetRenderTargets(1, &g_rtv_ms, g_dsv_ms);
        g_ctx->OMSetDepthStencilState(g_dss_3d, 0);
        g_ctx->OMSetBlendState(g_bs_off, nullptr, 0xffffffff);
        g_ctx->IASetInputLayout(g_il3d);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride = sizeof(GpuVert), off = 0;
        g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &off);
        g_ctx->VSSetShader(g_vs3d, nullptr, 0);
        g_ctx->PSSetShader(g_ps3d, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {g_srv_rom, g_srv_pal};
        g_ctx->PSSetShaderResources(0, 2, srvs);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
        g_ctx->VSSetConstantBuffers(0, 1, &g_cb);

        // Draw in order, one call per run of triangles sharing a clip rectangle.
        // buffer x 256 is the middle of the target; 512 buffer pixels span rw / wide
        const float sx = rw / (512.0f * wide), sy = rh / 512.0f, x0 = rw * 0.5f - 256.0f * sx;
        size_t start = 0;
        while (start < ntri) {
            size_t end = start + 1;
            const HdTri& a = tris[start];
            while (end < ntri && tris[end].clip_l == a.clip_l && tris[end].clip_r == a.clip_r &&
                   tris[end].clip_t == a.clip_t && tris[end].clip_b == a.clip_b) end++;
            D3D11_RECT r = {(LONG)(x0 + a.clip_l * sx), (LONG)(a.clip_t * sy), (LONG)(x0 + (a.clip_r + 1) * sx), (LONG)((a.clip_b + 1) * sy)};
            if (a.clip_l <= 0 && a.clip_r >= 512) { r.left = 0; r.right = rw; }   // full-screen view: the whole width
            g_ctx->RSSetScissorRects(1, &r);
            g_ctx->Draw((UINT)((end - start) * 3), (UINT)(start * 3));
            start = end;
        }
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        g_ctx->PSSetShaderResources(0, 2, none);
    }
    if (g_rt_samples > 1) g_ctx->ResolveSubresource(g_rt_res, 0, g_rt_ms, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    else g_ctx->CopyResource(g_rt_res, g_rt_ms);

    // ---- composite into the picture area of the back buffer ----
    D3D11_VIEWPORT vp = {(float)vx, (float)vy, (float)vw, (float)vh, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    D3D11_RECT full = {vx, vy, vx + vw, vy + vh};
    g_ctx->RSSetScissorRects(1, &full);
    g_ctx->OMSetRenderTargets(1, &target, nullptr);
    g_ctx->OMSetDepthStencilState(g_dss_off, 0);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vsfs, nullptr, 0);
    g_ctx->PSSetShader(g_pscomp, nullptr, 0);
    ID3D11ShaderResourceView* srvs[3] = {g_srv_below, g_srv_over, g_srv_res};
    g_ctx->PSSetShaderResources(0, 3, srvs);
    ID3D11SamplerState* samps[3] = {g_samp_point, g_samp_linear, g_samp_linear_clamp};
    g_ctx->PSSetSamplers(0, 3, samps);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[3] = {nullptr, nullptr, nullptr};
    g_ctx->PSSetShaderResources(0, 3, none);
    return true;
}

} // namespace

void hd3d_install(void) { fe_set_hd_draw(hd_draw); }
