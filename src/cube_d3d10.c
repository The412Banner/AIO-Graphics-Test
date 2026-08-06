// AIO Graphics Test - Direct3D 10 cube backend.
//
// A spinning, multi-colored cube rendered with Direct3D 10 (vs_4_0 / ps_4_0).
// Under Winlator this exercises the DXVK d3d10 -> d3d11 -> Vulkan -> Turnip
// translation path (DXVK implements d3d10 as a thin layer over d3d11), filling
// the gap between the DX9 and DX11 cubes so 8/9/10/11/12 are all covered.
//
// d3d10.dll is loaded dynamically (D3D10CreateDeviceAndSwapChain via
// GetProcAddress) and HLSL is compiled at runtime via d3dcompiler, so the .exe
// has no static dependency on either and still launches without DXVK, showing a
// graceful notice instead.
//
// Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600  // GetTickCount64
#endif
#include <windows.h>
#include <d3d10.h>
#include <d3dcompiler.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cube_d3d10.h"
#include "cube_embed.h"
#include "hud.h"
#include "bench.h"
#include "watchdog.h"

static int g_w = 640, g_h = 480;
static int g_quit;
// Pending window resize -> refresh the projection aspect in the loop, so the cube
// isn't stretched when the window is maximized/resized.
static volatile int g_resize_pending = 0;
static int g_resize_w = 0, g_resize_h = 0;

static LRESULT CALLBACK d3d10_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                g_quit = 1;
                PostQuitMessage(0);
            }
            return 0;
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) {
                int nw = LOWORD(l), nh = HIWORD(l);
                if (nw > 0 && nh > 0) { g_resize_w = nw; g_resize_h = nh; g_resize_pending = 1; }
            }
            return 0;
        case WM_CLOSE:
            g_quit = 1;
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcA(h, m, w, l);
}

// --- row-major / row-vector matrix math (v' = v * M), matching the row_major
//     cbuffer convention used by the shader below. ---
typedef struct {
    float m[16];
} Mat4;

static Mat4 mat_identity(void) {
    Mat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}
