// Direct3D 11 presenter: uploads the 512x448 picture to a texture and draws it with a flip-model swap
// chain into the 4:3 (or stretched / whole-number) area of the window, with the screen filter, the
// darkened + blurred backdrop behind the menu, and the overlay UI on top.
// A high-resolution renderer can draw the picture area instead (hd_hook.h).
// Falls back to GDI (fe_window.cpp) if anything here fails.
//
// Adapted from ReLayer (port/pc/video_d3d11.c), Copyright (c) 2026 The ReLayer project, MIT License
// (see LICENSE-ReLayer.txt in this folder). Converted to C++ and given the hd_hook.h entry point.
#include "fe_internal.h"
#include "hd_hook.h"
#include "settings.h"
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static IDXGISwapChain1* swap;
static ID3D11RenderTargetView* rtv;
static ID3D11Texture2D* back_tex;            // the swap chain's buffer 0 (for the post pass after the HD hook)
static ID3D11Texture2D* tex;                 // the 512x448 software picture
static ID3D11ShaderResourceView* srv;
static ID3D11Texture2D* scratch;             // copy of the picture area drawn by the HD hook
static ID3D11ShaderResourceView* scratch_srv;
static int scratch_w, scratch_h;
static ID3D11VertexShader* vs;
static ID3D11PixelShader* ps;
static ID3D11SamplerState *samp, *samp_linear;
static D3DCompileFn g_compile;
static ID3D11Buffer* blit_cb;
float g_blit_dim, g_blit_blur, g_blit_scan;
int g_blit_smooth;
static const char* g_save_backbuffer;
void d3d_request_backbuffer_save(const char* path) { g_save_backbuffer = path; }
static int tex_w, tex_h, buf_w, buf_h;
static int g_need_resize;
static HWND wnd;
static UINT sync_interval;
static HdDrawFn g_hd_draw;

template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Final picture: optional backdrop (darken + blur + desaturate behind the menu) and scanline filter.
// texel is one arcade pixel in uv units (1/512, 1/448), whatever the source texture's size.
static const char k_shader[] =
    "Texture2D t0 : register(t0); SamplerState s0 : register(s0); SamplerState s1 : register(s1);\n"
    "cbuffer B : register(b0) { float dim; float blur; float scan; float lines; float2 texel; float2 pad; };\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target {\n"
    "  float3 c = t0.Sample(s0, i.uv).rgb;\n"
    "  if (blur > 0) { float3 acc = 0; [unroll] for (int y = -3; y <= 3; y++) [unroll] for (int x = -3; x <= 3; x++)\n"
    "      acc += t0.Sample(s1, i.uv + float2(x, y) * texel * 2.5).rgb;\n"
    "    c = lerp(c, acc / 49.0, blur); }\n"
    "  if (scan > 0) { float k = abs(sin(i.uv.y * lines * 3.14159265)); c *= lerp(1.0, 0.72 + 0.36 * k, scan); }\n"
    "  float g = dot(c, float3(0.299, 0.587, 0.114));\n"
    "  c = lerp(c, g.xxx, dim * 0.5); c *= 1 - dim * 0.55;\n"
    "  return float4(c, 1); }\n";

static void release_views() {
    rel(srv); rel(tex); rel(scratch_srv); rel(scratch); rel(rtv); rel(back_tex);
    tex_w = tex_h = scratch_w = scratch_h = 0;
}

static void release_all() {
    // unbind everything and flush, or the swap chain's destruction is deferred and a new one for the same
    // window is refused (E_ACCESSDENIED)
    if (ctx) { ctx->ClearState(); ctx->Flush(); }
    release_views();
    rel(samp); rel(samp_linear);
    ui_d3d_release();
    rel(blit_cb); rel(ps); rel(vs);
    if (swap) { swap->SetFullscreenState(FALSE, nullptr); rel(swap); }   // never release it full screen
    if (ctx) { ctx->ClearState(); ctx->Flush(); }
    rel(ctx); rel(dev);
}

void d3d_shutdown(void) {
    if (swap) swap->SetFullscreenState(FALSE, nullptr);
    release_all();
}

static int make_rtv() {
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back_tex))) return 0;
    return SUCCEEDED(dev->CreateRenderTargetView(back_tex, nullptr, &rtv));
}

