// AIO Graphics Test - DirectDraw / legacy Direct3D backend.
//
// Exercises the legacy DirectDraw stack that DXVK does NOT implement: under
// Winlator this goes ddraw.dll -> Wine's built-in ddraw -> wined3d -> OpenGL ->
// Zink -> Vulkan, the exact path the old DX5/6/7 games actually use. That makes
// it a genuinely different diagnostic from the DXVK (d3d8/9/10/11) and
// VKD3D (d3d12) cubes.
//
// Implemented variants:
//   "dd7"  - Direct3D 7 immediate-mode cube (IDirect3DDevice7, the clean path)
//   "dd2d" - pure 2D DirectDraw blit test (no Direct3D at all)
// (DX5 / DX6 device-version variants build on this same DirectDraw7 setup.)
//
// ddraw.dll is loaded dynamically (DirectDrawCreateEx via GetProcAddress), so the
// .exe has no static dependency on it and still launches with a graceful notice
// if a container has no working DirectDraw.
//
// Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

#define COBJMACROS
// GUIDs (IID_IDirectDraw7, IID_IDirect3D7, the device IIDs, ...) come from the
// linked dxguid import lib - do NOT define INITGUID here or they'd collide.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600  // GetTickCount64
#endif
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cube_ddraw.h"
#include "cube_embed.h"
#include "hud.h"
#include "bench.h"
#include "watchdog.h"

static int g_w = 640, g_h = 480;
static int g_quit;

static LRESULT CALLBACK ddraw_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                g_quit = 1;
                PostQuitMessage(0);
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

// --- row-major / row-vector matrix math (v' = v * M), the D3D fixed-function
//     convention (same as the d3d9 backend). ---
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
static D3DMATRIX to_d3dmat(Mat4 a) {
    D3DMATRIX d;
    memcpy(&d, a.m, sizeof(a.m));
    return d;
}

// Pre-lit vertex (D3DLVERTEX): carries its own diffuse color so the cube is
// colored with lighting disabled - works identically on Device2/3/7.
static void build_lvert_cube(D3DLVERTEX *out) {
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
            memset(&out[v], 0, sizeof(out[v]));
            out[v].x = f[face][ci][0];
            out[v].y = f[face][ci][1];
            out[v].z = f[face][ci][2];
            out[v].color = col[face];
            out[v].specular = 0xFF000000;
            v++;
        }
}

typedef HRESULT(WINAPI *PFN_DirectDrawCreateEx)(GUID *, LPVOID *, REFIID, IUnknown *);

static void fail_box(const char *msg) {
    MessageBoxA(NULL, msg, "AIO Graphics Test - DirectDraw", MB_OK | MB_ICONERROR);
}

// --- Z-buffer format search (DX7 EnumZBufferFormats). Prefer a depth-only
//     format, preferring 24- then 16- then 32-bit. ---
typedef struct {
    DDPIXELFORMAT pf;
    int found;
    int rank;
} ZSearch;

static int z_rank(const DDPIXELFORMAT *pf) {
    int r = 0;
    if (!(pf->dwFlags & DDPF_STENCILBUFFER)) r += 100;  // prefer no stencil
    switch (pf->dwZBufferBitDepth) {
        case 24: r += 30; break;
        case 16: r += 20; break;
        case 32: r += 10; break;
        default: r += 1; break;
    }
    return r;
}

static HRESULT WINAPI enum_z_cb(DDPIXELFORMAT *pf, void *ctx) {
    ZSearch *z = (ZSearch *)ctx;
    if (pf && (pf->dwFlags & DDPF_ZBUFFER)) {
        int r = z_rank(pf);
        if (!z->found || r > z->rank) {
            z->pf = *pf;
            z->rank = r;
            z->found = 1;
        }
    }
    return D3DENUMRET_OK;
}

// Blit the offscreen render/back buffer to the window's client area on the
// (clipped) primary surface - the windowed-DirectDraw "present".
static void ddraw_present(IDirectDrawSurface7 *primary, IDirectDrawSurface7 *backbuf, HWND hwnd) {
    RECT dst;
    GetClientRect(hwnd, &dst);
    POINT tl = {0, 0};
    ClientToScreen(hwnd, &tl);
    OffsetRect(&dst, tl.x, tl.y);
    RECT src = {0, 0, g_w, g_h};
    IDirectDrawSurface7_Blt(primary, &dst, backbuf, &src, DDBLT_WAIT, NULL);
}