static Mat4 mat_mul(Mat4 a, Mat4 b) {
    Mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
static Mat4 mat_translate(float x, float y, float z) {
    Mat4 r = mat_identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}
static Mat4 mat_rotate(float ax, float ay, float az, float angle_rad) {
    float len = sqrtf(ax * ax + ay * ay + az * az);
    if (len > 0.0f) { ax /= len; ay /= len; az /= len; }
    float c = cosf(angle_rad), s = sinf(angle_rad), t = 1.0f - c;
    Mat4 r = mat_identity();
    r.m[0] = c + ax * ax * t;       r.m[1] = ax * ay * t + az * s;  r.m[2] = ax * az * t - ay * s;
    r.m[4] = ay * ax * t - az * s;  r.m[5] = c + ay * ay * t;       r.m[6] = ay * az * t + ax * s;
    r.m[8] = az * ax * t + ay * s;  r.m[9] = az * ay * t - ax * s;  r.m[10] = c + az * az * t;
    return r;
}
static Mat4 mat_perspective(float fovy_rad, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    Mat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = zn * zf / (zn - zf);
    return r;
}

typedef struct {
    float pos[3];
    float col[3];
} ColVertex;

static const float kFace[6][4][3] = {
    {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
    {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
    {{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}},
    {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
    {{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},
    {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
};
static const float kFaceCol[6][3] = {
    {0.90f, 0.20f, 0.20f}, {0.20f, 0.80f, 0.30f}, {0.25f, 0.45f, 0.95f},
    {0.95f, 0.80f, 0.20f}, {0.85f, 0.40f, 0.90f}, {0.20f, 0.85f, 0.90f},
};
static const int kQuadIdx[6] = {0, 1, 2, 0, 2, 3};

static void build_color_cube(ColVertex *out) {
    int v = 0;
    for (int face = 0; face < 6; face++)
        for (int k = 0; k < 6; k++) {
            int ci = kQuadIdx[k];
            out[v].pos[0] = kFace[face][ci][0];
            out[v].pos[1] = kFace[face][ci][1];
            out[v].pos[2] = kFace[face][ci][2];
            out[v].col[0] = kFaceCol[face][0];
            out[v].col[1] = kFaceCol[face][1];
            out[v].col[2] = kFaceCol[face][2];
            v++;
        }
}

static const char *kSpinHLSL =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; };\n"
    "struct VSIn { float3 pos : POSITION; float3 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float3 col : COLOR; };\n"
    "VSOut VSMain(VSIn i) { VSOut o; o.pos = mul(float4(i.pos,1.0), mvp); o.col = i.col; return o; }\n"
    "float4 PSMain(VSOut i) : SV_TARGET { return float4(i.col, 1.0); }\n";

typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                        ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
                                        ID3DBlob **);
typedef HRESULT(WINAPI *PFN_D3D10CreateDeviceAndSwapChain)(IDXGIAdapter *, D3D10_DRIVER_TYPE,
                                                           HMODULE, UINT, UINT,
                                                           DXGI_SWAP_CHAIN_DESC *,
                                                           IDXGISwapChain **, ID3D10Device **);

static PFN_D3DCompile g_compile;

static void fail_box(const char *msg) {
    MessageBoxA(NULL, msg, "AIO Graphics Test - Direct3D 10", MB_OK | MB_ICONERROR);
}

static ID3DBlob *compile_hlsl(const char *src, const char *entry, const char *target) {
    ID3DBlob *out = NULL, *err = NULL;
    HRESULT hr = g_compile(src, strlen(src), "aio_d3d10", NULL, NULL, entry, target, 0, 0, &out, &err);
    if (FAILED(hr)) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "Shader '%s' (%s) failed to compile.\n\n%s", entry, target,
                 err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        fail_box(msg);
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return out;
}

int aio_run_d3d10_cube(HINSTANCE hinst) {
    const char *api = "Direct3D 10";
    const char *cls = "AIOD3D10Cube";

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = d3d10_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(hinst, MAKEINTRESOURCEA(1));
    wc.lpszClassName = cls;
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(cls, "AIO Graphics Test  -  Direct3D 10",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640,
                              480, NULL, NULL, hinst, NULL);
    if (!hwnd) return 1;
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_w = rc.right - rc.left;
    g_h = rc.bottom - rc.top;
    if (g_w <= 0) g_w = 640;
    if (g_h <= 0) g_h = 480;

    HMODULE d3d10lib = LoadLibraryA("d3d10.dll");
    PFN_D3D10CreateDeviceAndSwapChain p_create =
        d3d10lib ? (PFN_D3D10CreateDeviceAndSwapChain)GetProcAddress(d3d10lib,
                                                                     "D3D10CreateDeviceAndSwapChain")
                 : NULL;
    if (!p_create) {
        fail_box(
            "Direct3D 10 is not available in this container.\n\n"
            "Could not load d3d10.dll (is DXVK installed?).");
        DestroyWindow(hwnd);
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC scd;
    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = 1;
    scd.BufferDesc.Width = g_w;
    scd.BufferDesc.Height = g_h;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    ID3D10Device *dev = NULL;
    IDXGISwapChain *swap = NULL;
    HRESULT hr = p_create(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &scd, &swap,
                          &dev);
    if (FAILED(hr) || !dev) {
        fail_box(
            "Direct3D 10 is not available in this container.\n\n"
            "Could not create a D3D10 device + swapchain (no d3d10.dll / DXVK?).");
        DestroyWindow(hwnd);
        return 1;
    }

    ID3D10Texture2D *backbuf = NULL;
    ID3D10RenderTargetView *rtv = NULL;
    IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D10Texture2D, (void **)&backbuf);
    ID3D10Device_CreateRenderTargetView(dev, (ID3D10Resource *)backbuf, NULL, &rtv);

    D3D10_TEXTURE2D_DESC dd;
    memset(&dd, 0, sizeof(dd));
    dd.Width = g_w;
    dd.Height = g_h;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D10_USAGE_DEFAULT;
    dd.BindFlags = D3D10_BIND_DEPTH_STENCIL;
    ID3D10Texture2D *depth_tex = NULL;
    ID3D10DepthStencilView *dsv = NULL;
    ID3D10Device_CreateTexture2D(dev, &dd, NULL, &depth_tex);
    ID3D10Device_CreateDepthStencilView(dev, (ID3D10Resource *)depth_tex, NULL, &dsv);

    D3D10_DEPTH_STENCIL_DESC dsd;
    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D10_COMPARISON_LESS;
    ID3D10DepthStencilState *dss = NULL;
    ID3D10Device_CreateDepthStencilState(dev, &dsd, &dss);
    ID3D10Device_OMSetDepthStencilState(dev, dss, 1);
    ID3D10Device_OMSetRenderTargets(dev, 1, &rtv, dsv);

    // CULL_NONE like the GL/Vulkan cubes: the cube is wound CCW-front (GL
    // convention), but D3D's default rasterizer is CULL_BACK + CW-front, which
    // would cull the outward faces and render the cube inside-out.
    D3D10_RASTERIZER_DESC rsd;
    memset(&rsd, 0, sizeof(rsd));
    rsd.FillMode = D3D10_FILL_SOLID;
    rsd.CullMode = D3D10_CULL_NONE;
    rsd.DepthClipEnable = TRUE;
    ID3D10RasterizerState *rs_default = NULL;
    ID3D10Device_CreateRasterizerState(dev, &rsd, &rs_default);
    ID3D10Device_RSSetState(dev, rs_default);

    D3D10_VIEWPORT vp;
    memset(&vp, 0, sizeof(vp));
    vp.Width = (UINT)g_w;
    vp.Height = (UINT)g_h;
    vp.MaxDepth = 1.0f;
    ID3D10Device_RSSetViewports(dev, 1, &vp);

    HMODULE d3dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!d3dc) d3dc = LoadLibraryA("d3dcompiler_43.dll");
    g_compile = d3dc ? (PFN_D3DCompile)GetProcAddress(d3dc, "D3DCompile") : NULL;
    if (!g_compile) {
        fail_box(
            "Could not load d3dcompiler (D3DCompile) in this container.\n\n"
            "The HLSL shaders for the Direct3D 10 cube can't be compiled.");
        DestroyWindow(hwnd);
        return 1;
    }

    ID3DBlob *vsb = compile_hlsl(kSpinHLSL, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(kSpinHLSL, "PSMain", "ps_4_0");
    if (!vsb || !psb) {
        DestroyWindow(hwnd);
        return 1;
    }
    ID3D10VertexShader *vs = NULL;
    ID3D10PixelShader *ps = NULL;
    ID3D10Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), &vs);
    ID3D10Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), &ps);

    D3D10_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D10InputLayout *layout = NULL;
    ID3D10Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    ColVertex verts[36];
    build_color_cube(verts);
    D3D10_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = sizeof(verts);
    vbd.Usage = D3D10_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    D3D10_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = verts;
    ID3D10Buffer *vbo = NULL;
    ID3D10Device_CreateBuffer(dev, &vbd, &sr, &vbo);

    D3D10_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(Mat4);
    cbd.Usage = D3D10_USAGE_DYNAMIC;
    cbd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    ID3D10Buffer *cbo = NULL;
    ID3D10Device_CreateBuffer(dev, &cbd, NULL, &cbo);

    aio_hud_create(hinst);
    aio_hud_update(hwnd, "Direct3D 10  -  measuring...");

    int bench_on = aio_bench_active();
    LARGE_INTEGER qpf, start, prev;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&start);
    prev = start;
    ULONGLONG last_ms = GetTickCount64();
    uint64_t frames = 0, last_frame = 0;
    const float clear[4] = {0.10f, 0.10f, 0.12f, 1.0f};
    float aspect = (g_h > 0) ? (float)g_w / (float)g_h : 1.0f;

    MSG msg;
    aio_watchdog_start(&frames, 12);
    g_quit = 0;
    while (!g_quit) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = 1; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_quit) break;
        if (g_resize_pending) {  // keep the cube round when the window is resized
            g_resize_pending = 0;
            if (g_resize_h > 0) aspect = (float)g_resize_w / (float)g_resize_h;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / (double)qpf.QuadPart;
        float a = (float)t * 0.6f;
        Mat4 model = mat_mul(mat_rotate(0, 1, 0, a), mat_rotate(1, 0, 0, 0.5f));
        Mat4 mvp = mat_mul(mat_mul(model, mat_translate(0, 0, -6.5f)),
                           mat_perspective(0.6f, aspect, 0.1f, 100.0f));

        void *p = NULL;
        if (SUCCEEDED(ID3D10Buffer_Map(cbo, D3D10_MAP_WRITE_DISCARD, 0, &p)) && p) {
            memcpy(p, mvp.m, sizeof(mvp.m));
            ID3D10Buffer_Unmap(cbo);
        }

        ID3D10Device_ClearRenderTargetView(dev, rtv, clear);
        ID3D10Device_ClearDepthStencilView(dev, dsv, D3D10_CLEAR_DEPTH, 1.0f, 0);

        UINT stride = sizeof(ColVertex), offset = 0;
        ID3D10Device_IASetInputLayout(dev, layout);
        ID3D10Device_IASetVertexBuffers(dev, 0, 1, &vbo, &stride, &offset);
        ID3D10Device_IASetPrimitiveTopology(dev, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D10Device_VSSetShader(dev, vs);
        ID3D10Device_VSSetConstantBuffers(dev, 0, 1, &cbo);
        ID3D10Device_PSSetShader(dev, ps);
        ID3D10Device_Draw(dev, 36, 0);

        IDXGISwapChain_Present(swap, aio_vsync ? 1 : 0, 0);
        frames++;

        if (bench_on) {
            double dt_ms = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)qpf.QuadPart;
            aio_bench_add(dt_ms);
            prev = now;
            if (t >= (double)aio_bench_seconds()) g_quit = 1;
        }

        ULONGLONG now_ms = GetTickCount64();
        if (now_ms - last_ms >= 500) {
            double secs = (double)(now_ms - last_ms) / 1000.0;
            double fps = (secs > 0.0) ? (double)(frames - last_frame) / secs : 0.0;
            char hud[64], title[128];
            snprintf(hud, sizeof(hud), "%s   %.0f FPS", api, fps);
            aio_hud_update(hwnd, hud);
            snprintf(title, sizeof(title), "AIO Graphics Test  -  %s  -  %.0f FPS", api, fps);
            SetWindowTextA(hwnd, title);
            last_ms = now_ms;
            last_frame = frames;
        }
    }

    aio_watchdog_stop();

    if (bench_on) {
        QueryPerformanceCounter(&prev);
        double total = (double)(prev.QuadPart - start.QuadPart) / (double)qpf.QuadPart;
        char *res = aio_bench_finish(api, total);
        if (res) {
            aio_bench_show_result(res);
            free(res);
        }
    }

    aio_hud_destroy();
    if (cbo) ID3D10Buffer_Release(cbo);
    if (vbo) ID3D10Buffer_Release(vbo);
    if (layout) ID3D10InputLayout_Release(layout);
    if (ps) ID3D10PixelShader_Release(ps);
    if (vs) ID3D10VertexShader_Release(vs);
    if (rs_default) ID3D10RasterizerState_Release(rs_default);
    if (dss) ID3D10DepthStencilState_Release(dss);
    if (dsv) ID3D10DepthStencilView_Release(dsv);
    if (depth_tex) ID3D10Texture2D_Release(depth_tex);
    if (rtv) ID3D10RenderTargetView_Release(rtv);
    if (backbuf) ID3D10Texture2D_Release(backbuf);
    if (swap) IDXGISwapChain_Release(swap);
    if (dev) ID3D10Device_Release(dev);
    DestroyWindow(hwnd);
    return 0;
}