static int create_swapchain(IDXGIFactory2* factory) {
    RECT cr;
    GetClientRect(wnd, &cr);
    buf_w = cr.right > 0 ? cr.right : 640;
    buf_h = cr.bottom > 0 ? cr.bottom : 480;
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = (UINT)buf_w;
    sd.Height = (UINT)buf_h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;           // Windows 10+
    HRESULT hr = factory->CreateSwapChainForHwnd(dev, wnd, &sd, nullptr, nullptr, &swap);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;    // Windows 8
        hr = factory->CreateSwapChainForHwnd(dev, wnd, &sd, nullptr, nullptr, &swap);
    }
    factory->MakeWindowAssociation(wnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr) || !make_rtv()) { fe_log("d3d11: swap chain failed (%08lX)", (unsigned long)hr); return 0; }
    fe_log("d3d11: %s swap chain %dx%d", sd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip-discard" : "flip-sequential",
           buf_w, buf_h);
    return 1;
}

static int recreate_swapchain() {
    IDXGIDevice1* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    int ok = 0;
    ctx->ClearState();
    release_views();
    if (swap) { swap->SetFullscreenState(FALSE, nullptr); rel(swap); }
    ctx->Flush();
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxdev)) &&
        SUCCEEDED(dxdev->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory)))
        ok = create_swapchain(factory);
    rel(factory); rel(adapter); rel(dxdev);
    return ok;
}

static int compile(D3DCompileFn fn, const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    HRESULT hr = fn(k_shader, sizeof k_shader - 1, "present", nullptr, nullptr, entry, target, 0, 0, out, &err);
    if (err) { fe_log("d3d11: shader %s: %s", entry, (const char*)err->GetBufferPointer()); err->Release(); }
    return SUCCEEDED(hr);
}

// ---- graphics cards ----
#define MAX_GPUS 8
static char g_gpu_names[MAX_GPUS][400];
static int g_ngpus, g_gpu_listed;
static IDXGIAdapter1* g_gpus[MAX_GPUS];
static SIZE_T g_gpu_vram[MAX_GPUS];

static void list_gpus() {
    if (g_gpu_listed) return;
    g_gpu_listed = 1;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; g_ngpus < MAX_GPUS && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, g_gpu_names[g_ngpus], sizeof g_gpu_names[0], nullptr, nullptr);
            g_gpu_vram[g_ngpus] = d.DedicatedVideoMemory;
            g_gpus[g_ngpus++] = a;
        } else {
            a->Release();
        }
    }
    f->Release();
}
int d3d_gpu_count(void) { list_gpus(); return g_ngpus; }
const char* d3d_gpu_name(int i) { list_gpus(); return i >= 0 && i < g_ngpus ? g_gpu_names[i] : "?"; }

int d3d_init(HWND hwnd, int refresh_hz, int gpu) {
    (void)refresh_hz;              // pacing is chosen by the window layer (video_set_pacing)
    wnd = hwnd;
    // which graphics card: the chosen one, or automatically the one with the most video memory (on laptops
    // that is the dedicated GPU rather than the integrated one)
    list_gpus();
    IDXGIAdapter* chosen = nullptr;
    if (gpu >= 1 && gpu <= g_ngpus) chosen = g_gpus[gpu - 1];
    else if (g_ngpus > 0) {
        int best = 0;
        for (int i = 1; i < g_ngpus; i++) if (g_gpu_vram[i] > g_gpu_vram[best]) best = i;
        chosen = g_gpus[best];
    }
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompileFn d3dcompile = comp ? (D3DCompileFn)(void*)GetProcAddress(comp, "D3DCompile") : nullptr;
    g_compile = d3dcompile;
    if (!d3dcompile) { fe_log("d3d11: d3dcompiler_47.dll not found"); return 0; }

    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (getenv("BURIKI_FORCE_WARP") ||
        (FAILED(D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 3,
                                  D3D11_SDK_VERSION, &dev, nullptr, &ctx)) &&
         FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 3, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))) {
        // no usable graphics card (old driver, virtual machine...): Windows' own software Direct3D still shows
        // the game and the menu
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 3, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
            fe_log("d3d11: no hardware device, and no software (WARP) device either");
            return 0;
        }
        fe_log("d3d11: no usable graphics card; using Windows' software renderer (WARP)");
    }
    IDXGIDevice1* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxdev)) ||
        FAILED(dxdev->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory))) {
        fe_log("d3d11: DXGI 1.2 not available");
        rel(adapter); rel(dxdev);
        release_all();
        return 0;
    }
    {
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(adapter->GetDesc(&ad))) {
            char name[400];
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
            fe_log("d3d11: graphics adapter: %s", name);
        }
    }
    // one frame may wait in the queue: absorbs an occasional slow frame, costs at most one frame of lag
    dxdev->SetMaximumFrameLatency(2);

    int made = create_swapchain(factory);
    rel(factory); rel(adapter); rel(dxdev);
    if (!made) { release_all(); return 0; }

    ID3DBlob *vb = nullptr, *pb = nullptr;
    if (!compile(d3dcompile, "vs", "vs_4_0", &vb) || !compile(d3dcompile, "ps", "ps_4_0", &pb)) {
        rel(vb);
        release_all();
        return 0;
    }
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps);
    rel(vb); rel(pb);

    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;               // sharp pixels
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&smp, &samp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;              // smooth filter, blur
    dev->CreateSamplerState(&smp, &samp_linear);
    if (!vs || !ps || !samp || !samp_linear) { release_all(); return 0; }
    sync_interval = 0;
    return 1;
}