// ============================ Direct3D 7 cube ============================
static int run_dd7(HINSTANCE hinst, HWND hwnd, IDirectDraw7 *dd, IDirectDrawSurface7 *primary,
                   IDirectDrawSurface7 *backbuf, const char *api) {
    IDirect3D7 *d3d = NULL;
    if (FAILED(IDirectDraw7_QueryInterface(dd, &IID_IDirect3D7, (void **)&d3d)) || !d3d) {
        fail_box("Could not obtain IDirect3D7 (no Direct3D in this container's DirectDraw?).");
        return 1;
    }

    // Z-buffer: enumerate a supported format, create + attach to the back buffer.
    ZSearch zs;
    memset(&zs, 0, sizeof(zs));
    IDirect3D7_EnumZBufferFormats(d3d, &IID_IDirect3DTnLHalDevice, enum_z_cb, &zs);
    if (!zs.found)
        IDirect3D7_EnumZBufferFormats(d3d, &IID_IDirect3DHALDevice, enum_z_cb, &zs);

    IDirectDrawSurface7 *zbuf = NULL;
    if (zs.found) {
        DDSURFACEDESC2 zd;
        memset(&zd, 0, sizeof(zd));
        zd.dwSize = sizeof(zd);
        zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        zd.dwWidth = g_w;
        zd.dwHeight = g_h;
        zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
        zd.ddpfPixelFormat = zs.pf;
        if (FAILED(IDirectDraw7_CreateSurface(dd, &zd, &zbuf, NULL))) {
            zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_SYSTEMMEMORY;
            IDirectDraw7_CreateSurface(dd, &zd, &zbuf, NULL);
        }
        if (zbuf) IDirectDrawSurface7_AddAttachedSurface(backbuf, zbuf);
    }

    // Device: try T&L HAL, then HAL, then RGB (software) so it works anywhere.
    IDirect3DDevice7 *dev = NULL;
    static const IID *kDevs[] = {&IID_IDirect3DTnLHalDevice, &IID_IDirect3DHALDevice,
                                 &IID_IDirect3DRGBDevice};
    for (int i = 0; i < 3 && !dev; i++)
        IDirect3D7_CreateDevice(d3d, kDevs[i], backbuf, &dev);
    if (!dev) {
        fail_box(
            "Could not create a Direct3D 7 device.\n\n"
            "This container's DirectDraw/wined3d doesn't expose a usable 3D device.");
        if (zbuf) IDirectDrawSurface7_Release(zbuf);
        IDirect3D7_Release(d3d);
        return 1;
    }

    D3DVIEWPORT7 vp;
    memset(&vp, 0, sizeof(vp));
    vp.dwWidth = g_w;
    vp.dwHeight = g_h;
    vp.dvMinZ = 0.0f;
    vp.dvMaxZ = 1.0f;
    IDirect3DDevice7_SetViewport(dev, &vp);

    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_LIGHTING, FALSE);
    IDirect3DDevice7_SetRenderState(dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);

    D3DLVERTEX verts[36];
    build_lvert_cube(verts);

    aio_hud_create(hinst);
    aio_hud_update(hwnd, "Direct3D 7 (DirectDraw)  -  measuring...");

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

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / (double)qpf.QuadPart;
        float a = (float)t * 0.6f;
        Mat4 model = mat_mul(mat_rotate(0, 1, 0, a), mat_rotate(1, 0, 0, 0.5f));
        D3DMATRIX world = to_d3dmat(model);
        D3DMATRIX view = to_d3dmat(mat_translate(0, 0, -6.5f));
        D3DMATRIX proj = to_d3dmat(mat_perspective(0.6f, aspect, 0.1f, 100.0f));

        IDirect3DDevice7_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x001A1A1F, 1.0f,
                               0);
        if (SUCCEEDED(IDirect3DDevice7_BeginScene(dev))) {
            IDirect3DDevice7_SetTransform(dev, D3DTRANSFORMSTATE_WORLD, &world);
            IDirect3DDevice7_SetTransform(dev, D3DTRANSFORMSTATE_VIEW, &view);
            IDirect3DDevice7_SetTransform(dev, D3DTRANSFORMSTATE_PROJECTION, &proj);
            IDirect3DDevice7_DrawPrimitive(dev, D3DPT_TRIANGLELIST, D3DFVF_LVERTEX, verts, 36, 0);
            IDirect3DDevice7_EndScene(dev);
        }
        ddraw_present(primary, backbuf, hwnd);
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
            char hud[80], title[160];
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
    IDirect3DDevice7_Release(dev);
    if (zbuf) IDirectDrawSurface7_Release(zbuf);
    IDirect3D7_Release(d3d);
    return 0;
}