// ---------------------------------------------------------------------------
// Embedded offscreen + CPU-readback backend (AioEmbed contract, cube_embed.h).
//
// D3D10 needs NO window: we create a headless HARDWARE device with
// D3D10CreateDevice, render one frame into an offscreen R8G8B8A8 render-target,
// CopyResource it into a STAGING texture, Map that for CPU read and copy the
// rows into g_embed_px. The shell uploads those pixels into its own D3D11
// texture (RGBA8, top-down), exactly like the DX11 path.
// ---------------------------------------------------------------------------

typedef HRESULT(WINAPI *PFN_D3D10CreateDevice)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT,
                                               UINT, ID3D10Device **);

static ID3D10Device *g_dev;
static ID3D10Texture2D *g_rt;
static ID3D10RenderTargetView *g_rtv;
static ID3D10Texture2D *g_depth;
static ID3D10DepthStencilView *g_dsv;
static ID3D10Texture2D *g_stage;
static ID3D10VertexShader *g_vs;
static ID3D10PixelShader *g_ps;
static ID3D10InputLayout *g_layout;
static ID3D10Buffer *g_vbo;
static ID3D10Buffer *g_cbo;
static ID3D10RasterizerState *g_rs;
static ID3D10DepthStencilState *g_dss;

static unsigned char *g_embed_px;
static int g_embed_w, g_embed_h;
static double g_embed_ms;
static LARGE_INTEGER g_embed_freq;