void d3d_set_sync(int interval) { sync_interval = (UINT)(interval < 0 ? 0 : interval > 4 ? 4 : interval); }
int d3d_vsync(void) { return swap && sync_interval; }

int d3d_set_exclusive(int on, HMONITOR monitor) {
    if (!swap) return 0;
    if (!on) { swap->SetFullscreenState(FALSE, nullptr); g_need_resize = 1; return 1; }
    IDXGIDevice* dx = nullptr;
    IDXGIAdapter* ad = nullptr;
    IDXGIOutput *out = nullptr, *match = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx)) && SUCCEEDED(dx->GetAdapter(&ad))) {
        for (UINT i = 0; ad->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_OUTPUT_DESC d;
            if (!match && SUCCEEDED(out->GetDesc(&d)) && d.Monitor == monitor) { match = out; continue; }
            out->Release();
        }
    }
    HRESULT hr = swap->SetFullscreenState(TRUE, match);
    rel(match); rel(ad); rel(dx);
    if (FAILED(hr)) { fe_log("d3d11: exclusive full screen refused (%08lX)", (unsigned long)hr); return 0; }
    g_need_resize = 1;             // DXGI requires ResizeBuffers after a full-screen change, or Present fails
    return 1;
}

static int ensure_texture(ID3D11Texture2D*& t, ID3D11ShaderResourceView*& v, int& tw, int& th, int w, int h, bool dynamic) {
    if (t && tw == w && th == h) return 1;
    rel(v); rel(t);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &t))) return 0;
    if (FAILED(dev->CreateShaderResourceView(t, nullptr, &v))) return 0;
    tw = w;
    th = h;
    return 1;
}

// ---- hd_hook.h ----
ID3D11Device* fe_d3d_device() { return dev; }
ID3D11DeviceContext* fe_d3d_context() { return ctx; }
void fe_set_hd_draw(HdDrawFn fn) { g_hd_draw = fn; }
bool fe_setting_enhanced() { return g_settings.enhanced != 0; }
int fe_setting_quality() { return g_settings.quality; }
int fe_setting_quality_scale() { return settings_quality_scale(g_settings.quality); }
int fe_setting_filter() { return g_settings.filter; }
int fe_setting_aspect() { return g_settings.aspect; }
bool fe_setting_hud_edges() { return g_settings.hud_edges != 0; }

ID3D11Device* d3d_dev(void) { return dev; }
ID3D11DeviceContext* d3d_ctx(void) { return ctx; }
D3DCompileFn d3d_compiler(void) { return g_compile; }

static int follow_resize() {
    RECT cr;
    GetClientRect(wnd, &cr);
    if (cr.right > 0 && cr.bottom > 0 && (cr.right != buf_w || cr.bottom != buf_h || g_need_resize)) {
        g_need_resize = 0;
        // ResizeBuffers fails while anything still refers to the old buffers, bound views included
        ctx->ClearState();
        rel(rtv); rel(back_tex);
        ctx->Flush();
        HRESULT hr = swap->ResizeBuffers(0, (UINT)cr.right, (UINT)cr.bottom, DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(hr) && make_rtv()) {
            buf_w = cr.right;
            buf_h = cr.bottom;
            fe_log("d3d11: swap chain resized to %dx%d", buf_w, buf_h);
            return 1;
        }
        fe_log("d3d11: resizing the swap chain to %ldx%ld failed (%08lX); making a new one", cr.right, cr.bottom,
               (unsigned long)hr);
        if (recreate_swapchain()) return 1;
        fe_log("d3d11: no swap chain; switching to GDI");
        release_all();
        return 0;
    }
    return 1;
}

static void save_backbuffer(const char* path);

