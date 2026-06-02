// AIO Graphics Test - Direct3D 8 cube backend.
//
// A spinning, multi-colored cube rendered with Direct3D 8 fixed-function (no
// shaders). Under Winlator this exercises the DXVK d3d8 wrapper -> DXVK d3d9 ->
// Vulkan -> Turnip path - thematically the API the original DolphinVS used.
//
// d3d8.dll is loaded dynamically (Direct3DCreate8 via GetProcAddress), so the
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
#include <d3d8.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cube_d3d8.h"
#include "hud.h"
#include "bench.h"
#include "watchdog.h"

static int g_w = 640, g_h = 480;
static int g_quit;
// Pending window resize -> refresh the projection aspect in the loop, so the cube
// isn't stretched when the window is maximized/resized.
static volatile int g_resize_pending = 0;
static int g_resize_w = 0, g_resize_h = 0;

static LRESULT CALLBACK d3d8_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
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

// --- row-major / row-vector matrix math (D3D8 fixed-function uses v * M) ---
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

typedef IDirect3D8 *(WINAPI *PFN_Direct3DCreate8)(UINT);

static void fail_box(const char *msg) {
    MessageBoxA(NULL, msg, "AIO Graphics Test - Direct3D 8", MB_OK | MB_ICONERROR);
}

int aio_run_d3d8_cube(HINSTANCE hinst) {
    const char *api = "Direct3D 8";
    const char *cls = "AIOD3D8Cube";

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = d3d8_wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(hinst, MAKEINTRESOURCEA(1));
    wc.lpszClassName = cls;
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA(cls, "AIO Graphics Test  -  Direct3D 8",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640,
                              480, NULL, NULL, hinst, NULL);
    if (!hwnd) return 1;
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_w = rc.right - rc.left;
    g_h = rc.bottom - rc.top;
    if (g_w <= 0) g_w = 640;
    if (g_h <= 0) g_h = 480;

    HMODULE d3d8lib = LoadLibraryA("d3d8.dll");
    PFN_Direct3DCreate8 p_create =
        d3d8lib ? (PFN_Direct3DCreate8)GetProcAddress(d3d8lib, "Direct3DCreate8") : NULL;
    if (!p_create) {
        fail_box(
            "Direct3D 8 is not available in this container.\n\n"
            "Could not load d3d8.dll (is the DXVK d3d8 wrapper installed?).");
        DestroyWindow(hwnd);
        return 1;
    }
    IDirect3D8 *d3d = p_create(D3D_SDK_VERSION);
    if (!d3d) {
        fail_box("Direct3DCreate8 failed.");
        DestroyWindow(hwnd);
        return 1;
    }

    // Windowed D3D8 needs a back-buffer format matching the desktop.
    D3DDISPLAYMODE mode;
    memset(&mode, 0, sizeof(mode));
    IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = g_w;
    pp.BackBufferHeight = g_h;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.FullScreen_PresentationInterval =
        aio_vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice8 *dev = NULL;
    HRESULT hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev)
        hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev) {
        fail_box(
            "Could not create a Direct3D 8 device.\n\n"
            "This container's GPU/driver doesn't expose D3D8 (DXVK d3d8 wrapper).");
        IDirect3D8_Release(d3d);
        DestroyWindow(hwnd);
        return 1;
    }

    IDirect3DDevice8_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);

    Vertex verts[36];
    build_cube(verts);

    aio_hud_create(hinst);
    aio_hud_update(hwnd, "Direct3D 8  -  measuring...");

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

        IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               D3DCOLOR_XRGB(26, 26, 31), 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice8_BeginScene(dev))) {
            IDirect3DDevice8_SetTransform(dev, D3DTS_WORLD, &world);
            IDirect3DDevice8_SetTransform(dev, D3DTS_VIEW, &view);
            IDirect3DDevice8_SetTransform(dev, D3DTS_PROJECTION, &proj);
            IDirect3DDevice8_SetVertexShader(dev, CUBE_FVF);  // D3D8: FVF via SetVertexShader
            IDirect3DDevice8_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 12, verts, sizeof(Vertex));
            IDirect3DDevice8_EndScene(dev);
        }
        IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL);
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
    IDirect3DDevice8_Release(dev);
    IDirect3D8_Release(d3d);
    DestroyWindow(hwnd);
    return 0;
}