// ============================ pure 2D DirectDraw ============================
// No Direct3D: animated color bars + a bouncing block, drawn with COLORFILL
// Blts and presented with a clipped surface->surface Blt. Tests the raw ddraw
// 2D blit path (the most legacy path of all).
static int run_dd2d(HINSTANCE hinst, HWND hwnd, IDirectDraw7 *dd, IDirectDrawSurface7 *primary,
                    IDirectDrawSurface7 *backbuf, const char *api) {
    aio_hud_create(hinst);
    aio_hud_update(hwnd, "DirectDraw 2D  -  measuring...");

    int bench_on = aio_bench_active();
    LARGE_INTEGER qpf, start, prev;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&start);
    prev = start;
    ULONGLONG last_ms = GetTickCount64();
    uint64_t frames = 0, last_frame = 0;

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

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / (double)qpf.QuadPart;

        // Clear, then scrolling vertical bars (raw color values - exact hue is
        // format-dependent, the point is that COLORFILL + Blt work at all).
        DDBLTFX fx;
        memset(&fx, 0, sizeof(fx));
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 0;
        IDirectDrawSurface7_Blt(backbuf, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);

        int bars = 16;
        int bw = g_w / bars;
        int scroll = (int)(t * 120.0) % (bw > 0 ? bw : 1);
        for (int i = 0; i < bars; i++) {
            RECT r;
            r.left = i * bw - scroll;
            r.right = r.left + bw - 2;
            r.top = 0;
            r.bottom = g_h;
            if (r.left < 0) r.left = 0;
            if (r.right > g_w) r.right = g_w;
            if (r.right <= r.left) continue;
            fx.dwFillColor = (DWORD)((i * 0x1F1F2F) ^ 0x3399CC) | 0xFF000000;
            IDirectDrawSurface7_Blt(backbuf, &r, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        }
        // Bouncing block.
        int bsz = g_h / 5;
        float px = (sinf((float)t * 1.3f) * 0.5f + 0.5f) * (float)(g_w - bsz);
        float py = (sinf((float)t * 1.9f) * 0.5f + 0.5f) * (float)(g_h - bsz);
        RECT blk = {(LONG)px, (LONG)py, (LONG)px + bsz, (LONG)py + bsz};
        fx.dwFillColor = 0xFFFFFFFF;
        IDirectDrawSurface7_Blt(backbuf, &blk, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);

        ddraw_present(primary, backbuf, hwnd);
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
            char hud[80], title[160];
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
    return 0;
}