// Draw a picture (whole texture) into the viewport through the filter / backdrop shader.
static void draw_picture(ID3D11ShaderResourceView* picture, bool linear, int px, int py, int pw, int ph) {
    if (!blit_cb) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &blit_cb);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (blit_cb && SUCCEEDED(ctx->Map(blit_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* f = (float*)m.pData;
        // scanlines: one dark gap per pair of the 448 lines (the 224-line look of an arcade monitor)
        f[0] = g_blit_dim; f[1] = g_blit_blur; f[2] = g_blit_scan; f[3] = 224.0f;
        f[4] = 1.0f / 512.0f; f[5] = 1.0f / 448.0f;
        f[6] = f[7] = 0;
        ctx->Unmap(blit_cb, 0);
    }
    D3D11_VIEWPORT vp = {(FLOAT)px, (FLOAT)py, (FLOAT)pw, (FLOAT)ph, 0, 1};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &picture);
    ID3D11SamplerState* smps[2] = {(linear || g_blit_smooth) ? samp_linear : samp, samp_linear};
    ctx->PSSetSamplers(0, 2, smps);
    ctx->PSSetConstantBuffers(0, 1, &blit_cb);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
}

int d3d_present(const uint32_t* rgb, int w, int h) {
    if (!swap || !follow_resize()) return 0;
    int px, py, pw, ph;
    picture_rect(buf_w, buf_h, &px, &py, &pw, &ph);
    static const FLOAT black[4] = {0, 0, 0, 1};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, black);

    // the high-resolution renderer, if one is plugged in and wanted
    bool hd = false;
    if (g_hd_draw && g_settings.enhanced && pw > 0 && ph > 0) {
        int hx = px, hy = py, hw = pw, hh = ph;
        if (g_aspect_mode == ASPECT_WIDE) picture_rect_wide(buf_w, buf_h, &hx, &hy, &hw, &hh);
        hd = g_hd_draw(rtv, hx, hy, hw, hh);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        if (hd) { px = hx; py = hy; pw = hw; ph = hh; }     // the filters below cover what was drawn
    }
    if (!hd) {
        if (!ensure_texture(tex, srv, tex_w, tex_h, w, h, true)) return 0;
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
        for (int y = 0; y < h; y++) memcpy((uint8_t*)m.pData + (size_t)y * m.RowPitch, rgb + (size_t)y * w, (size_t)w * 4);
        ctx->Unmap(tex, 0);
        draw_picture(srv, false, px, py, pw, ph);
    } else if (g_blit_dim > 0 || g_blit_blur > 0 || g_blit_scan > 0) {
        // filters on top of the high-resolution picture: copy its area out and draw it back through the shader
        if (ensure_texture(scratch, scratch_srv, scratch_w, scratch_h, pw, ph, false)) {
            D3D11_BOX box = {(UINT)px, (UINT)py, 0, (UINT)(px + pw), (UINT)(py + ph), 1};
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ctx->CopySubresourceRegion(scratch, 0, 0, 0, 0, back_tex, 0, &box);
            draw_picture(scratch_srv, true, px, py, pw, ph);
        }
    }
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ui_d3d_render(buf_w, buf_h);                    // menu, toasts, frame-rate counter
    if (g_save_backbuffer) { save_backbuffer(g_save_backbuffer); g_save_backbuffer = nullptr; }
    HRESULT hr = swap->Present(sync_interval, 0);
    if (FAILED(hr) && hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) {
        // the swap chain is in a bad state (e.g. after a full-screen change): make a new one and carry on
        static int retries;
        if (retries < 20 && recreate_swapchain()) {
            retries++;
            fe_log("d3d11: present failed (%08lX); made a new swap chain", (unsigned long)hr);
            return 1;
        }
    }
    if (FAILED(hr)) {
        fe_log("d3d11: present failed (%08lX)", (unsigned long)hr);
        release_all();
        return 0;
    }
    return 1;
}

// Save the back buffer (what is about to be shown) as a 24-bit BMP.
static void save_backbuffer(const char* path) {
    ID3D11Texture2D* st = nullptr;
    D3D11_TEXTURE2D_DESC td;
    back_tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return;
    ctx->CopyResource(st, back_tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        FILE* f = fopen(path, "wb");
        if (f) {
            int w = (int)td.Width, h = (int)td.Height, row = (w * 3 + 3) & ~3;
            uint32_t data = (uint32_t)(row * h), file = 54 + data;
            uint8_t hdr[54] = {'B', 'M'};
            memcpy(hdr + 2, &file, 4); hdr[10] = 54; hdr[14] = 40;
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); hdr[26] = 1; hdr[28] = 24; memcpy(hdr + 34, &data, 4);
            fwrite(hdr, 1, 54, f);
            uint8_t* line = (uint8_t*)calloc(1, (size_t)row);
            for (int y = h - 1; y >= 0; y--) {
                const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                for (int x = 0; x < w; x++) { line[x * 3] = src[x * 4]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4 + 2]; }
                fwrite(line, 1, (size_t)row, f);
            }
            free(line);
            fclose(f);
            fe_log("saved screen to %s", path);
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
}
