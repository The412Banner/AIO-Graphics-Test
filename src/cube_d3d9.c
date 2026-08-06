// AIO Graphics Test - Direct3D 9 cube backend.
//
// A spinning, multi-colored cube rendered with Direct3D 9 fixed-function (no
// shaders). Under Winlator this exercises the DXVK d3d9 -> Vulkan -> Turnip
// translation path, the counterpart to the DXVK-d3d11 / VKD3D-d3d12 cubes.
//
// d3d9.dll is loaded dynamically (Direct3DCreate9 via GetProcAddress), so the
// .exe has no static dependency on it and still launches without DXVK, showing
// a graceful notice instead.
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
#include <d3d9.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cube_d3d9.h"
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

static LRESULT CALLBACK d3d9_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
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

// --- row-major / row-vector matrix math (D3D9 fixed-function uses v * M) ---
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
static Mat4 mat_rotate(float ax, float ay, float az, float a) {
    float l = sqrtf(ax * ax + ay * ay + az * az);
    if (l > 0) { ax /= l; ay /= l; az /= l; }
    float c = cosf(a), s = sinf(a), t = 1.0f - c;
    Mat4 r = mat_identity();
    r.m[0] = c + ax * ax * t;       r.m[1] = ax * ay * t + az * s;  r.m[2] = ax * az * t - ay * s;
    r.m[4] = ay * ax * t - az * s;  r.m[5] = c + ay * ay * t;       r.m[6] = ay * az * t + ax * s;
    r.m[8] = az * ax * t + ay * s;  r.m[9] = az * ay * t - ax * s;  r.m[10] = c + az * az * t;
    return r;
}
static Mat4 mat_perspective(float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy * 0.5f);
    Mat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = zn * zf / (zn - zf);
    return r;
}
static D3DMATRIX to_d3d(Mat4 a) {
    D3DMATRIX d;
    memcpy(&d, a.m, sizeof(a.m));
    return d;
}

// FVF vertex: position + diffuse color.
typedef struct {
    float x, y, z;
    DWORD color;
} Vertex;
#define CUBE_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE)

static void build_cube(Vertex *out) {
    static const float f[6][4][3] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
        {{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}},
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
        {{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
    };
    static const DWORD col[6] = {
        0xFFE63333, 0xFF33CC4D, 0xFF4073F2, 0xFFF2CC33, 0xFFD966E6, 0xFF33D9E6,
    };
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    int v = 0;
    for (int face = 0; face < 6; face++)
        for (int k = 0; k < 6; k++) {
            int ci = idx[k];
            out[v].x = f[face][ci][0];
            out[v].y = f[face][ci][1];
            out[v].z = f[face][ci][2];
            out[v].color = col[face];
            v++;
        }
}

typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT);

static void fail_box(const char *msg) {
    MessageBoxA(NULL, msg, "AIO Graphics Test - Direct3D 9", MB_OK | MB_ICONERROR);
}

int aio_run_d3d9_cube(HINSTANCE hinst) {
    const char *api = "Direct3D 9";
    const char *cls = "AIOD3D9Cube";

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = d3d9_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(hinst, MAKEINTRESOURCEA(1));
    wc.lpszClassName = cls;
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(cls, "AIO Graphics Test  -  Direct3D 9",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640,
                              480, NULL, NULL, hinst, NULL);
    if (!hwnd) return 1;
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_w = rc.right - rc.left;
    g_h = rc.bottom - rc.top;
    if (g_w <= 0) g_w = 640;
    if (g_h <= 0) g_h = 480;

    HMODULE d3d9lib = LoadLibraryA("d3d9.dll");
    PFN_Direct3DCreate9 p_create =
        d3d9lib ? (PFN_Direct3DCreate9)GetProcAddress(d3d9lib, "Direct3DCreate9") : NULL;
    if (!p_create) {
        fail_box(
            "Direct3D 9 is not available in this container.\n\n"
            "Could not load d3d9.dll (is DXVK installed?).");
        DestroyWindow(hwnd);
        return 1;
    }
    IDirect3D9 *d3d = p_create(D3D_SDK_VERSION);
    if (!d3d) {
        fail_box("Direct3DCreate9 failed.");
        DestroyWindow(hwnd);
        return 1;
    }

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = g_w;
    pp.BackBufferHeight = g_h;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval =
        aio_vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev)
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev) {
        fail_box(
            "Could not create a Direct3D 9 device.\n\n"
            "This container's GPU/driver doesn't expose D3D9 (DXVK).");
        IDirect3D9_Release(d3d);
        DestroyWindow(hwnd);
        return 1;
    }

    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

    Vertex verts[36];
    build_cube(verts);

    aio_hud_create(hinst);
    aio_hud_update(hwnd, "Direct3D 9  -  measuring...");

    int bench_on = aio_bench_active();
    LARGE_INTEGER qpf, start, prev;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&start);
    prev = start;
    ULONGLONG last_ms = GetTickCount64();
    uint64_t frames = 0, last_frame = 0;
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
        D3DMATRIX world = to_d3d(model);
        D3DMATRIX view = to_d3d(mat_translate(0, 0, -6.5f));
        D3DMATRIX proj = to_d3d(mat_perspective(0.6f, aspect, 0.1f, 100.0f));

        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               D3DCOLOR_XRGB(26, 26, 31), 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice9_BeginScene(dev))) {
            IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &world);
            IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &view);
            IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &proj);
            IDirect3DDevice9_SetFVF(dev, CUBE_FVF);
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 12, verts, sizeof(Vertex));
            IDirect3DDevice9_EndScene(dev);
        }
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
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
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(hwnd);
    return 0;
}