int aio_run_ddraw_cube(HINSTANCE hinst, const char *variant) {
    if (!variant) variant = "dd7";
    int is_2d = (strcmp(variant, "dd2d") == 0);
    const char *api = is_2d ? "DirectDraw 2D" : "Direct3D 7 (DirectDraw)";
    const char *cls = "AIODDrawCube";

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = ddraw_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(hinst, MAKEINTRESOURCEA(1));
    wc.lpszClassName = cls;
    RegisterClassA(&wc);

    char wtitle[96];
    snprintf(wtitle, sizeof(wtitle), "AIO Graphics Test  -  %s", api);
    HWND hwnd = CreateWindowA(cls, wtitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                              CW_USEDEFAULT, 640, 480, NULL, NULL, hinst, NULL);
    if (!hwnd) return 1;
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_w = rc.right - rc.left;
    g_h = rc.bottom - rc.top;
    if (g_w <= 0) g_w = 640;
    if (g_h <= 0) g_h = 480;

    HMODULE ddlib = LoadLibraryA("ddraw.dll");
    PFN_DirectDrawCreateEx p_create =
        ddlib ? (PFN_DirectDrawCreateEx)GetProcAddress(ddlib, "DirectDrawCreateEx") : NULL;
    if (!p_create) {
        fail_box(
            "DirectDraw is not available in this container.\n\n"
            "Could not load ddraw.dll / DirectDrawCreateEx.");
        DestroyWindow(hwnd);
        return 1;
    }

    IDirectDraw7 *dd = NULL;
    if (FAILED(p_create(NULL, (void **)&dd, &IID_IDirectDraw7, NULL)) || !dd) {
        fail_box("DirectDrawCreateEx failed.");
        DestroyWindow(hwnd);
        return 1;
    }
    IDirectDraw7_SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL);

    // Primary surface + a clipper bound to our window (so the present Blt is
    // clipped to the client area in windowed mode).
    DDSURFACEDESC2 ddsd;
    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7 *primary = NULL;
    if (FAILED(IDirectDraw7_CreateSurface(dd, &ddsd, &primary, NULL)) || !primary) {
        fail_box("Could not create the DirectDraw primary surface.");
        IDirectDraw7_Release(dd);
        DestroyWindow(hwnd);
        return 1;
    }
    IDirectDrawClipper *clipper = NULL;
    if (SUCCEEDED(IDirectDraw7_CreateClipper(dd, 0, &clipper, NULL)) && clipper) {
        IDirectDrawClipper_SetHWnd(clipper, 0, hwnd);
        IDirectDrawSurface7_SetClipper(primary, clipper);
    }

    // Offscreen back buffer (also a 3D render target for the D3D7 variant).
    DWORD bbcaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
    if (!is_2d) bbcaps |= DDSCAPS_3DDEVICE;
    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    ddsd.dwWidth = g_w;
    ddsd.dwHeight = g_h;
    ddsd.ddsCaps.dwCaps = bbcaps;
    IDirectDrawSurface7 *backbuf = NULL;
    if (FAILED(IDirectDraw7_CreateSurface(dd, &ddsd, &backbuf, NULL)) || !backbuf) {
        // Retry in system memory (no video-memory 3D target available).
        ddsd.ddsCaps.dwCaps = (bbcaps & ~DDSCAPS_VIDEOMEMORY) | DDSCAPS_SYSTEMMEMORY;
        if (FAILED(IDirectDraw7_CreateSurface(dd, &ddsd, &backbuf, NULL)) || !backbuf) {
            fail_box("Could not create a DirectDraw back buffer.");
            if (clipper) IDirectDrawClipper_Release(clipper);
            IDirectDrawSurface7_Release(primary);
            IDirectDraw7_Release(dd);
            DestroyWindow(hwnd);
            return 1;
        }
    }

    int rc_code = is_2d ? run_dd2d(hinst, hwnd, dd, primary, backbuf, api)
                        : run_dd7(hinst, hwnd, dd, primary, backbuf, api);

    IDirectDrawSurface7_Release(backbuf);
    if (clipper) IDirectDrawClipper_Release(clipper);
    IDirectDrawSurface7_Release(primary);
    IDirectDraw7_Release(dd);
    DestroyWindow(hwnd);
    return rc_code;
}

// ==================== Embedded offscreen + readback path ====================
// Renders the Direct3D 7 (DirectDraw) cube to an OFFSCREEN, CPU-lockable
// surface on its own DirectDraw device (no primary surface, no window blit),
// then LOCKs that surface to read back its 32-bit pixels for the ImGui shell.
// See cube_embed.h for the AioEmbed contract. Additive to the standalone runner.

static IDirectDraw7 *g_dd;
static IDirectDrawSurface7 *g_rt;    // offscreen 3D render target (lockable)
static IDirectDrawSurface7 *g_zbuf;  // attached depth buffer
static IDirect3D7 *g_d3d;
static IDirect3DDevice7 *g_dev;
static HWND g_embed_hwnd;            // hidden cooperative-level window
static unsigned char *g_embed_px;    // CPU readback buffer (w*h*4, BGRA)
static int g_embed_w, g_embed_h;
static double g_embed_ms;            // last per-frame render+readback cost
static LARGE_INTEGER g_embed_freq;