void aio_dx10_embed_cleanup(void) {
    if (g_stage) { ID3D10Texture2D_Release(g_stage); g_stage = NULL; }
    if (g_cbo) { ID3D10Buffer_Release(g_cbo); g_cbo = NULL; }
    if (g_vbo) { ID3D10Buffer_Release(g_vbo); g_vbo = NULL; }
    if (g_layout) { ID3D10InputLayout_Release(g_layout); g_layout = NULL; }
    if (g_ps) { ID3D10PixelShader_Release(g_ps); g_ps = NULL; }
    if (g_vs) { ID3D10VertexShader_Release(g_vs); g_vs = NULL; }
    if (g_rs) { ID3D10RasterizerState_Release(g_rs); g_rs = NULL; }
    if (g_dss) { ID3D10DepthStencilState_Release(g_dss); g_dss = NULL; }
    if (g_dsv) { ID3D10DepthStencilView_Release(g_dsv); g_dsv = NULL; }
    if (g_depth) { ID3D10Texture2D_Release(g_depth); g_depth = NULL; }
    if (g_rtv) { ID3D10RenderTargetView_Release(g_rtv); g_rtv = NULL; }
    if (g_rt) { ID3D10Texture2D_Release(g_rt); g_rt = NULL; }
    if (g_dev) { ID3D10Device_Release(g_dev); g_dev = NULL; }
    if (g_embed_px) { free(g_embed_px); g_embed_px = NULL; }
    g_embed_w = 0;
    g_embed_h = 0;
}