// ---------------------------------------------------------------------------
// Embedded offscreen + readback path (AioEmbed contract, see cube_embed.h).
//
// Renders one D3D9 frame to an OFFSCREEN render target on a private device that
// owns a HIDDEN popup window (never shown), reads the pixels back through a
// SYSTEMMEM plain surface, and publishes them to the ImGui shell. No MessageBox,
// no ShowWindow, d3d9.dll loaded dynamically. Reuses this file's static helpers
// (build_cube, Mat4 math, to_d3d, Vertex, CUBE_FVF, PFN_Direct3DCreate9).
// ---------------------------------------------------------------------------

static IDirect3D9        *g_embed_d3d = NULL;
static IDirect3DDevice9  *g_embed_dev = NULL;
static IDirect3DSurface9 *g_embed_rt = NULL;   // offscreen render target (lockable=FALSE)
static IDirect3DSurface9 *g_embed_ds = NULL;   // matching depth-stencil surface
static IDirect3DSurface9 *g_embed_sys = NULL;  // SYSTEMMEM readback surface
static unsigned char     *g_embed_px = NULL;   // CPU pixel buffer, w*h*4 bytes
static int                g_embed_w = 0, g_embed_h = 0;
static HWND               g_embed_hwnd = NULL; // hidden device window
static HMODULE            g_embed_lib = NULL;  // dynamically loaded d3d9.dll
static double             g_embed_ms = 0.0;