int aio_ddraw_embed_init(int w, int h) {
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (g_dd || g_dev || g_embed_px) aio_ddraw_embed_cleanup();

    QueryPerformanceFrequency(&g_embed_freq);

    HINSTANCE hinst = GetModuleHandleA(NULL);
    static int class_registered = 0;
    const char *cls = "AIODDrawEmbed";
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
    if (!g_embed_hwnd) return 1;

    HMODULE ddlib = LoadLibraryA("ddraw.dll");
    PFN_DirectDrawCreateEx p_create =
        ddlib ? (PFN_DirectDrawCreateEx)GetProcAddress(ddlib, "DirectDrawCreateEx") : NULL;
    if (!p_create) {
        DestroyWindow(g_embed_hwnd);
        g_embed_hwnd = NULL;
        return 1;
    }

    if (FAILED(p_create(NULL, (void **)&g_dd, &IID_IDirectDraw7, NULL)) || !g_dd) {
        aio_ddraw_embed_cleanup();
        return 1;
    }
    // DirectDrawCreateEx still needs a cooperative window even in NORMAL mode.
    IDirectDraw7_SetCooperativeLevel(g_dd, g_embed_hwnd, DDSCL_NORMAL);

    // Offscreen 3D render target with an explicit 32-bit RGB (Blue at byte 0)
    // pixel format, in system memory so it is reliably CPU-lockable. Retry in
    // video memory if system-memory creation is refused.
    DDSURFACEDESC2 rd;
    memset(&rd, 0, sizeof(rd));
    rd.dwSize = sizeof(rd);
    rd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    rd.dwWidth = w;
    rd.dwHeight = h;
    rd.ddpfPixelFormat.dwSize = sizeof(rd.ddpfPixelFormat);
    rd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    rd.ddpfPixelFormat.dwRGBBitCount = 32;
    rd.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
    rd.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
    rd.ddpfPixelFormat.dwBBitMask = 0x000000FF;
    rd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY;
    if (FAILED(IDirectDraw7_CreateSurface(g_dd, &rd, &g_rt, NULL)) || !g_rt) {
        g_rt = NULL;
        rd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
        if (FAILED(IDirectDraw7_CreateSurface(g_dd, &rd, &g_rt, NULL)) || !g_rt) {
            g_rt = NULL;
            aio_ddraw_embed_cleanup();
            return 1;
        }
    }

    // IDirect3D7 for enumeration / device creation.
    if (FAILED(IDirectDraw7_QueryInterface(g_dd, &IID_IDirect3D7, (void **)&g_d3d)) || !g_d3d) {
        g_d3d = NULL;
        aio_ddraw_embed_cleanup();
        return 1;
    }

    // Z-buffer: enumerate a supported format, create + attach to the target,
    // preferring system memory to match the (typically system-memory) target.
    ZSearch zs;
    memset(&zs, 0, sizeof(zs));
    IDirect3D7_EnumZBufferFormats(g_d3d, &IID_IDirect3DTnLHalDevice, enum_z_cb, &zs);
    if (!zs.found)
        IDirect3D7_EnumZBufferFormats(g_d3d, &IID_IDirect3DHALDevice, enum_z_cb, &zs);
    if (zs.found) {
        DDSURFACEDESC2 zd;
        memset(&zd, 0, sizeof(zd));
        zd.dwSize = sizeof(zd);
        zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        zd.dwWidth = w;
        zd.dwHeight = h;
        zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_SYSTEMMEMORY;
        zd.ddpfPixelFormat = zs.pf;
        if (FAILED(IDirectDraw7_CreateSurface(g_dd, &zd, &g_zbuf, NULL))) {
            g_zbuf = NULL;
            zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
            IDirectDraw7_CreateSurface(g_dd, &zd, &g_zbuf, NULL);
        }
        if (g_zbuf) IDirectDrawSurface7_AddAttachedSurface(g_rt, g_zbuf);
    }

    // Device: T&L HAL, then HAL, then RGB (software) against the offscreen target.
    static const IID *kDevs[] = {&IID_IDirect3DTnLHalDevice, &IID_IDirect3DHALDevice,
                                 &IID_IDirect3DRGBDevice};
    for (int i = 0; i < 3 && !g_dev; i++)
        IDirect3D7_CreateDevice(g_d3d, kDevs[i], g_rt, &g_dev);
    if (!g_dev) {
        aio_ddraw_embed_cleanup();
        return 1;
    }

    D3DVIEWPORT7 vp;
    memset(&vp, 0, sizeof(vp));
    vp.dwWidth = w;
    vp.dwHeight = h;
    vp.dvMinZ = 0.0f;
    vp.dvMaxZ = 1.0f;
    IDirect3DDevice7_SetViewport(g_dev, &vp);

    IDirect3DDevice7_SetRenderState(g_dev, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice7_SetRenderState(g_dev, D3DRENDERSTATE_LIGHTING, FALSE);
    IDirect3DDevice7_SetRenderState(g_dev, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);

    g_embed_px = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
    if (!g_embed_px) {
        aio_ddraw_embed_cleanup();
        return 1;
    }
    g_embed_w = w;
    g_embed_h = h;
    g_embed_ms = 0.0;
    return 0;
}

int aio_ddraw_embed_resize(int w, int h) {
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (g_dev && w == g_embed_w && h == g_embed_h) return 0;
    aio_ddraw_embed_cleanup();
    return aio_ddraw_embed_init(w, h);
}

void aio_ddraw_embed_render(double t) {
    if (!g_dev) return;

    LARGE_INTEGER qstart, qend;
    QueryPerformanceCounter(&qstart);

    float aspect = (g_embed_h > 0) ? (float)g_embed_w / (float)g_embed_h : 1.0f;
    float a = (float)t * 0.6f;
    Mat4 model = mat_mul(mat_rotate(0, 1, 0, a), mat_rotate(1, 0, 0, 0.5f));
    D3DMATRIX world = to_d3dmat(model);
    D3DMATRIX view = to_d3dmat(mat_translate(0, 0, -6.5f));
    D3DMATRIX proj = to_d3dmat(mat_perspective(0.6f, aspect, 0.1f, 100.0f));

    D3DLVERTEX verts[36];
    build_lvert_cube(verts);

    IDirect3DDevice7_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00080C11, 1.0f, 0);
    if (SUCCEEDED(IDirect3DDevice7_BeginScene(g_dev))) {
        IDirect3DDevice7_SetTransform(g_dev, D3DTRANSFORMSTATE_WORLD, &world);
        IDirect3DDevice7_SetTransform(g_dev, D3DTRANSFORMSTATE_VIEW, &view);
        IDirect3DDevice7_SetTransform(g_dev, D3DTRANSFORMSTATE_PROJECTION, &proj);
        IDirect3DDevice7_DrawPrimitive(g_dev, D3DPT_TRIANGLELIST, D3DFVF_LVERTEX, verts, 36, 0);
        IDirect3DDevice7_EndScene(g_dev);
    }

    // Read back: lock the offscreen target and copy rows honoring the pitch.
    DDSURFACEDESC2 sd;
    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (g_embed_px &&
        SUCCEEDED(IDirectDrawSurface7_Lock(g_rt, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL))) {
        for (int y = 0; y < g_embed_h; y++)
            memcpy(g_embed_px + (size_t)y * g_embed_w * 4,
                   (BYTE *)sd.lpSurface + (size_t)y * sd.lPitch, (size_t)g_embed_w * 4);
        IDirectDrawSurface7_Unlock(g_rt, NULL);
    }

    QueryPerformanceCounter(&qend);
    g_embed_ms = (g_embed_freq.QuadPart > 0)
                     ? (double)(qend.QuadPart - qstart.QuadPart) * 1000.0 / (double)g_embed_freq.QuadPart
                     : 0.0;
}