int aio_dx10_embed_init(int w, int h) {
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (g_dev) aio_dx10_embed_cleanup();

    QueryPerformanceFrequency(&g_embed_freq);

    HMODULE d3d10lib = LoadLibraryA("d3d10.dll");
    PFN_D3D10CreateDevice p_create =
        d3d10lib ? (PFN_D3D10CreateDevice)GetProcAddress(d3d10lib, "D3D10CreateDevice") : NULL;
    if (!p_create) { aio_dx10_embed_cleanup(); return 1; }

    HRESULT hr = p_create(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &g_dev);
    if (FAILED(hr) || !g_dev) { aio_dx10_embed_cleanup(); return 1; }

    if (!g_compile) {
        HMODULE d3dc = LoadLibraryA("d3dcompiler_47.dll");
        if (!d3dc) d3dc = LoadLibraryA("d3dcompiler_43.dll");
        g_compile = d3dc ? (PFN_D3DCompile)GetProcAddress(d3dc, "D3DCompile") : NULL;
    }
    if (!g_compile) { aio_dx10_embed_cleanup(); return 1; }

    // Offscreen render-target color texture + view.
    D3D10_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_DEFAULT;
    td.BindFlags = D3D10_BIND_RENDER_TARGET;
    if (FAILED(ID3D10Device_CreateTexture2D(g_dev, &td, NULL, &g_rt)) || !g_rt) {
        aio_dx10_embed_cleanup();
        return 1;
    }
    if (FAILED(ID3D10Device_CreateRenderTargetView(g_dev, (ID3D10Resource *)g_rt, NULL, &g_rtv)) ||
        !g_rtv) {
        aio_dx10_embed_cleanup();
        return 1;
    }

    // Depth-stencil texture + view.
    D3D10_TEXTURE2D_DESC dd;
    memset(&dd, 0, sizeof(dd));
    dd.Width = (UINT)w;
    dd.Height = (UINT)h;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D10_USAGE_DEFAULT;
    dd.BindFlags = D3D10_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D10Device_CreateTexture2D(g_dev, &dd, NULL, &g_depth)) || !g_depth) {
        aio_dx10_embed_cleanup();
        return 1;
    }
    if (FAILED(ID3D10Device_CreateDepthStencilView(g_dev, (ID3D10Resource *)g_depth, NULL,
                                                   &g_dsv)) ||
        !g_dsv) {
        aio_dx10_embed_cleanup();
        return 1;
    }

    // Staging texture for CPU readback (same size/format as the color RT).
    D3D10_TEXTURE2D_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Width = (UINT)w;
    sd.Height = (UINT)h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D10_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    if (FAILED(ID3D10Device_CreateTexture2D(g_dev, &sd, NULL, &g_stage)) || !g_stage) {
        aio_dx10_embed_cleanup();
        return 1;
    }

    // Depth-stencil state (LESS) + rasterizer (CULL_NONE, matches standalone).
    D3D10_DEPTH_STENCIL_DESC dsd;
    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D10_COMPARISON_LESS;
    if (FAILED(ID3D10Device_CreateDepthStencilState(g_dev, &dsd, &g_dss)) || !g_dss) {
        aio_dx10_embed_cleanup();
        return 1;
    }
    ID3D10Device_OMSetDepthStencilState(g_dev, g_dss, 1);

    D3D10_RASTERIZER_DESC rsd;
    memset(&rsd, 0, sizeof(rsd));
    rsd.FillMode = D3D10_FILL_SOLID;
    rsd.CullMode = D3D10_CULL_NONE;
    rsd.DepthClipEnable = TRUE;
    if (FAILED(ID3D10Device_CreateRasterizerState(g_dev, &rsd, &g_rs)) || !g_rs) {
        aio_dx10_embed_cleanup();
        return 1;
    }
    ID3D10Device_RSSetState(g_dev, g_rs);

    // Shaders + input layout.
    ID3DBlob *vsb = compile_hlsl(kSpinHLSL, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(kSpinHLSL, "PSMain", "ps_4_0");
    if (!vsb || !psb) {
        if (vsb) ID3D10Blob_Release(vsb);
        if (psb) ID3D10Blob_Release(psb);
        aio_dx10_embed_cleanup();
        return 1;
    }
    if (FAILED(ID3D10Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb),
                                               ID3D10Blob_GetBufferSize(vsb), &g_vs)) ||
        FAILED(ID3D10Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb),
                                              ID3D10Blob_GetBufferSize(psb), &g_ps))) {
        ID3D10Blob_Release(vsb);
        ID3D10Blob_Release(psb);
        aio_dx10_embed_cleanup();
        return 1;
    }

    D3D10_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0},
    };
    HRESULT lhr = ID3D10Device_CreateInputLayout(g_dev, il, 2, ID3D10Blob_GetBufferPointer(vsb),
                                                 ID3D10Blob_GetBufferSize(vsb), &g_layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    if (FAILED(lhr) || !g_layout) { aio_dx10_embed_cleanup(); return 1; }

    // Vertex buffer (immutable) + constant buffer (dynamic).
    ColVertex verts[36];
    build_color_cube(verts);
    D3D10_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = sizeof(verts);
    vbd.Usage = D3D10_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    D3D10_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = verts;
    if (FAILED(ID3D10Device_CreateBuffer(g_dev, &vbd, &sr, &g_vbo)) || !g_vbo) {
        aio_dx10_embed_cleanup();
        return 1;
    }

    D3D10_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(Mat4);
    cbd.Usage = D3D10_USAGE_DYNAMIC;
    cbd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    if (FAILED(ID3D10Device_CreateBuffer(g_dev, &cbd, NULL, &g_cbo)) || !g_cbo) {
        aio_dx10_embed_cleanup();
        return 1;
    }

    g_embed_px = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
    if (!g_embed_px) { aio_dx10_embed_cleanup(); return 1; }

    g_embed_w = w;
    g_embed_h = h;
    return 0;
}