int aio_dx9_embed_init(int w, int h) {
    HINSTANCE hinst;
    const char *cls = "AIOD3D9Embed";
    static int class_registered = 0;
    PFN_Direct3DCreate9 p_create;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;

    if (g_embed_dev || g_embed_px || g_embed_hwnd) aio_dx9_embed_cleanup();

    if (w < 8) w = 8;
    if (h < 8) h = 8;

    hinst = GetModuleHandleA(NULL);

    if (!class_registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = hinst;
        wc.lpszClassName = cls;
        RegisterClassA(&wc);
        class_registered = 1;
    }

    g_embed_hwnd = CreateWindowA(cls, "", WS_POPUP, 0, 0, w, h, NULL, NULL, hinst, NULL);
    if (!g_embed_hwnd) { aio_dx9_embed_cleanup(); return 1; }

    g_embed_lib = LoadLibraryA("d3d9.dll");
    p_create = g_embed_lib
                   ? (PFN_Direct3DCreate9)GetProcAddress(g_embed_lib, "Direct3DCreate9")
                   : NULL;
    if (!p_create) { aio_dx9_embed_cleanup(); return 1; }

    g_embed_d3d = p_create(D3D_SDK_VERSION);
    if (!g_embed_d3d) { aio_dx9_embed_cleanup(); return 1; }

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow = g_embed_hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    hr = IDirect3D9_CreateDevice(g_embed_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_embed_hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_embed_dev);
    if (FAILED(hr) || !g_embed_dev)
        hr = IDirect3D9_CreateDevice(g_embed_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_embed_hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_embed_dev);
    if (FAILED(hr) || !g_embed_dev) { aio_dx9_embed_cleanup(); return 1; }

    IDirect3DDevice9_SetRenderState(g_embed_dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(g_embed_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(g_embed_dev, D3DRS_CULLMODE, D3DCULL_NONE);

    hr = IDirect3DDevice9_CreateRenderTarget(g_embed_dev, w, h, D3DFMT_X8R8G8B8,
                                             D3DMULTISAMPLE_NONE, 0, FALSE, &g_embed_rt, NULL);
    if (FAILED(hr) || !g_embed_rt) { aio_dx9_embed_cleanup(); return 1; }

    hr = IDirect3DDevice9_CreateDepthStencilSurface(g_embed_dev, w, h, D3DFMT_D24S8,
                                                    D3DMULTISAMPLE_NONE, 0, TRUE, &g_embed_ds, NULL);
    if (FAILED(hr) || !g_embed_ds) { aio_dx9_embed_cleanup(); return 1; }

    if (FAILED(IDirect3DDevice9_SetRenderTarget(g_embed_dev, 0, g_embed_rt))) {
        aio_dx9_embed_cleanup();
        return 1;
    }
    if (FAILED(IDirect3DDevice9_SetDepthStencilSurface(g_embed_dev, g_embed_ds))) {
        aio_dx9_embed_cleanup();
        return 1;
    }

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(g_embed_dev, w, h, D3DFMT_X8R8G8B8,
                                                      D3DPOOL_SYSTEMMEM, &g_embed_sys, NULL);
    if (FAILED(hr) || !g_embed_sys) { aio_dx9_embed_cleanup(); return 1; }

    g_embed_px = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
    if (!g_embed_px) { aio_dx9_embed_cleanup(); return 1; }

    g_embed_w = w;
    g_embed_h = h;
    return 0;
}

int aio_dx9_embed_resize(int w, int h) {
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (w == g_embed_w && h == g_embed_h) return 0;
    aio_dx9_embed_cleanup();
    return aio_dx9_embed_init(w, h);
}

void aio_dx9_embed_render(double t) {
    static LARGE_INTEGER qpf;
    static int qpf_init = 0;
    LARGE_INTEGER t0, t1;
    float aspect, a;
    Mat4 model;
    D3DMATRIX world, view, proj;
    Vertex verts[36];
    D3DVIEWPORT9 vp;

    if (!g_embed_dev) return;

    if (!qpf_init) { QueryPerformanceFrequency(&qpf); qpf_init = 1; }
    QueryPerformanceCounter(&t0);

    aspect = (g_embed_h > 0) ? (float)g_embed_w / (float)g_embed_h : 1.0f;

    a = (float)t * 0.6f;
    model = mat_mul(mat_rotate(0, 1, 0, a), mat_rotate(1, 0, 0, 0.5f));
    world = to_d3d(model);
    view = to_d3d(mat_translate(0, 0, -6.5f));
    proj = to_d3d(mat_perspective(0.6f, aspect, 0.1f, 100.0f));

    build_cube(verts);

    vp.X = 0;
    vp.Y = 0;
    vp.Width = (DWORD)g_embed_w;
    vp.Height = (DWORD)g_embed_h;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    IDirect3DDevice9_SetViewport(g_embed_dev, &vp);

    IDirect3DDevice9_Clear(g_embed_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           D3DCOLOR_XRGB((int)(aio_embed_clear_rgb[0] * 255.0f + 0.5f),
                                         (int)(aio_embed_clear_rgb[1] * 255.0f + 0.5f),
                                         (int)(aio_embed_clear_rgb[2] * 255.0f + 0.5f)),
                           1.0f, 0);
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_embed_dev))) {
        IDirect3DDevice9_SetTransform(g_embed_dev, D3DTS_WORLD, &world);
        IDirect3DDevice9_SetTransform(g_embed_dev, D3DTS_VIEW, &view);
        IDirect3DDevice9_SetTransform(g_embed_dev, D3DTS_PROJECTION, &proj);
        IDirect3DDevice9_SetFVF(g_embed_dev, CUBE_FVF);
        IDirect3DDevice9_DrawPrimitiveUP(g_embed_dev, D3DPT_TRIANGLELIST, 12, verts,
                                         sizeof(Vertex));
        IDirect3DDevice9_EndScene(g_embed_dev);
    }

    if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_embed_dev, g_embed_rt, g_embed_sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(g_embed_sys, &lr, NULL, D3DLOCK_READONLY))) {
            int y;
            for (y = 0; y < g_embed_h; y++)
                memcpy(g_embed_px + (size_t)y * g_embed_w * 4,
                       (BYTE *)lr.pBits + (size_t)y * lr.Pitch, (size_t)g_embed_w * 4);
            IDirect3DSurface9_UnlockRect(g_embed_sys);
        }
    }

    QueryPerformanceCounter(&t1);
    g_embed_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qpf.QuadPart;
}

int aio_dx9_embed_get_frame(AioEmbedFrame *out) {
    if (!g_embed_px) return 1;
    out->pixels = g_embed_px;
    out->width = g_embed_w;
    out->height = g_embed_h;
    out->pitch = g_embed_w * 4;
    out->fmt = AIO_EMBED_FMT_BGRA8;
    out->flip_y = 0;
    return 0;
}

double aio_dx9_embed_gpu_ms(void) {
    return g_embed_ms;
}

void aio_dx9_embed_cleanup(void) {
    if (g_embed_sys) { IDirect3DSurface9_Release(g_embed_sys); g_embed_sys = NULL; }
    if (g_embed_ds)  { IDirect3DSurface9_Release(g_embed_ds);  g_embed_ds = NULL; }
    if (g_embed_rt)  { IDirect3DSurface9_Release(g_embed_rt);  g_embed_rt = NULL; }
    if (g_embed_dev) { IDirect3DDevice9_Release(g_embed_dev);  g_embed_dev = NULL; }
    if (g_embed_d3d) { IDirect3D9_Release(g_embed_d3d);        g_embed_d3d = NULL; }
    if (g_embed_px)  { free(g_embed_px);                       g_embed_px = NULL; }
    if (g_embed_lib) { FreeLibrary(g_embed_lib);              g_embed_lib = NULL; }
    if (g_embed_hwnd) { DestroyWindow(g_embed_hwnd);           g_embed_hwnd = NULL; }
    g_embed_w = 0;
    g_embed_h = 0;
    g_embed_ms = 0.0;
}