int aio_ddraw_embed_get_frame(AioEmbedFrame *out) {
    if (!g_embed_px || !out) return 1;
    out->pixels = g_embed_px;
    out->width = g_embed_w;
    out->height = g_embed_h;
    out->pitch = g_embed_w * 4;
    out->fmt = AIO_EMBED_FMT_BGRA8;  // 32-bit RGB masks put Blue at byte 0
    out->flip_y = 0;
    return 0;
}

double aio_ddraw_embed_gpu_ms(void) { return g_embed_ms; }

void aio_ddraw_embed_cleanup(void) {
    if (g_dev) { IDirect3DDevice7_Release(g_dev); g_dev = NULL; }
    if (g_zbuf) { IDirectDrawSurface7_Release(g_zbuf); g_zbuf = NULL; }
    if (g_d3d) { IDirect3D7_Release(g_d3d); g_d3d = NULL; }
    if (g_rt) { IDirectDrawSurface7_Release(g_rt); g_rt = NULL; }
    if (g_dd) { IDirectDraw7_Release(g_dd); g_dd = NULL; }
    if (g_embed_px) { free(g_embed_px); g_embed_px = NULL; }
    if (g_embed_hwnd) { DestroyWindow(g_embed_hwnd); g_embed_hwnd = NULL; }
    g_embed_w = 0;
    g_embed_h = 0;
    g_embed_ms = 0.0;
}