int aio_dx10_embed_resize(int w, int h) {
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (g_dev && w == g_embed_w && h == g_embed_h) return 0;
    aio_dx10_embed_cleanup();
    return aio_dx10_embed_init(w, h);
}

void aio_dx10_embed_render(double t) {
    if (!g_dev) return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    float aspect = (g_embed_h > 0) ? (float)g_embed_w / (float)g_embed_h : 1.0f;
    float a = (float)t * 0.6f;
    Mat4 model = mat_mul(mat_rotate(0, 1, 0, a), mat_rotate(1, 0, 0, 0.5f));
    Mat4 mvp = mat_mul(mat_mul(model, mat_translate(0, 0, -6.5f)),
                       mat_perspective(0.6f, aspect, 0.1f, 100.0f));

    void *p = NULL;
    if (SUCCEEDED(ID3D10Buffer_Map(g_cbo, D3D10_MAP_WRITE_DISCARD, 0, &p)) && p) {
        memcpy(p, mvp.m, sizeof(mvp.m));
        ID3D10Buffer_Unmap(g_cbo);
    }

    ID3D10Device_OMSetRenderTargets(g_dev, 1, &g_rtv, g_dsv);

    D3D10_VIEWPORT vp;
    memset(&vp, 0, sizeof(vp));
    vp.Width = (UINT)g_embed_w;
    vp.Height = (UINT)g_embed_h;
    vp.MaxDepth = 1.0f;
    ID3D10Device_RSSetViewports(g_dev, 1, &vp);

    const float clear[4] = {8 / 255.f, 12 / 255.f, 17 / 255.f, 1.f};
    ID3D10Device_ClearRenderTargetView(g_dev, g_rtv, clear);
    ID3D10Device_ClearDepthStencilView(g_dev, g_dsv, D3D10_CLEAR_DEPTH, 1.0f, 0);

    UINT stride = sizeof(ColVertex), offset = 0;
    ID3D10Device_IASetInputLayout(g_dev, g_layout);
    ID3D10Device_IASetVertexBuffers(g_dev, 0, 1, &g_vbo, &stride, &offset);
    ID3D10Device_IASetPrimitiveTopology(g_dev, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D10Device_VSSetShader(g_dev, g_vs);
    ID3D10Device_VSSetConstantBuffers(g_dev, 0, 1, &g_cbo);
    ID3D10Device_PSSetShader(g_dev, g_ps);
    ID3D10Device_Draw(g_dev, 36, 0);

    // GPU -> staging -> CPU readback, honoring the staging RowPitch.
    ID3D10Device_CopyResource(g_dev, (ID3D10Resource *)g_stage, (ID3D10Resource *)g_rt);
    D3D10_MAPPED_TEXTURE2D map;
    memset(&map, 0, sizeof(map));
    if (g_embed_px && SUCCEEDED(ID3D10Texture2D_Map(g_stage, 0, D3D10_MAP_READ, 0, &map)) &&
        map.pData) {
        int rowbytes = g_embed_w * 4;
        for (int y = 0; y < g_embed_h; y++)
            memcpy(g_embed_px + (size_t)y * rowbytes,
                   (BYTE *)map.pData + (size_t)y * map.RowPitch, rowbytes);
        ID3D10Texture2D_Unmap(g_stage, 0);
    }

    QueryPerformanceCounter(&t1);
    if (g_embed_freq.QuadPart > 0)
        g_embed_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_embed_freq.QuadPart;
}

int aio_dx10_embed_get_frame(AioEmbedFrame *out) {
    if (!g_embed_px) return 1;
    out->pixels = g_embed_px;
    out->width = g_embed_w;
    out->height = g_embed_h;
    out->pitch = g_embed_w * 4;
    out->fmt = AIO_EMBED_FMT_RGBA8;
    out->flip_y = 0;
    return 0;
}

double aio_dx10_embed_gpu_ms(void) { return g_embed_ms; }
