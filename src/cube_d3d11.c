// AIO Graphics Test - Direct3D 11 test suite.
//
// A set of Direct3D 11 scenes that each exercise a different part of the DXVK
// (d3d11.dll -> Vulkan -> Turnip) path under Winlator:
//   spin       - baseline spinning colored cube (pipeline smoke test)
//   textured   - cube with a generated texture (texture upload + SRV + sampler)
//   instanced  - hundreds of cubes via instanced draw (throughput / benchmark)
//   tess       - tessellated shape (hull/domain shaders)         [stage 2]
//   compute    - compute-shader particle sim                     [stage 3]
//
// Shared scaffolding (window, device, swapchain, depth buffer, HUD overlay,
// benchmark loop) lives in the runner; each scene only provides its shaders,
// resources, and per-frame draw via a small D3D11Scene interface.
//
// HLSL is compiled at runtime via d3dcompiler_47.dll, and d3d11.dll is loaded
// dynamically, so the .exe has NO static dependency on either - it launches
// fine without DXVK and shows a graceful notice instead.
//
// Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cube_d3d11.h"
#include "dolphin_assets.h"  // embedded DolphinVS mesh/texture/caustic data
#include "hud.h"
#include "bench.h"
#include "watchdog.h"

static int g_w = 640, g_h = 480;
static int g_quit;
// Pending swapchain resize (set from WM_SIZE, applied in the render loop where the
// swapchain/RTV/DSV are in scope). Otherwise maximizing stretches a fixed-size
// image, squashing the aspect ratio (e.g. the Space planet goes oval).
static volatile int g_resize_pending = 0;
static int g_resize_w = 0, g_resize_h = 0;

static LRESULT CALLBACK d3d11_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
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
                if (nw > 0 && nh > 0) {
                    g_resize_w = nw;
                    g_resize_h = nh;
                    g_resize_pending = 1;
                }
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

// --- Minimal row-major / row-vector matrix math (v' = v * M), matching the D3D
//     convention used by the row_major cbuffers in the shaders below. ---
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

// Row-vector rotation about a unit axis (x,y,z).
static Mat4 mat_rotate(float ax, float ay, float az, float angle_rad) {
    float len = sqrtf(ax * ax + ay * ay + az * az);
    if (len > 0.0f) {
        ax /= len;
        ay /= len;
        az /= len;
    }
    float c = cosf(angle_rad), s = sinf(angle_rad), t = 1.0f - c;
    Mat4 r = mat_identity();
    r.m[0] = c + ax * ax * t;
    r.m[1] = ax * ay * t + az * s;
    r.m[2] = ax * az * t - ay * s;
    r.m[4] = ay * ax * t - az * s;
    r.m[5] = c + ay * ay * t;
    r.m[6] = ay * az * t + ax * s;
    r.m[8] = az * ax * t + ay * s;
    r.m[9] = az * ay * t - ax * s;
    r.m[10] = c + az * az * t;
    return r;
}

static Mat4 mat_scale(float s) {
    Mat4 r = mat_identity();
    r.m[0] = r.m[5] = r.m[10] = s;
    return r;
}

// Right-handed look-at (row-vector world->view), matching the perspective below.
static Mat4 mat_lookat(float ex, float ey, float ez, float ax, float ay, float az, float ux,
                       float uy, float uz) {
    float zx = ex - ax, zy = ey - ay, zz = ez - az;
    float zl = sqrtf(zx * zx + zy * zy + zz * zz);
    zx /= zl; zy /= zl; zz /= zl;
    float xx = uy * zz - uz * zy, xy = uz * zx - ux * zz, xz = ux * zy - uy * zx;
    float xl = sqrtf(xx * xx + xy * xy + xz * xz);
    xx /= xl; xy /= xl; xz /= xl;
    float yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    Mat4 r = mat_identity();
    r.m[0] = xx; r.m[1] = yx; r.m[2] = zx;
    r.m[4] = xy; r.m[5] = yy; r.m[6] = zy;
    r.m[8] = xz; r.m[9] = yz; r.m[10] = zz;
    r.m[12] = -(xx * ex + xy * ey + xz * ez);
    r.m[13] = -(yx * ex + yy * ey + yz * ez);
    r.m[14] = -(zx * ex + zy * ey + zz * ez);
    return r;
}

// Right-handed perspective with clip-space z in [0,1] (Direct3D convention).
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

// --- Shared shader-compile helper (dynamic d3dcompiler) ---
typedef HRESULT(WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                        ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
                                        ID3DBlob **);

// D3D11CreateDeviceAndSwapChain is loaded dynamically (NOT statically linked) so
// the whole .exe has no hard import dependency on d3d11.dll / dxgi.dll.
typedef HRESULT(WINAPI *PFN_D3D11CreateDeviceAndSwapChain)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);

static PFN_D3DCompile g_compile;

static void fail_box(const char *msg) {
    MessageBoxA(NULL, msg, "AIO Graphics Test - Direct3D 11", MB_OK | MB_ICONERROR);
}

// Compiles one HLSL entry point to a blob. Returns NULL (and shows a box) on error.
static ID3DBlob *compile_hlsl(const char *src, const char *entry, const char *target) {
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = g_compile(src, strlen(src), "scene.hlsl", NULL, NULL, entry, target, 0, 0, &blob,
                           &err);
    if (FAILED(hr)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Shader '%s' (%s) failed to compile.\n\n%s", entry, target,
                 err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        fail_box(msg);
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return blob;
}

// --- Scene interface ---
// init: build resources (return 0 on success). frame: update + issue draws for
// one frame (RTV+DSV already bound and cleared). cleanup: release resources.
typedef struct {
    const char *name;   // selector ("spin", ...)
    const char *label;  // HUD/title label ("D3D11 Cube", ...)
    int (*init)(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h);
    void (*frame)(ID3D11DeviceContext *ctx, double t, float aspect);
    void (*cleanup)(void);
} D3D11Scene;

// ============================ shared cube geometry ============================
typedef struct {
    float pos[3];
    float col[3];
} ColVertex;

static const float kFace[6][4][3] = {
    {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},      // front
    {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},  // back
    {{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}},      // top
    {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},  // bottom
    {{1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},      // right
    {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},  // left
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

// Position + normal vertex + procedural mesh builders, shared by the lit scenes
// (cel shading, matcap). Both produce a flat (non-indexed) triangle list.
typedef struct {
    float pos[3];
    float nrm[3];
} PNVertex;

// UV sphere of radius 1 (normal == position). Returns vertex count = rings*sectors*6.
static int build_sphere_pn(PNVertex *out, int rings, int sectors) {
    int n = 0;
    for (int i = 0; i < rings; i++) {
        float t0 = (float)i / rings * 3.14159265f, t1 = (float)(i + 1) / rings * 3.14159265f;
        for (int j = 0; j < sectors; j++) {
            float p0 = (float)j / sectors * 6.2831853f, p1 = (float)(j + 1) / sectors * 6.2831853f;
#define AIO_SPN(t, p, dst)                                                  \
    do {                                                                    \
        float st = sinf(t), ct = cosf(t), cp = cosf(p), sp = sinf(p);       \
        (dst).pos[0] = st * cp; (dst).pos[1] = ct; (dst).pos[2] = st * sp;  \
        (dst).nrm[0] = (dst).pos[0]; (dst).nrm[1] = (dst).pos[1]; (dst).nrm[2] = (dst).pos[2]; \
    } while (0)
            PNVertex a, b, c, d;
            AIO_SPN(t0, p0, a);
            AIO_SPN(t1, p0, b);
            AIO_SPN(t1, p1, c);
            AIO_SPN(t0, p1, d);
#undef AIO_SPN
            out[n++] = a; out[n++] = b; out[n++] = c;
            out[n++] = a; out[n++] = c; out[n++] = d;
        }
    }
    return n;
}

// Torus with analytic normals. Returns vertex count = nring*nside*6.
static int build_torus_pn(PNVertex *out, int nring, int nside, float R, float r) {
    int n = 0;
    for (int i = 0; i < nring; i++) {
        float u0 = (float)i / nring * 6.2831853f, u1 = (float)(i + 1) / nring * 6.2831853f;
        for (int j = 0; j < nside; j++) {
            float v0 = (float)j / nside * 6.2831853f, v1 = (float)(j + 1) / nside * 6.2831853f;
#define AIO_TPN(u, v, dst)                                                       \
    do {                                                                         \
        float cu = cosf(u), su = sinf(u), cv = cosf(v), sv = sinf(v);            \
        (dst).pos[0] = (R + r * cv) * cu; (dst).pos[1] = (R + r * cv) * su;      \
        (dst).pos[2] = r * sv;                                                   \
        (dst).nrm[0] = cv * cu; (dst).nrm[1] = cv * su; (dst).nrm[2] = sv;       \
    } while (0)
            PNVertex a, b, c, d;
            AIO_TPN(u0, v0, a);
            AIO_TPN(u1, v0, b);
            AIO_TPN(u1, v1, c);
            AIO_TPN(u0, v1, d);
#undef AIO_TPN
            out[n++] = a; out[n++] = b; out[n++] = c;
            out[n++] = a; out[n++] = c; out[n++] = d;
        }
    }
    return n;
}

// ================================ SPIN scene =================================
static const char *kSpinHLSL =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; };\n"
    "struct VSIn { float3 pos : POSITION; float3 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float3 col : COLOR; };\n"
    "VSOut VSMain(VSIn i) { VSOut o; o.pos = mul(float4(i.pos,1.0), mvp); o.col = i.col; return o; }\n"
    "float4 PSMain(VSOut i) : SV_TARGET { return float4(i.col, 1.0); }\n";

static struct {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vbo, *cbo;
} g_spin;

static int spin_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *vsb = compile_hlsl(kSpinHLSL, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(kSpinHLSL, "PSMain", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_spin.vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_spin.ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &g_spin.layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    ColVertex verts[36];
    build_color_cube(verts);
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = sizeof(verts);
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = verts;
    ID3D11Device_CreateBuffer(dev, &vbd, &sr, &g_spin.vbo);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(Mat4);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_spin.cbo);
    return 0;
}

static void upload_mat(ID3D11DeviceContext *ctx, ID3D11Buffer *cbo, Mat4 m) {
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cbo, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                          &map))) {
        memcpy(map.pData, m.m, sizeof(m.m));
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cbo, 0);
    }
}

static void spin_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    // Turntable: fixed forward tilt + slow spin about the vertical axis, narrow
    // FOV + camera pulled back -> reads as the classic 3-faces-visible cube.
    float a = (float)t * 0.6f;
    Mat4 model = mat_mul(mat_rotate(0, 1, 0, a), mat_rotate(1, 0, 0, 0.5f));
    Mat4 mvp = mat_mul(mat_mul(model, mat_translate(0, 0, -6.5f)),
                       mat_perspective(0.6f, aspect, 0.1f, 100.0f));
    upload_mat(ctx, g_spin.cbo, mvp);

    UINT stride = sizeof(ColVertex), offset = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, g_spin.layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_spin.vbo, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_spin.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_spin.cbo);
    ID3D11DeviceContext_PSSetShader(ctx, g_spin.ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 36, 0);
}

static void spin_cleanup(void) {
    if (g_spin.cbo) ID3D11Buffer_Release(g_spin.cbo);
    if (g_spin.vbo) ID3D11Buffer_Release(g_spin.vbo);
    if (g_spin.layout) ID3D11InputLayout_Release(g_spin.layout);
    if (g_spin.ps) ID3D11PixelShader_Release(g_spin.ps);
    if (g_spin.vs) ID3D11VertexShader_Release(g_spin.vs);
    memset(&g_spin, 0, sizeof(g_spin));
}

// ============================== TEXTURED scene ==============================
static const char *kTexHLSL =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; };\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };\n"
    "VSOut VSMain(VSIn i) { VSOut o; o.pos = mul(float4(i.pos,1.0), mvp); o.uv = i.uv; return o; }\n"
    "float4 PSMain(VSOut i) : SV_TARGET { return tex.Sample(smp, i.uv); }\n";

typedef struct {
    float pos[3];
    float uv[2];
} TexVertex;

static struct {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vbo, *cbo;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState *smp;
} g_tex;

static void build_tex_cube(TexVertex *out) {
    static const float uvq[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    int v = 0;
    for (int face = 0; face < 6; face++)
        for (int k = 0; k < 6; k++) {
            int ci = kQuadIdx[k];
            out[v].pos[0] = kFace[face][ci][0];
            out[v].pos[1] = kFace[face][ci][1];
            out[v].pos[2] = kFace[face][ci][2];
            out[v].uv[0] = uvq[ci][0];
            out[v].uv[1] = uvq[ci][1];
            v++;
        }
}

static int tex_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *vsb = compile_hlsl(kTexHLSL, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(kTexHLSL, "PSMain", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_tex.vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_tex.ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &g_tex.layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    TexVertex verts[36];
    build_tex_cube(verts);
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = sizeof(verts);
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = verts;
    ID3D11Device_CreateBuffer(dev, &vbd, &sr, &g_tex.vbo);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(Mat4);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_tex.cbo);

    // Generate a 256x256 checker + gradient RGBA texture.
    const int TS = 256;
    uint32_t *pix = (uint32_t *)malloc((size_t)TS * TS * 4);
    if (!pix) return 1;
    for (int y = 0; y < TS; y++)
        for (int x = 0; x < TS; x++) {
            int chk = ((x >> 5) ^ (y >> 5)) & 1;
            uint8_t r = (uint8_t)(chk ? 230 : 40 + x / 2);
            uint8_t g = (uint8_t)(chk ? 80 + y / 2 : 200);
            uint8_t b = (uint8_t)(chk ? 60 : 120 + (x ^ y) / 4);
            pix[y * TS + x] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        }
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width = TS;
    td.Height = TS;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA tsr;
    memset(&tsr, 0, sizeof(tsr));
    tsr.pSysMem = pix;
    tsr.SysMemPitch = TS * 4;
    ID3D11Device_CreateTexture2D(dev, &td, &tsr, &g_tex.tex);
    free(pix);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_tex.tex, NULL, &g_tex.srv);

    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &sd, &g_tex.smp);
    return (g_tex.tex && g_tex.srv && g_tex.smp) ? 0 : 1;
}

static void tex_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    // Turntable view (see spin_frame) so the textured cube reads clearly as a cube.
    float a = (float)t * 0.6f;
    Mat4 model = mat_mul(mat_rotate(0, 1, 0, a), mat_rotate(1, 0, 0, 0.5f));
    Mat4 mvp = mat_mul(mat_mul(model, mat_translate(0, 0, -6.5f)),
                       mat_perspective(0.6f, aspect, 0.1f, 100.0f));
    upload_mat(ctx, g_tex.cbo, mvp);

    UINT stride = sizeof(TexVertex), offset = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, g_tex.layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_tex.vbo, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_tex.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_tex.cbo);
    ID3D11DeviceContext_PSSetShader(ctx, g_tex.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_tex.srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_tex.smp);
    ID3D11DeviceContext_Draw(ctx, 36, 0);
}

static void tex_cleanup(void) {
    if (g_tex.smp) ID3D11SamplerState_Release(g_tex.smp);
    if (g_tex.srv) ID3D11ShaderResourceView_Release(g_tex.srv);
    if (g_tex.tex) ID3D11Texture2D_Release(g_tex.tex);
    if (g_tex.cbo) ID3D11Buffer_Release(g_tex.cbo);
    if (g_tex.vbo) ID3D11Buffer_Release(g_tex.vbo);
    if (g_tex.layout) ID3D11InputLayout_Release(g_tex.layout);
    if (g_tex.ps) ID3D11PixelShader_Release(g_tex.ps);
    if (g_tex.vs) ID3D11VertexShader_Release(g_tex.vs);
    memset(&g_tex, 0, sizeof(g_tex));
}

// ============================== INSTANCED scene =============================
// A grid of cubes drawn with one instanced draw call (per-instance offset in a
// second vertex buffer). Good throughput / benchmark load.
#define INST_AXIS 8
#define INST_COUNT (INST_AXIS * INST_AXIS * INST_AXIS)

static const char *kInstHLSL =
    "cbuffer CB : register(b0) { row_major float4x4 viewproj; };\n"
    "struct VSIn { float3 pos : POSITION; float3 col : COLOR; float3 inst : INSTOFF; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float3 col : COLOR; };\n"
    "VSOut VSMain(VSIn i) {\n"
    "  VSOut o; float3 world = i.pos * 0.45 + i.inst;\n"
    "  o.pos = mul(float4(world,1.0), viewproj); o.col = i.col; return o; }\n"
    "float4 PSMain(VSOut i) : SV_TARGET { return float4(i.col, 1.0); }\n";

static struct {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vbo, *inst, *cbo;
} g_inst;

static int inst_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *vsb = compile_hlsl(kInstHLSL, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(kInstHLSL, "PSMain", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_inst.vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_inst.ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INSTOFF", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    ID3D11Device_CreateInputLayout(dev, il, 3, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &g_inst.layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    ColVertex verts[36];
    build_color_cube(verts);
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = sizeof(verts);
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = verts;
    ID3D11Device_CreateBuffer(dev, &vbd, &sr, &g_inst.vbo);

    // Per-instance offsets: centered grid.
    float(*off)[3] = (float(*)[3])malloc(sizeof(float) * 3 * INST_COUNT);
    if (!off) return 1;
    int n = 0;
    float span = (INST_AXIS - 1) * 1.5f;
    for (int z = 0; z < INST_AXIS; z++)
        for (int y = 0; y < INST_AXIS; y++)
            for (int x = 0; x < INST_AXIS; x++) {
                off[n][0] = x * 1.5f - span * 0.5f;
                off[n][1] = y * 1.5f - span * 0.5f;
                off[n][2] = z * 1.5f - span * 0.5f;
                n++;
            }
    D3D11_BUFFER_DESC ibd;
    memset(&ibd, 0, sizeof(ibd));
    ibd.ByteWidth = (UINT)(sizeof(float) * 3 * INST_COUNT);
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isr;
    memset(&isr, 0, sizeof(isr));
    isr.pSysMem = off;
    ID3D11Device_CreateBuffer(dev, &ibd, &isr, &g_inst.inst);
    free(off);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(Mat4);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_inst.cbo);
    return 0;
}

static void inst_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    float a = (float)t * 0.3f;
    // Pull the camera back far enough to see the whole grid, and spin it.
    Mat4 world = mat_mul(mat_rotate(0, 1, 0, a), mat_rotate(1, 0, 0, a * 0.4f));
    Mat4 vp = mat_mul(mat_mul(world, mat_translate(0, 0, -28)),
                      mat_perspective(0.7854f, aspect, 0.1f, 200.0f));
    upload_mat(ctx, g_inst.cbo, vp);

    UINT strides[2] = {sizeof(ColVertex), sizeof(float) * 3};
    UINT offsets[2] = {0, 0};
    ID3D11Buffer *bufs[2] = {g_inst.vbo, g_inst.inst};
    ID3D11DeviceContext_IASetInputLayout(ctx, g_inst.layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 2, bufs, strides, offsets);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_inst.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_inst.cbo);
    ID3D11DeviceContext_PSSetShader(ctx, g_inst.ps, NULL, 0);
    ID3D11DeviceContext_DrawInstanced(ctx, 36, INST_COUNT, 0, 0);
}

static void inst_cleanup(void) {
    if (g_inst.cbo) ID3D11Buffer_Release(g_inst.cbo);
    if (g_inst.inst) ID3D11Buffer_Release(g_inst.inst);
    if (g_inst.vbo) ID3D11Buffer_Release(g_inst.vbo);
    if (g_inst.layout) ID3D11InputLayout_Release(g_inst.layout);
    if (g_inst.ps) ID3D11PixelShader_Release(g_inst.ps);
    if (g_inst.vs) ID3D11VertexShader_Release(g_inst.vs);
    memset(&g_inst, 0, sizeof(g_inst));
}

// ============================= TESSELLATION scene ===========================
// An icosahedron whose 20 triangle patches are tessellated (hull/domain shaders,
// SM5 / feature level 11) and displaced onto a sphere; the tess factor animates
// 1..16 so you watch it refine from faceted to smooth.
#define PIF 3.14159265f

static const char *kTessHLSL =
    "cbuffer CB : register(b0) { row_major float4x4 mvp; float tessf; float3 pad; };\n"
    "struct VSOut { float3 pos : POSITION; };\n"
    "VSOut VSMain(float3 pos : POSITION) { VSOut o; o.pos = normalize(pos); return o; }\n"
    "struct PatchConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };\n"
    "PatchConst HSConst(InputPatch<VSOut,3> ip) {\n"
    "  PatchConst p; p.edges[0]=p.edges[1]=p.edges[2]=tessf; p.inside=tessf; return p; }\n"
    "[domain(\"tri\")][partitioning(\"fractional_odd\")][outputtopology(\"triangle_cw\")]\n"
    "[outputcontrolpoints(3)][patchconstantfunc(\"HSConst\")]\n"
    "VSOut HSMain(InputPatch<VSOut,3> ip, uint id : SV_OutputControlPointID) { return ip[id]; }\n"
    "struct DSOut { float4 pos : SV_POSITION; float3 col : COLOR; };\n"
    "[domain(\"tri\")]\n"
    "DSOut DSMain(PatchConst pc, float3 bary : SV_DomainLocation, const OutputPatch<VSOut,3> patch) {\n"
    "  float3 p = patch[0].pos*bary.x + patch[1].pos*bary.y + patch[2].pos*bary.z;\n"
    "  p = normalize(p); DSOut o; o.pos = mul(float4(p*1.6,1.0), mvp); o.col = p*0.5+0.5; return o; }\n"
    "float4 PSMain(DSOut i) : SV_TARGET { return float4(i.col, 1.0); }\n";

typedef struct {
    Mat4 mvp;
    float tessf;
    float pad[3];
} TessCB;

static struct {
    ID3D11VertexShader *vs;
    ID3D11HullShader *hs;
    ID3D11DomainShader *ds;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vbo, *cbo;
} g_tess;

static int tess_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *vsb = compile_hlsl(kTessHLSL, "VSMain", "vs_5_0");
    ID3DBlob *hsb = compile_hlsl(kTessHLSL, "HSMain", "hs_5_0");
    ID3DBlob *dsb = compile_hlsl(kTessHLSL, "DSMain", "ds_5_0");
    ID3DBlob *psb = compile_hlsl(kTessHLSL, "PSMain", "ps_5_0");
    if (!vsb || !hsb || !dsb || !psb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_tess.vs);
    ID3D11Device_CreateHullShader(dev, ID3D10Blob_GetBufferPointer(hsb),
                                  ID3D10Blob_GetBufferSize(hsb), NULL, &g_tess.hs);
    ID3D11Device_CreateDomainShader(dev, ID3D10Blob_GetBufferPointer(dsb),
                                    ID3D10Blob_GetBufferSize(dsb), NULL, &g_tess.ds);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_tess.ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, il, 1, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &g_tess.layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(hsb);
    ID3D10Blob_Release(dsb);
    ID3D10Blob_Release(psb);
    if (!g_tess.hs || !g_tess.ds) {
        fail_box(
            "Tessellation needs Direct3D 11 feature level 11.\n\n"
            "This container's device doesn't support hull/domain shaders.");
        return 1;
    }

    const float t = 1.618034f;
    const float ico[12][3] = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t},  {0, 1, t},
        {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1},
    };
    const int faces[20][3] = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
        {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
        {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
    };
    float cp[60][3];
    int n = 0;
    for (int f = 0; f < 20; f++)
        for (int k = 0; k < 3; k++) {
            cp[n][0] = ico[faces[f][k]][0];
            cp[n][1] = ico[faces[f][k]][1];
            cp[n][2] = ico[faces[f][k]][2];
            n++;
        }
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = sizeof(cp);
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = cp;
    ID3D11Device_CreateBuffer(dev, &vbd, &sr, &g_tess.vbo);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(TessCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_tess.cbo);
    return 0;
}

static void tess_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    float a = (float)t * 0.6f;
    Mat4 model = mat_mul(mat_rotate(0, 1, 0, a), mat_rotate(1, 0, 0, a * 0.4f));
    TessCB cb;
    cb.mvp = mat_mul(mat_mul(model, mat_translate(0, 0, -5)),
                     mat_perspective(0.7854f, aspect, 0.1f, 100.0f));
    cb.tessf = 1.0f + (sinf((float)t * 0.8f) * 0.5f + 0.5f) * 15.0f;  // 1..16

    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_tess.cbo, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &cb, sizeof(cb));
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_tess.cbo, 0);
    }

    UINT stride = sizeof(float) * 3, offset = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, g_tess.layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_tess.vbo, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_tess.vs, NULL, 0);
    ID3D11DeviceContext_HSSetShader(ctx, g_tess.hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(ctx, g_tess.ds, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_tess.ps, NULL, 0);
    ID3D11DeviceContext_HSSetConstantBuffers(ctx, 0, 1, &g_tess.cbo);
    ID3D11DeviceContext_DSSetConstantBuffers(ctx, 0, 1, &g_tess.cbo);
    ID3D11DeviceContext_Draw(ctx, 60, 0);
}

static void tess_cleanup(void) {
    if (g_tess.cbo) ID3D11Buffer_Release(g_tess.cbo);
    if (g_tess.vbo) ID3D11Buffer_Release(g_tess.vbo);
    if (g_tess.layout) ID3D11InputLayout_Release(g_tess.layout);
    if (g_tess.ps) ID3D11PixelShader_Release(g_tess.ps);
    if (g_tess.ds) ID3D11DomainShader_Release(g_tess.ds);
    if (g_tess.hs) ID3D11HullShader_Release(g_tess.hs);
    if (g_tess.vs) ID3D11VertexShader_Release(g_tess.vs);
    memset(&g_tess, 0, sizeof(g_tess));
}

// ============================== COMPUTE scene ===============================
// A compute shader (cs_5_0) advances a swirling particle cloud in a structured
// buffer each frame; the vertex shader reads it back by SV_VertexID and draws
// the particles as points. Exercises the entire D3D11 compute path + UAV/SRV.
#define PART_COUNT 131072  // 512 * 256
#define PART_GROUPS (PART_COUNT / 256)

static const char *kCompCS =
    "struct Particle { float3 pos; float3 vel; };\n"
    "RWStructuredBuffer<Particle> parts : register(u0);\n"
    "cbuffer CB : register(b0) { float dt; float time; float2 pad; };\n"
    "[numthreads(256,1,1)]\n"
    "void CSMain(uint3 id : SV_DispatchThreadID) {\n"
    "  Particle p = parts[id.x];\n"
    "  float3 toC = -p.pos; float d = length(toC) + 0.001;\n"
    "  float3 grav = toC/d * (3.0/(d*d+0.5));\n"
    "  float3 tang = cross(float3(0,1,0), p.pos);\n"
    "  p.vel += (grav + tang*0.25) * dt; p.vel *= 0.999;\n"
    "  p.pos += p.vel * dt;\n"
    "  if (length(p.pos) > 32.0) { p.pos *= 0.03; p.vel *= 0.2; }\n"
    "  parts[id.x] = p; }\n";

static const char *kCompVS =
    "struct Particle { float3 pos; float3 vel; };\n"
    "StructuredBuffer<Particle> parts : register(t0);\n"
    "cbuffer CB : register(b0) { row_major float4x4 mvp; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float3 col : COLOR; };\n"
    "VSOut VSMain(uint vid : SV_VertexID) {\n"
    "  Particle p = parts[vid]; VSOut o; o.pos = mul(float4(p.pos,1.0), mvp);\n"
    "  float sp = saturate(length(p.vel) * 0.25);\n"
    "  o.col = lerp(float3(0.15,0.35,1.0), float3(1.0,0.55,0.1), sp); return o; }\n"
    "float4 PSMain(VSOut i) : SV_TARGET { return float4(i.col, 1.0); }\n";

typedef struct {
    float pos[3];
    float vel[3];
} Particle;

typedef struct {
    float dt, time, pad[2];
} CompCB;

static struct {
    ID3D11ComputeShader *cs;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *buf, *cscb, *vscb;
    ID3D11UnorderedAccessView *uav;
    ID3D11ShaderResourceView *srv;
} g_comp;

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static int comp_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *csb = compile_hlsl(kCompCS, "CSMain", "cs_5_0");
    ID3DBlob *vsb = compile_hlsl(kCompVS, "VSMain", "vs_5_0");
    ID3DBlob *psb = compile_hlsl(kCompVS, "PSMain", "ps_5_0");
    if (!csb || !vsb || !psb) return 1;
    ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(csb),
                                     ID3D10Blob_GetBufferSize(csb), NULL, &g_comp.cs);
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_comp.vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_comp.ps);
    ID3D10Blob_Release(csb);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    if (!g_comp.cs) {
        fail_box("Compute shaders (cs_5_0) are not available on this D3D11 device.");
        return 1;
    }

    Particle *init = (Particle *)malloc(sizeof(Particle) * PART_COUNT);
    if (!init) return 1;
    srand(1234);
    for (int i = 0; i < PART_COUNT; i++) {
        float r = 4.0f + frand() * 8.0f;
        float th = frand() * 2.0f * PIF, ph = (frand() - 0.5f) * PIF;
        init[i].pos[0] = r * cosf(ph) * cosf(th);
        init[i].pos[1] = r * sinf(ph);
        init[i].pos[2] = r * cosf(ph) * sinf(th);
        // tangential initial velocity (swirl)
        init[i].vel[0] = -init[i].pos[2] * 0.15f;
        init[i].vel[1] = (frand() - 0.5f) * 0.4f;
        init[i].vel[2] = init[i].pos[0] * 0.15f;
    }
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(Particle) * PART_COUNT;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(Particle);
    D3D11_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = init;
    ID3D11Device_CreateBuffer(dev, &bd, &sr, &g_comp.buf);
    free(init);

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof(ud));
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = PART_COUNT;
    ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)g_comp.buf, &ud, &g_comp.uav);

    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = PART_COUNT;
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_comp.buf, &sd, &g_comp.srv);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.ByteWidth = sizeof(CompCB);
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_comp.cscb);
    cbd.ByteWidth = sizeof(Mat4);
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_comp.vscb);
    return (g_comp.uav && g_comp.srv) ? 0 : 1;
}

static void comp_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    // Advance the simulation (fixed dt for stability).
    CompCB cb = {0.016f, (float)t, {0, 0}};
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_comp.cscb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &cb, sizeof(cb));
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_comp.cscb, 0);
    }
    ID3D11DeviceContext_CSSetShader(ctx, g_comp.cs, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &g_comp.cscb);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &g_comp.uav, NULL);
    ID3D11DeviceContext_Dispatch(ctx, PART_GROUPS, 1, 1);
    // Unbind UAV + compute shader before reading the buffer as an SRV.
    ID3D11UnorderedAccessView *nuav = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nuav, NULL);
    ID3D11DeviceContext_CSSetShader(ctx, NULL, NULL, 0);

    Mat4 world = mat_mul(mat_rotate(0, 1, 0, (float)t * 0.2f), mat_rotate(1, 0, 0, 0.35f));
    Mat4 mvp = mat_mul(mat_mul(world, mat_translate(0, 0, -70)),
                       mat_perspective(0.9f, aspect, 0.1f, 300.0f));
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_comp.vscb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, mvp.m, sizeof(mvp.m));
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_comp.vscb, 0);
    }
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_comp.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_comp.vscb);
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &g_comp.srv);
    ID3D11DeviceContext_PSSetShader(ctx, g_comp.ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, PART_COUNT, 0);
    // Unbind the SRV so next frame's compute can take the UAV.
    ID3D11ShaderResourceView *nsrv = NULL;
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &nsrv);
}

static void comp_cleanup(void) {
    if (g_comp.vscb) ID3D11Buffer_Release(g_comp.vscb);
    if (g_comp.cscb) ID3D11Buffer_Release(g_comp.cscb);
    if (g_comp.srv) ID3D11ShaderResourceView_Release(g_comp.srv);
    if (g_comp.uav) ID3D11UnorderedAccessView_Release(g_comp.uav);
    if (g_comp.buf) ID3D11Buffer_Release(g_comp.buf);
    if (g_comp.ps) ID3D11PixelShader_Release(g_comp.ps);
    if (g_comp.vs) ID3D11VertexShader_Release(g_comp.vs);
    if (g_comp.cs) ID3D11ComputeShader_Release(g_comp.cs);
    memset(&g_comp, 0, sizeof(g_comp));
}

// ============================== DOLPHIN scene ===============================
// The classic DolphinVS underwater scene, reproduced from the original Microsoft
// DirectX SDK assets (embedded in dolphin_assets.h): the real 284-vertex dolphin
// mesh tweened between its 3 keyframe poses (Dolphin1/2/3.x) for the swim, its
// skin texture, the seafloor mesh + texture, and the 32-frame animated caustics,
// with underwater fog. The 3-keyframe position+normal tween reproduces the
// original DolphinTween.vsh technique.
static const char *kDolBodyHLSL =
    "cbuffer CB:register(b0){row_major float4x4 mvp;float4 weights;float4 lightdir;float4 fog;};\n"
    "Texture2D dtex:register(t0); SamplerState smp:register(s0);\n"
    "struct VSIn{float3 p0:POSITION0;float3 p1:POSITION1;float3 p2:POSITION2;"
    "float3 n0:NORMAL0;float3 n1:NORMAL1;float3 n2:NORMAL2;float2 uv:TEXCOORD0;};\n"
    "struct VSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;float3 nrm:NORMAL;float fog:FOG;};\n"
    "VSOut VSMain(VSIn i){\n"
    "  float3 p = i.p0*weights.x + i.p1*weights.y + i.p2*weights.z;\n"
    "  float3 n = i.n0*weights.x + i.n1*weights.y + i.n2*weights.z;\n"
    "  VSOut o; o.pos = mul(float4(p,1.0),mvp); o.uv=i.uv; o.nrm=n;\n"
    "  o.fog = saturate((o.pos.w - fog.x)/(fog.y - fog.x)); return o; }\n"
    "float4 PSMain(VSOut i):SV_TARGET{\n"
    "  float3 N=normalize(i.nrm); float3 L=normalize(lightdir.xyz);\n"
    "  float d = saturate(dot(N,L))*0.75 + 0.45;\n"
    "  float3 c = dtex.Sample(smp,i.uv).rgb * d;\n"
    "  c = lerp(c, float3(0.10,0.32,0.45), i.fog); return float4(c,1.0); }\n";

static const char *kSeaHLSL =
    "cbuffer CB:register(b0){row_major float4x4 mvp;float4 weights;float4 lightdir;float4 fog;};\n"
    "Texture2D stex:register(t0); Texture2DArray ctex:register(t1); SamplerState smp:register(s0);\n"
    "struct VSIn{float3 pos:POSITION0;float3 nrm:NORMAL0;float2 uv:TEXCOORD0;};\n"
    "struct VSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;float fog:FOG;};\n"
    "VSOut VSMain(VSIn i){VSOut o; o.pos=mul(float4(i.pos,1.0),mvp); o.uv=i.uv;\n"
    "  o.fog=saturate((o.pos.w - fog.x)/(fog.y - fog.x)); return o; }\n"
    "float4 PSMain(VSOut i):SV_TARGET{\n"
    "  float3 base = stex.Sample(smp,i.uv).rgb;\n"
    "  float caus = ctex.Sample(smp, float3(i.uv*3.0, fog.z)).r;\n"
    "  float3 c = base*(0.55 + caus*1.1);\n"
    "  c = lerp(c, float3(0.10,0.32,0.45), i.fog); return float4(c,1.0); }\n";

typedef struct {
    Mat4 mvp;
    float weights[4];
    float light[4];
    float fog[4];  // x=start y=end z=caust-frame w=time
} DolSceneCB;

static struct {
    ID3D11VertexShader *dvs, *svs;
    ID3D11PixelShader *dps, *sps;
    ID3D11InputLayout *dlayout, *slayout;
    ID3D11Buffer *dvbo, *dibo, *svbo, *sibo, *cbo;
    ID3D11Texture2D *dtex, *stex, *ctex;
    ID3D11ShaderResourceView *dsrv, *ssrv, *csrv;
    ID3D11SamplerState *smp;
    ID3D11RasterizerState *rs;
} g_dol;

static void dol_upload(ID3D11DeviceContext *ctx, const DolSceneCB *cb) {
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_dol.cbo, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, cb, sizeof(*cb));
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_dol.cbo, 0);
    }
}

static int dol_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *dvb = compile_hlsl(kDolBodyHLSL, "VSMain", "vs_4_0");
    ID3DBlob *dpb = compile_hlsl(kDolBodyHLSL, "PSMain", "ps_4_0");
    ID3DBlob *svb = compile_hlsl(kSeaHLSL, "VSMain", "vs_4_0");
    ID3DBlob *spb = compile_hlsl(kSeaHLSL, "PSMain", "ps_4_0");
    if (!dvb || !dpb || !svb || !spb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(dvb),
                                    ID3D10Blob_GetBufferSize(dvb), NULL, &g_dol.dvs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(dpb),
                                   ID3D10Blob_GetBufferSize(dpb), NULL, &g_dol.dps);
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(svb),
                                    ID3D10Blob_GetBufferSize(svb), NULL, &g_dol.svs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(spb),
                                   ID3D10Blob_GetBufferSize(spb), NULL, &g_dol.sps);

    D3D11_INPUT_ELEMENT_DESC dil[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"POSITION", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"POSITION", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 72, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, dil, 7, ID3D10Blob_GetBufferPointer(dvb),
                                   ID3D10Blob_GetBufferSize(dvb), &g_dol.dlayout);
    D3D11_INPUT_ELEMENT_DESC sil[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, sil, 3, ID3D10Blob_GetBufferPointer(svb),
                                   ID3D10Blob_GetBufferSize(svb), &g_dol.slayout);
    ID3D10Blob_Release(dvb);
    ID3D10Blob_Release(dpb);
    ID3D10Blob_Release(svb);
    ID3D10Blob_Release(spb);

    // Interleave dolphin vertex buffer: 3 positions + 3 normals + uv = 20 floats.
    float *dv = (float *)malloc(sizeof(float) * 20 * DOLPHIN_NVERTS);
    if (!dv) return 1;
    for (int i = 0; i < DOLPHIN_NVERTS; i++) {
        float *o = &dv[i * 20];
        memcpy(o + 0, &dolphin_pos1[i * 3], 12);
        memcpy(o + 3, &dolphin_pos2[i * 3], 12);
        memcpy(o + 6, &dolphin_pos3[i * 3], 12);
        memcpy(o + 9, &dolphin_nrm1[i * 3], 12);
        memcpy(o + 12, &dolphin_nrm2[i * 3], 12);
        memcpy(o + 15, &dolphin_nrm3[i * 3], 12);
        memcpy(o + 18, &dolphin_uv[i * 2], 8);
    }
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sr;
    memset(&bd, 0, sizeof(bd));
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(sizeof(float) * 20 * DOLPHIN_NVERTS);
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = dv;
    ID3D11Device_CreateBuffer(dev, &bd, &sr, &g_dol.dvbo);
    free(dv);

    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = sizeof(dolphin_idx);
    sr.pSysMem = dolphin_idx;
    ID3D11Device_CreateBuffer(dev, &bd, &sr, &g_dol.dibo);

    // Seafloor vertex buffer: pos + normal + uv = 8 floats.
    float *sv = (float *)malloc(sizeof(float) * 8 * SEAFLOOR_NVERTS);
    if (!sv) return 1;
    for (int i = 0; i < SEAFLOOR_NVERTS; i++) {
        float *o = &sv[i * 8];
        memcpy(o + 0, &seafloor_pos[i * 3], 12);
        memcpy(o + 3, &seafloor_nrm[i * 3], 12);
        memcpy(o + 6, &seafloor_uv[i * 2], 8);
    }
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(sizeof(float) * 8 * SEAFLOOR_NVERTS);
    sr.pSysMem = sv;
    ID3D11Device_CreateBuffer(dev, &bd, &sr, &g_dol.svbo);
    free(sv);

    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = sizeof(seafloor_idx);
    sr.pSysMem = seafloor_idx;
    ID3D11Device_CreateBuffer(dev, &bd, &sr, &g_dol.sibo);

    // Constant buffer.
    memset(&bd, 0, sizeof(bd));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(DolSceneCB);
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &g_dol.cbo);

    // Dolphin + seafloor textures (RGBA8).
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA tsr;
    memset(&td, 0, sizeof(td));
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Width = DOLTEX_W;
    td.Height = DOLTEX_H;
    memset(&tsr, 0, sizeof(tsr));
    tsr.pSysMem = dolphin_tex;
    tsr.SysMemPitch = DOLTEX_W * 4;
    ID3D11Device_CreateTexture2D(dev, &td, &tsr, &g_dol.dtex);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_dol.dtex, NULL, &g_dol.dsrv);
    td.Width = SEATEX_W;
    td.Height = SEATEX_H;
    tsr.pSysMem = seafloor_tex;
    tsr.SysMemPitch = SEATEX_W * 4;
    ID3D11Device_CreateTexture2D(dev, &td, &tsr, &g_dol.stex);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_dol.stex, NULL, &g_dol.ssrv);

    // Caustic texture array: 32 single-channel (R8) frames.
    D3D11_TEXTURE2D_DESC cd;
    memset(&cd, 0, sizeof(cd));
    cd.Width = CAUST_W;
    cd.Height = CAUST_H;
    cd.MipLevels = 1;
    cd.ArraySize = CAUST_FRAMES;
    cd.Format = DXGI_FORMAT_R8_UNORM;
    cd.SampleDesc.Count = 1;
    cd.Usage = D3D11_USAGE_IMMUTABLE;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA csr[CAUST_FRAMES];
    for (int i = 0; i < CAUST_FRAMES; i++) {
        csr[i].pSysMem = &caust_tex[i * CAUST_W * CAUST_H];
        csr[i].SysMemPitch = CAUST_W;
        csr[i].SysMemSlicePitch = 0;
    }
    ID3D11Device_CreateTexture2D(dev, &cd, csr, &g_dol.ctex);
    D3D11_SHADER_RESOURCE_VIEW_DESC cv;
    memset(&cv, 0, sizeof(cv));
    cv.Format = DXGI_FORMAT_R8_UNORM;
    cv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    cv.Texture2DArray.MipLevels = 1;
    cv.Texture2DArray.ArraySize = CAUST_FRAMES;
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_dol.ctex, &cv, &g_dol.csrv);

    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &sd, &g_dol.smp);

    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // mesh winding not guaranteed; show both sides
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &g_dol.rs);
    return (g_dol.dsrv && g_dol.ssrv && g_dol.csrv) ? 0 : 1;
}

static void dol_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    // Orbiting camera (object fixed in world -> lighting stays put).
    float ang = (float)t * 0.3f, R = 7.0f;
    Mat4 view = mat_lookat(R * sinf(ang), 2.0f, R * cosf(ang), 0.0f, -0.4f, 0.0f, 0, 1, 0);
    Mat4 vp = mat_mul(view, mat_perspective(0.85f, aspect, 0.1f, 100.0f));

    DolSceneCB cb;
    cb.light[0] = 0.3f; cb.light[1] = 1.0f; cb.light[2] = 0.4f; cb.light[3] = 0.0f;
    cb.fog[0] = 6.0f; cb.fog[1] = 16.0f;
    cb.fog[2] = (float)(((int)(t * 15.0)) % CAUST_FRAMES);
    cb.fog[3] = (float)t;

    ID3D11DeviceContext_RSSetState(ctx, g_dol.rs);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // --- Seafloor ---
    Mat4 m_sea = mat_mul(mat_scale(0.04f), mat_translate(0.0f, -2.5f, 0.0f));
    cb.mvp = mat_mul(m_sea, vp);
    cb.weights[0] = cb.weights[1] = cb.weights[2] = cb.weights[3] = 0.0f;
    dol_upload(ctx, &cb);
    UINT ss = sizeof(float) * 8, so = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, g_dol.slayout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_dol.svbo, &ss, &so);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, g_dol.sibo, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_VSSetShader(ctx, g_dol.svs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_dol.cbo);
    ID3D11DeviceContext_PSSetShader(ctx, g_dol.sps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_dol.cbo);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &g_dol.smp);
    ID3D11ShaderResourceView *seasrv[2] = {g_dol.ssrv, g_dol.csrv};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, seasrv);
    ID3D11DeviceContext_DrawIndexed(ctx, SEAFLOOR_NINDICES, 0, 0);

    // --- Dolphin (3-keyframe swim tween) ---
    Mat4 m_dol = mat_mul(mat_scale(0.01f), mat_translate(-0.25f, 0.27f, 0.0f));
    cb.mvp = mat_mul(m_dol, vp);
    float phase = fmodf((float)t * 1.6f, 3.0f);
    int seg = (int)phase;
    float frac = phase - (float)seg;
    cb.weights[0] = cb.weights[1] = cb.weights[2] = 0.0f;
    cb.weights[seg] = 1.0f - frac;
    cb.weights[(seg + 1) % 3] = frac;
    dol_upload(ctx, &cb);
    UINT ds = sizeof(float) * 20, dofs = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, g_dol.dlayout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_dol.dvbo, &ds, &dofs);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, g_dol.dibo, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_VSSetShader(ctx, g_dol.dvs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_dol.dps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_dol.dsrv);
    ID3D11DeviceContext_DrawIndexed(ctx, DOLPHIN_NINDICES, 0, 0);
}

static void dol_cleanup(void) {
    if (g_dol.rs) ID3D11RasterizerState_Release(g_dol.rs);
    if (g_dol.smp) ID3D11SamplerState_Release(g_dol.smp);
    if (g_dol.csrv) ID3D11ShaderResourceView_Release(g_dol.csrv);
    if (g_dol.ssrv) ID3D11ShaderResourceView_Release(g_dol.ssrv);
    if (g_dol.dsrv) ID3D11ShaderResourceView_Release(g_dol.dsrv);
    if (g_dol.ctex) ID3D11Texture2D_Release(g_dol.ctex);
    if (g_dol.stex) ID3D11Texture2D_Release(g_dol.stex);
    if (g_dol.dtex) ID3D11Texture2D_Release(g_dol.dtex);
    if (g_dol.cbo) ID3D11Buffer_Release(g_dol.cbo);
    if (g_dol.sibo) ID3D11Buffer_Release(g_dol.sibo);
    if (g_dol.svbo) ID3D11Buffer_Release(g_dol.svbo);
    if (g_dol.dibo) ID3D11Buffer_Release(g_dol.dibo);
    if (g_dol.dvbo) ID3D11Buffer_Release(g_dol.dvbo);
    if (g_dol.slayout) ID3D11InputLayout_Release(g_dol.slayout);
    if (g_dol.dlayout) ID3D11InputLayout_Release(g_dol.dlayout);
    if (g_dol.sps) ID3D11PixelShader_Release(g_dol.sps);
    if (g_dol.svs) ID3D11VertexShader_Release(g_dol.svs);
    if (g_dol.dps) ID3D11PixelShader_Release(g_dol.dps);
    if (g_dol.dvs) ID3D11VertexShader_Release(g_dol.dvs);
    memset(&g_dol, 0, sizeof(g_dol));
}

// ============================== RAYMARCH scene ==============================
// An original signed-distance-field scene ray-marched entirely in the pixel
// shader over a single fullscreen triangle (no vertex/index buffers). A rotating
// torus and four orbiting spheres are blended with a smooth-min onto a ground
// plane, then lit with a key light + soft ray-marched shadows + cheap AO + fog.
// All procedural / original content -- this doubles as a fragment-shader (ALU)
// stress test. The VS builds the fullscreen triangle from SV_VertexID and passes
// clip-space xy; the PS does all the work. Only iTime + aspect feed in, so it is
// resolution-independent.
static const char *kRaymarchHLSL =
    "cbuffer CB : register(b0) { float iTime; float iAspect; float2 pad; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };\n"
    "VSOut VSMain(uint vid : SV_VertexID) {\n"
    "  VSOut o; float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); o.ndc = o.pos.xy; return o;\n"
    "}\n"
    "float2 rot(float2 v, float a){ float s=sin(a),c=cos(a); return float2(v.x*c-v.y*s, v.x*s+v.y*c); }\n"
    "float sdSphere(float3 p, float r){ return length(p)-r; }\n"
    "float sdTorus(float3 p, float2 t){ float2 q=float2(length(p.xz)-t.x, p.y); return length(q)-t.y; }\n"
    "float smin(float a, float b, float k){ float h=saturate(0.5+0.5*(b-a)/k); return lerp(b,a,h)-k*h*(1.0-h); }\n"
    "float map(float3 p){\n"
    "  float3 q = p; q.xz = rot(q.xz, iTime*0.5);\n"
    "  float obj = sdTorus(q, float2(1.5, 0.45));\n"
    "  [unroll] for (int i=0;i<4;i++){ float a=iTime*0.8 + i*1.5707963;\n"
    "    float3 c=float3(cos(a)*1.9, sin(iTime+i)*0.6, sin(a)*1.9);\n"
    "    obj = smin(obj, sdSphere(p-c, 0.55), 0.55); }\n"
    "  float ground = p.y + 1.3;\n"
    "  return min(obj, ground);\n"
    "}\n"
    "float3 calcN(float3 p){ float2 e=float2(0.0012,0.0);\n"
    "  return normalize(float3(map(p+e.xyy)-map(p-e.xyy), map(p+e.yxy)-map(p-e.yxy), map(p+e.yyx)-map(p-e.yyx))); }\n"
    "float shadow(float3 ro, float3 rd){ float res=1.0, t=0.05;\n"
    "  [loop] for(int i=0;i<32;i++){ float h=map(ro+rd*t); if(h<0.001) return 0.0;\n"
    "    res=min(res, 8.0*h/t); t+=clamp(h,0.02,0.3); if(t>20.0) break; } return saturate(res); }\n"
    "float4 PSMain(VSOut inp) : SV_TARGET {\n"
    "  float2 uv = inp.ndc; uv.x *= iAspect;\n"
    "  float3 ro = float3(0.0, 1.6, 5.6);\n"
    "  float3 fw = normalize(float3(0.0,-0.2,0.0) - ro);\n"
    "  float3 rt = normalize(cross(float3(0.0,1.0,0.0), fw)); float3 up = cross(fw, rt);\n"
    "  float3 rd = normalize(uv.x*rt + uv.y*up + 1.7*fw);\n"
    "  float t = 0.0; int steps = 0;\n"
    "  [loop] for(int i=0;i<96;i++){ steps=i; float d=map(ro+rd*t); if(d<0.001) break; t+=d; if(t>40.0) break; }\n"
    "  float3 col;\n"
    "  if (t < 40.0) {\n"
    "    float3 p = ro+rd*t; float3 n = calcN(p);\n"
    "    float3 lp = normalize(float3(0.7,0.85,0.35));\n"
    "    float dif = saturate(dot(n, lp)); float sh = shadow(p+n*0.02, lp);\n"
    "    float ao = 1.0 - (float)steps/96.0;\n"
    "    float3 base = 0.5 + 0.5*cos(float3(0.0,2.0,4.0) + p.y*0.6 + iTime*0.2);\n"
    "    col = base*(0.22 + dif*sh);\n"
    "    float3 hv = normalize(lp - rd); col += pow(saturate(dot(n,hv)), 32.0)*sh*0.7;\n"
    "    col *= 0.55 + 0.45*ao;\n"
    "    col = lerp(col, float3(0.5,0.65,0.85), 1.0 - exp(-0.0008*t*t*t));\n"
    "  } else {\n"
    "    col = lerp(float3(0.55,0.70,0.95), float3(0.12,0.18,0.32), saturate(rd.y + 0.25));\n"
    "  }\n"
    "  col = pow(saturate(col), 1.0/2.2);\n"
    "  return float4(col, 1.0);\n"
    "}\n";

typedef struct {
    float iTime;
    float iAspect;
    float pad0, pad1;
} RayCB;

static struct {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *cbo;
    ID3D11RasterizerState *rs;
} g_ray;

static int ray_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *vsb = compile_hlsl(kRaymarchHLSL, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(kRaymarchHLSL, "PSMain", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_ray.vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_ray.ps);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(RayCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_ray.cbo);

    // The fullscreen triangle covers the screen regardless of winding; disable
    // culling so it is never back-face culled (the runner sets no rasterizer
    // state, so the default CULL_BACK would otherwise discard a CCW triangle).
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &g_ray.rs);
    return (g_ray.vs && g_ray.ps && g_ray.cbo && g_ray.rs) ? 0 : 1;
}

static void ray_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_ray.cbo, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        RayCB *cb = (RayCB *)m.pData;
        cb->iTime = (float)t;
        cb->iAspect = aspect;
        cb->pad0 = cb->pad1 = 0.0f;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_ray.cbo, 0);
    }
    ID3D11DeviceContext_RSSetState(ctx, g_ray.rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_ray.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_ray.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_ray.cbo);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
}

static void ray_cleanup(void) {
    if (g_ray.rs) ID3D11RasterizerState_Release(g_ray.rs);
    if (g_ray.cbo) ID3D11Buffer_Release(g_ray.cbo);
    if (g_ray.ps) ID3D11PixelShader_Release(g_ray.ps);
    if (g_ray.vs) ID3D11VertexShader_Release(g_ray.vs);
    memset(&g_ray, 0, sizeof(g_ray));
}

// ===================== procedural "shader scene" framework ==================
// Original Shadertoy-style fragment scenes: a fullscreen triangle (no vertex
// buffer) + a heavy pixel shader. All scenes share the same VS (builds the
// triangle from SV_VertexID) and a {iTime, iAspect} constant buffer; each scene
// supplies only its PS. CULL_NONE so the CCW triangle is never culled. 100%
// original / standard-math content (clean-room; nothing from any third party).
#define AIO_STOY_HEADER                                                        \
    "cbuffer CB:register(b0){float iTime;float iAspect;float2 pad;}\n"         \
    "struct VSOut{float4 pos:SV_POSITION;float2 ndc:TEXCOORD0;};\n"            \
    "VSOut VSMain(uint vid:SV_VertexID){VSOut o;float2 p=float2((vid<<1)&2,vid&2);" \
    "o.pos=float4(p*2.0-1.0,0.0,1.0);o.ndc=o.pos.xy;return o;}\n"

typedef struct {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *cbo;
    ID3D11RasterizerState *rs;
} StoyState;

static int stoy_init(ID3D11Device *dev, StoyState *s, const char *hlsl) {
    ID3DBlob *vsb = compile_hlsl(hlsl, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(hlsl, "PSMain", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s->vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &s->ps);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(RayCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &s->cbo);
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &s->rs);
    return (s->vs && s->ps && s->cbo && s->rs) ? 0 : 1;
}

static void stoy_frame(ID3D11DeviceContext *ctx, StoyState *s, double t, float aspect) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s->cbo, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                          &m))) {
        RayCB *cb = (RayCB *)m.pData;
        cb->iTime = (float)t;
        cb->iAspect = aspect;
        cb->pad0 = cb->pad1 = 0.0f;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s->cbo, 0);
    }
    ID3D11DeviceContext_RSSetState(ctx, s->rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, s->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, s->ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &s->cbo);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
}

static void stoy_cleanup(StoyState *s) {
    if (s->rs) ID3D11RasterizerState_Release(s->rs);
    if (s->cbo) ID3D11Buffer_Release(s->cbo);
    if (s->ps) ID3D11PixelShader_Release(s->ps);
    if (s->vs) ID3D11VertexShader_Release(s->vs);
    memset(s, 0, sizeof(*s));
}

// ------------------------------- OCEAN scene --------------------------------
// A procedural sea: directional swell waves + fbm chop, ray-marched as a height
// field, with sky reflection (fresnel), a sharp sun glint, and horizon fog.
static const char *kOceanHLSL =
    AIO_STOY_HEADER
    "float h11(float2 p){p=frac(p*float2(123.34,456.21));p+=dot(p,p+45.32);return frac(p.x*p.y);}\n"
    "float vn(float2 p){float2 i=floor(p),f=frac(p);f=f*f*(3.0-2.0*f);\n"
    "  float a=h11(i),b=h11(i+float2(1,0)),c=h11(i+float2(0,1)),d=h11(i+float2(1,1));\n"
    "  return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);}\n"
    "float fbm(float2 p){float s=0.0,a=0.5;[unroll]for(int i=0;i<5;i++){s+=a*vn(p);p*=2.02;a*=0.5;}return s;}\n"
    "float wave(float2 xz){float t=iTime;\n"
    "  float h=sin(xz.x*0.6+t*1.1)*0.22+sin(xz.y*0.5-t*0.9)*0.18+sin((xz.x+xz.y)*0.35+t*0.7)*0.15;\n"
    "  h+=(fbm(xz*0.7+t*0.15)-0.5)*0.4; return h;}\n"
    "float3 wnormal(float3 p){float e=0.10;float h=wave(p.xz);\n"
    "  return normalize(float3(h-wave(p.xz+float2(e,0)), e, h-wave(p.xz+float2(0,e))));}\n"
    "float4 PSMain(VSOut inp):SV_TARGET{\n"
    "  float2 uv=inp.ndc; uv.x*=iAspect;\n"
    "  float3 ro=float3(0.0,2.4,4.0-iTime*0.7);\n"
    "  float3 ta=ro+float3(0.0,-0.55,-1.0);\n"
    "  float3 fw=normalize(ta-ro),rt=normalize(cross(float3(0,1,0),fw)),up=cross(fw,rt);\n"
    "  float3 rd=normalize(uv.x*rt+uv.y*up+1.5*fw);\n"
    "  float3 sun=normalize(float3(0.4,0.5,-0.75));\n"
    "  float t=0.0; bool hit=false; float3 p=ro;\n"
    "  [loop]for(int i=0;i<80;i++){p=ro+rd*t;float hh=wave(p.xz);if(p.y<hh+0.02){hit=true;break;}t+=max(0.12,(p.y-hh)*0.45);if(t>70.0)break;}\n"
    "  float3 col;\n"
    "  if(hit){float3 n=wnormal(p);\n"
    "    float fres=pow(1.0-saturate(dot(n,-rd)),4.0);\n"
    "    float3 water=lerp(float3(0.02,0.10,0.18),float3(0.10,0.36,0.42),saturate(n.y));\n"
    "    float3 sky=lerp(float3(0.55,0.72,0.95),float3(0.85,0.9,1.0),saturate(rd.y*0.5+0.5));\n"
    "    float dif=saturate(dot(n,sun));\n"
    "    float3 hv=normalize(sun-rd);float spec=pow(saturate(dot(n,hv)),120.0);\n"
    "    col=lerp(water*(0.3+0.7*dif),sky,fres*0.6)+spec*2.0;\n"
    "    col=lerp(col,float3(0.72,0.82,0.96),1.0-exp(-0.00009*t*t*t));\n"
    "  }else{float sd=pow(saturate(dot(rd,sun)),500.0);\n"
    "    col=lerp(float3(0.5,0.68,0.95),float3(0.86,0.91,1.0),saturate(rd.y*1.2));\n"
    "    col+=sd*float3(1.0,0.9,0.7)*2.0;}\n"
    "  col=pow(saturate(col),1.0/2.2); return float4(col,1.0);}\n";

static StoyState g_ocean;
static int ocean_init(ID3D11Device *d, ID3D11DeviceContext *c, int w, int h) {
    (void)c; (void)w; (void)h; return stoy_init(d, &g_ocean, kOceanHLSL);
}
static void ocean_frame(ID3D11DeviceContext *c, double t, float a) { stoy_frame(c, &g_ocean, t, a); }
static void ocean_cleanup(void) { stoy_cleanup(&g_ocean); }

// ----------------------------- MANDELBULB scene -----------------------------
// The power-8 Mandelbulb (standard distance estimator) ray-marched, with
// orbit-trap coloring + AO. A heavy fragment-ALU workload.
static const char *kBulbHLSL =
    AIO_STOY_HEADER
    "float demb(float3 pos,out float trap){float3 z=pos;float dr=1.0,r=0.0;trap=1e10;\n"
    "  [loop]for(int i=0;i<7;i++){r=length(z);if(r>2.0)break;\n"
    "    float th=acos(clamp(z.z/r,-1.0,1.0)),ph=atan2(z.y,z.x);float pw=8.0;\n"
    "    dr=pow(r,pw-1.0)*pw*dr+1.0;float zr=pow(r,pw);th*=pw;ph*=pw;\n"
    "    z=zr*float3(sin(th)*cos(ph),sin(ph)*sin(th),cos(th))+pos;trap=min(trap,r);}\n"
    "  return 0.5*log(max(r,1e-6))*r/max(dr,1e-6);}\n"
    "float3 calcN(float3 p){float2 e=float2(0.0009,0.0);float tr;\n"
    "  return normalize(float3(demb(p+e.xyy,tr)-demb(p-e.xyy,tr),demb(p+e.yxy,tr)-demb(p-e.yxy,tr),demb(p+e.yyx,tr)-demb(p-e.yyx,tr)));}\n"
    "float4 PSMain(VSOut inp):SV_TARGET{\n"
    "  float2 uv=inp.ndc; uv.x*=iAspect;\n"
    "  float a=iTime*0.25;float3 ro=float3(sin(a)*2.6,0.55,cos(a)*2.6);\n"
    "  float3 fw=normalize(-ro),rt=normalize(cross(float3(0,1,0),fw)),up=cross(fw,rt);\n"
    "  float3 rd=normalize(uv.x*rt+uv.y*up+1.9*fw);\n"
    "  float t=0.0,trap=0.0;bool hit=false;int steps=0;\n"
    "  [loop]for(int i=0;i<96;i++){steps=i;float tr;float d=demb(ro+rd*t,tr);trap=tr;if(d<0.0008){hit=true;break;}t+=d;if(t>6.0)break;}\n"
    "  float3 col;\n"
    "  if(hit){float3 p=ro+rd*t;float3 n=calcN(p);float3 L=normalize(float3(0.6,0.7,0.4));\n"
    "    float dif=saturate(dot(n,L));float ao=1.0-(float)steps/96.0;\n"
    "    float3 base=0.5+0.5*cos(float3(0.0,1.0,2.0)*2.0+trap*9.0+iTime*0.2);\n"
    "    col=base*(0.2+0.8*dif)*ao;float3 hv=normalize(L-rd);col+=pow(saturate(dot(n,hv)),32.0)*0.5;\n"
    "  }else{col=float3(0.02,0.02,0.05)+max(0.0,rd.y)*0.03;}\n"
    "  col=pow(saturate(col),1.0/2.2); return float4(col,1.0);}\n";

static StoyState g_bulb;
static int bulb_init(ID3D11Device *d, ID3D11DeviceContext *c, int w, int h) {
    (void)c; (void)w; (void)h; return stoy_init(d, &g_bulb, kBulbHLSL);
}
static void bulb_frame(ID3D11DeviceContext *c, double t, float a) { stoy_frame(c, &g_bulb, t, a); }
static void bulb_cleanup(void) { stoy_cleanup(&g_bulb); }

// ------------------------------ NEBULA scene --------------------------------
// A volumetric nebula: ray-marched 3D fbm noise accumulated with emission +
// absorption, a slowly rotating colorful cloud. Heavy (volume march x 3D fbm).
static const char *kNebulaHLSL =
    AIO_STOY_HEADER
    "float h13(float3 p){p=frac(p*0.3183099+0.1);p*=17.0;return frac(p.x*p.y*p.z*(p.x+p.y+p.z));}\n"
    "float vn3(float3 p){float3 i=floor(p),f=frac(p);f=f*f*(3.0-2.0*f);\n"
    "  return lerp(lerp(lerp(h13(i+float3(0,0,0)),h13(i+float3(1,0,0)),f.x),lerp(h13(i+float3(0,1,0)),h13(i+float3(1,1,0)),f.x),f.y),\n"
    "              lerp(lerp(h13(i+float3(0,0,1)),h13(i+float3(1,0,1)),f.x),lerp(h13(i+float3(0,1,1)),h13(i+float3(1,1,1)),f.x),f.y),f.z);}\n"
    "float fbm3(float3 p){float s=0.0,a=0.5;[unroll]for(int i=0;i<5;i++){s+=a*vn3(p);p=p*2.03+0.13;a*=0.5;}return s;}\n"
    "float4 PSMain(VSOut inp):SV_TARGET{\n"
    "  float2 uv=inp.ndc; uv.x*=iAspect;\n"
    "  float a=iTime*0.06;float3 ro=float3(0,0,-3.0);float3 rd=normalize(float3(uv,1.6));\n"
    "  float c=cos(a),s=sin(a);rd.xz=mul(rd.xz,float2x2(c,-s,s,c));\n"
    "  float3 acc=float3(0,0,0);float trans=1.0;float t=0.5;\n"
    "  [loop]for(int i=0;i<48;i++){float3 p=ro+rd*t;\n"
    "    float den=fbm3(p*0.9+float3(0,0,iTime*0.1))-0.52;den=saturate(den)*1.5;\n"
    "    if(den>0.001){float3 emit=0.5+0.5*cos(float3(0.0,1.5,2.5)+length(p)*0.6+iTime*0.1);\n"
    "      acc+=trans*emit*den*0.5;trans*=exp(-den*0.4);}\n"
    "    t+=0.16;if(trans<0.02||t>9.0)break;}\n"
    "  float3 col=acc+float3(0.02,0.01,0.04)*(1.0-trans);\n"
    "  col=pow(saturate(col),1.0/2.2); return float4(col,1.0);}\n";

static StoyState g_nebula;
static int nebula_init(ID3D11Device *d, ID3D11DeviceContext *c, int w, int h) {
    (void)c; (void)w; (void)h; return stoy_init(d, &g_nebula, kNebulaHLSL);
}
static void nebula_frame(ID3D11DeviceContext *c, double t, float a) { stoy_frame(c, &g_nebula, t, a); }
static void nebula_cleanup(void) { stoy_cleanup(&g_nebula); }

// ----------------------------- SHOWCASE scene -------------------------------
// A full lit/shadowed/reflective scene: three animated SDF objects (a bouncing
// sphere, an orbiting sphere, a spinning rounded box) on a reflective checkered
// floor, with Phong lighting, soft ray-marched shadows, and a fresnel floor
// reflection (one bounce), under a slowly orbiting camera. Reflections + shadows
// fall out of ray-marching for free (extra rays) - no render targets needed.
static const char *kShowcaseHLSL =
    AIO_STOY_HEADER
    "float2 rot2(float2 v,float a){float s=sin(a),c=cos(a);return float2(v.x*c-v.y*s,v.x*s+v.y*c);}\n"
    "float sdSph(float3 p,float r){return length(p)-r;}\n"
    "float sdBox(float3 p,float3 b){float3 q=abs(p)-b;return length(max(q,0.0))+min(max(q.x,max(q.y,q.z)),0.0);}\n"
    "float2 mapS(float3 p){float t=iTime;\n"
    "  float2 res=float2(p.y+1.0,0.0);\n"
    "  float3 a=float3(sin(t*0.9)*1.6,abs(sin(t*1.6))*1.3-0.45,cos(t*0.7)*1.6);\n"
    "  float d=sdSph(p-a,0.5);if(d<res.x)res=float2(d,1.0);\n"
    "  float3 b=float3(cos(t*1.1)*2.2,sin(t*0.9)*0.4+0.1,sin(t*1.1)*2.2);\n"
    "  d=sdSph(p-b,0.4);if(d<res.x)res=float2(d,2.0);\n"
    "  float3 q=p;q.xz=rot2(q.xz,t*0.6);\n"
    "  d=sdBox(q,float3(0.45,0.45,0.45))-0.08;if(d<res.x)res=float2(d,3.0);\n"
    "  return res;}\n"
    "float3 nrm(float3 p){float2 e=float2(0.0015,0.0);\n"
    "  return normalize(float3(mapS(p+e.xyy).x-mapS(p-e.xyy).x,mapS(p+e.yxy).x-mapS(p-e.yxy).x,mapS(p+e.yyx).x-mapS(p-e.yyx).x));}\n"
    "float shadow(float3 ro,float3 rd){float res=1.0,t=0.04;\n"
    "  [loop]for(int i=0;i<36;i++){float h=mapS(ro+rd*t).x;if(h<0.001)return 0.0;res=min(res,12.0*h/t);t+=clamp(h,0.02,0.4);if(t>22.0)break;}return saturate(res);}\n"
    "float march(float3 ro,float3 rd,out float mid){float t=0.0;mid=-1.0;\n"
    "  [loop]for(int i=0;i<100;i++){float2 r=mapS(ro+rd*t);if(r.x<0.001){mid=r.y;return t;}t+=r.x;if(t>50.0)break;}return -1.0;}\n"
    "float3 matc(float mid,float3 p){if(mid<0.5){float c=fmod(floor(p.x)+floor(p.z),2.0);return lerp(float3(0.18,0.18,0.20),float3(0.55,0.55,0.58),c);}\n"
    "  if(mid<1.5)return float3(0.90,0.25,0.18);if(mid<2.5)return float3(0.20,0.55,0.92);return float3(0.95,0.78,0.20);}\n"
    "float3 sky(float3 rd){return lerp(float3(0.5,0.62,0.85),float3(0.12,0.16,0.30),saturate(rd.y*1.2+0.1));}\n"
    "float3 shade(float3 p,float3 rd,float mid){float3 n=nrm(p);float3 L=normalize(float3(4.0,6.5,3.0)-p);\n"
    "  float dif=saturate(dot(n,L));float sh=shadow(p+n*0.02,L);float3 base=matc(mid,p);\n"
    "  float3 H=normalize(L-rd);float spec=pow(saturate(dot(n,H)),48.0)*sh;\n"
    "  float3 amb=base*0.18*(0.5+0.5*n.y);return base*dif*sh+amb+spec*0.6;}\n"
    "float4 PSMain(VSOut inp):SV_TARGET{float2 uv=inp.ndc;uv.x*=iAspect;\n"
    "  float a=iTime*0.18;float3 ro=float3(sin(a)*5.0,2.6,cos(a)*5.0);float3 ta=float3(0,0.2,0);\n"
    "  float3 fw=normalize(ta-ro),rt=normalize(cross(float3(0,1,0),fw)),up=cross(fw,rt);\n"
    "  float3 rd=normalize(uv.x*rt+uv.y*up+1.8*fw);\n"
    "  float mid;float t=march(ro,rd,mid);float3 col;\n"
    "  if(t>0.0){float3 p=ro+rd*t;col=shade(p,rd,mid);\n"
    "    if(mid<0.5){float3 n=nrm(p);float3 r=reflect(rd,n);float mid2;float t2=march(p+n*0.03,r,mid2);\n"
    "      float3 refl=(t2>0.0)?shade(p+r*t2,r,mid2):sky(r);\n"
    "      float fres=0.04+0.5*pow(1.0-saturate(dot(n,-rd)),5.0);col=lerp(col,refl,saturate(fres*1.4));}\n"
    "    col=lerp(col,sky(rd),1.0-exp(-0.0009*t*t*t));\n"
    "  }else col=sky(rd);\n"
    "  col=pow(saturate(col),1.0/2.2);return float4(col,1.0);}\n";

static StoyState g_show;
static int show_init(ID3D11Device *d, ID3D11DeviceContext *c, int w, int h) {
    (void)c; (void)w; (void)h; return stoy_init(d, &g_show, kShowcaseHLSL);
}
static void show_frame(ID3D11DeviceContext *c, double t, float a) { stoy_frame(c, &g_show, t, a); }
static void show_cleanup(void) { stoy_cleanup(&g_show); }

// ------------------------------- SPACE scene --------------------------------
// A procedural planet (ocean/land/snow + a fresnel atmosphere rim-glow), three
// orbiting noise-displaced asteroids, and a chrome sphere that reflects the
// scene, sun-lit with soft shadows over a starfield + faint nebula. Reflections,
// shadows, lighting and moving (orbiting) objects, space-themed.
static const char *kSpaceHLSL =
    AIO_STOY_HEADER
    "float h31(float3 p){p=frac(p*0.3183099+0.1);p*=17.0;return frac(p.x*p.y*p.z*(p.x+p.y+p.z));}\n"
    "float n3(float3 p){float3 i=floor(p),f=frac(p);f=f*f*(3.0-2.0*f);\n"
    "  return lerp(lerp(lerp(h31(i),h31(i+float3(1,0,0)),f.x),lerp(h31(i+float3(0,1,0)),h31(i+float3(1,1,0)),f.x),f.y),\n"
    "              lerp(lerp(h31(i+float3(0,0,1)),h31(i+float3(1,0,1)),f.x),lerp(h31(i+float3(0,1,1)),h31(i+float3(1,1,1)),f.x),f.y),f.z);}\n"
    // 6-octave fbm - only ever evaluated at the hit point / background, never in the SDF.
    "float fbm(float3 p){float s=0.0,a=0.5;[unroll]for(int i=0;i<6;i++){s+=a*n3(p);p=p*2.03+0.1;a*=0.5;}return s;}\n"
    "float3 astP(int i){float t=iTime*0.25+i*2.094;float r=3.6+i*0.7;return float3(cos(t)*r,sin(t*0.6+i)*1.0,sin(t)*r);}\n"
    // SMOOTH spheres only -> fast marching; all realism is in the shading below.
    "float2 mapSp(float3 p){float2 res=float2(length(p)-2.0,0.0);\n"
    "  [unroll]for(int i=0;i<3;i++){float d=length(p-astP(i))-0.4;if(d<res.x)res=float2(d,1.0);}\n"
    "  float3 cp=float3(cos(iTime*0.4)*3.0,1.3,sin(iTime*0.4)*3.0);float d=length(p-cp)-0.55;if(d<res.x)res=float2(d,2.0);\n"
    "  return res;}\n"
    "float3 nrmSp(float3 p){float2 e=float2(0.004,0.0);return normalize(float3(mapSp(p+e.xyy).x-mapSp(p-e.xyy).x,mapSp(p+e.yxy).x-mapSp(p-e.yxy).x,mapSp(p+e.yyx).x-mapSp(p-e.yyx).x));}\n"
    "float shSp(float3 ro,float3 rd){float res=1.0,t=0.1;[loop]for(int i=0;i<22;i++){float h=mapSp(ro+rd*t).x;if(h<0.003)return 0.0;res=min(res,14.0*h/t);t+=clamp(h,0.06,0.8);if(t>11.0)break;}return saturate(res);}\n"
    "float marchSp(float3 ro,float3 rd,out float mid){float t=0.0;mid=-1.0;[loop]for(int i=0;i<90;i++){float2 r=mapSp(ro+rd*t);if(r.x<0.002){mid=r.y;return t;}t+=r.x;if(t>40.0)break;}return -1.0;}\n"
    "float3 sunDir(){return normalize(float3(0.85,0.32,-0.42));}\n"
    "float elev(float3 d){float e=fbm(d*3.1+11.0);return 0.5+(e-0.5)*1.4;}\n"
    "float3 biome(float h,float lat){float ice=smoothstep(0.60,0.80,lat);\n"
    "  float3 ocean=lerp(float3(0.01,0.04,0.16),float3(0.03,0.28,0.42),smoothstep(0.30,0.485,h));\n"
    "  float3 land=lerp(float3(0.62,0.57,0.40),float3(0.13,0.34,0.11),smoothstep(0.495,0.54,h));\n"
    "  land=lerp(land,float3(0.06,0.20,0.05),smoothstep(0.55,0.62,h));\n"
    "  land=lerp(land,float3(0.33,0.29,0.25),smoothstep(0.66,0.74,h));\n"
    "  land=lerp(land,float3(0.92,0.94,0.97),smoothstep(0.78,0.86,h));\n"
    "  float3 c=lerp(ocean,land,smoothstep(0.496,0.504,h));return lerp(c,float3(0.93,0.95,0.99),ice);}\n"
    "float clouds(float3 d){return smoothstep(0.52,0.74,fbm(d*3.4+float3(iTime*0.012,0.0,iTime*0.008)));}\n"
    "float3 surfCol(float3 p,float3 n,float3 rd,float3 L){float dif0=dot(n,L);float sh=shSp(p+n*0.04,L);\n"
    "  float3 d=normalize(p);float h=elev(d);float lat=abs(d.y);float isOcean=1.0-smoothstep(0.496,0.504,h);\n"
    "  float e=0.013;float3 gr=float3(elev(d+float3(e,0,0)),elev(d+float3(0,e,0)),elev(d+float3(0,0,e)))-h;\n"
    "  float3 tang=gr-n*dot(gr,n);float3 nn=normalize(n-tang*(1.0-isOcean)*7.0);\n"
    "  float dif=saturate(dot(nn,L));float3 base=biome(h,lat);\n"
    "  float coast=1.0-smoothstep(0.0,0.009,abs(h-0.5));base=lerp(base,float3(0.80,0.80,0.70),coast*0.45);\n"
    "  float cl=clouds(d);base=lerp(base,float3(1.0,1.0,1.0),cl*0.85)*(1.0-cl*0.45);\n"
    "  float3 col=base*(0.05+1.15*dif*sh);\n"
    "  float3 H=normalize(L-rd);float glint=pow(saturate(dot(nn,H)),250.0)*sh*isOcean*(1.0-cl);col+=float3(1.0,0.95,0.8)*glint*3.0;\n"
    "  float term=saturate(smoothstep(-0.3,0.1,dif0)-smoothstep(0.1,0.45,dif0));col+=float3(1.0,0.45,0.15)*term*0.30;\n"
    "  float fres=pow(1.0-saturate(dot(n,-rd)),3.0);col+=float3(0.30,0.55,1.0)*fres*saturate(dif0+0.2)*1.3;\n"
    "  float night=smoothstep(0.05,-0.18,dif0);float city=step(0.975,h31(floor(d*55.0)))*(1.0-isOcean)*(1.0-cl);\n"
    "  col+=float3(1.0,0.8,0.4)*city*night*1.6+float3(0.01,0.02,0.05)*night;return col;}\n"
    "float3 astCol(float3 p,float3 n,float3 rd,float3 L){float dif=saturate(dot(n,L));float sh=shSp(p+n*0.05,L);\n"
    "  float3 base=float3(0.28,0.25,0.22)*(0.45+0.7*fbm(p*5.0));float3 H=normalize(L-rd);float spec=pow(saturate(dot(n,H)),14.0)*sh;\n"
    "  return base*(0.04+1.1*dif*sh)+spec*0.25;}\n"
    "float3 bg(float3 ro,float3 rd,float3 L){float3 g=floor(rd*180.0);float hh=h31(g);float st=(hh>0.99)?pow((hh-0.99)/0.01,3.0):0.0;\n"
    "  float3 col=float3(0.008,0.007,0.018)+st*float3(0.9,0.95,1.0);\n"
    "  float sn=dot(rd,L);col+=float3(1.0,0.96,0.85)*(smoothstep(0.9994,0.9999,sn)*12.0+pow(saturate(sn),140.0)*0.7);\n"
    "  float tc=-dot(ro,rd);float3 cp=ro+rd*max(tc,0.0);float cd=length(cp);\n"
    "  float halo=smoothstep(2.55,2.02,cd)*smoothstep(1.98,2.05,cd);float lit=saturate(dot(normalize(cp),L)*0.6+0.4);\n"
    "  col+=float3(0.20,0.40,0.95)*halo*lit*1.4;return col;}\n"
    "float4 PSMain(VSOut inp):SV_TARGET{float2 uv=inp.ndc;uv.x*=iAspect;\n"
    "  float a=iTime*0.1;float3 ro=float3(sin(a)*6.5,1.5,cos(a)*6.5);\n"
    "  float3 fw=normalize(-ro),rt=normalize(cross(float3(0,1,0),fw)),up=cross(fw,rt);float3 rd=normalize(uv.x*rt+uv.y*up+1.8*fw);\n"
    "  float3 L=sunDir();float mid;float t=marchSp(ro,rd,mid);float3 col;\n"
    "  if(t>0.0){float3 p=ro+rd*t;float3 n=nrmSp(p);\n"
    "    if(mid<0.5)col=surfCol(p,n,rd,L);\n"
    "    else if(mid<1.5)col=astCol(p,n,rd,L);\n"
    "    else{float3 r=reflect(rd,n);float m2;float t2=marchSp(p+n*0.05,r,m2);float3 refl;\n"
    "      if(t2>0.0){float3 pr=p+r*t2;float3 nr=nrmSp(pr);refl=(m2<0.5)?surfCol(pr,nr,r,L):astCol(pr,nr,r,L);}else refl=bg(p,r,L);\n"
    "      float3 H=normalize(L-rd);float gl=pow(saturate(dot(n,H)),350.0)*6.0;col=refl*0.9+float3(1.0,0.96,0.85)*gl;}\n"
    "  }else col=bg(ro,rd,L);\n"
    "  col*=1.15;col=(col*(2.51*col+0.03))/(col*(2.43*col+0.59)+0.14);\n"
    "  col=pow(saturate(col),1.0/2.2);return float4(col,1.0);}\n";

static StoyState g_space;
static int space_init(ID3D11Device *d, ID3D11DeviceContext *c, int w, int h) {
    (void)c; (void)w; (void)h; return stoy_init(d, &g_space, kSpaceHLSL);
}
static void space_frame(ID3D11DeviceContext *c, double t, float a) { stoy_frame(c, &g_space, t, a); }
static void space_cleanup(void) { stoy_cleanup(&g_space); }

// ----------------------------- CITYSCAPE scene ------------------------------
// A night Blade-Runner city: a domain-repeated grid of towers (hashed heights)
// with emissive window grids + neon, a WET REFLECTIVE STREET that mirrors the
// lights, volumetric night haze, and a camera gliding down the avenue. Reflections
// (wet road), shadows (towers occlude), lighting (windows + sky), moving (forward
// camera), city-themed.
static const char *kCityHLSL =
    AIO_STOY_HEADER
    "float h21(float2 p){p=frac(p*float2(127.1,311.7));p+=dot(p,p+34.5);return frac(p.x*p.y);}\n"
    "float h11(float n){return frac(sin(n)*43758.5453);}\n"
    "float sdBox(float3 p,float3 b){float3 q=abs(p)-b;return length(max(q,0.0))+min(max(q.x,max(q.y,q.z)),0.0);}\n"
    "float2 mapCity(float3 p){float2 res=float2(p.y,0.0);\n"
    "  float cs=7.0;float2 cell=floor(p.xz/cs+0.5);float2 local=p.xz-cell*cs;\n"
    "  float ht=3.0+h21(cell)*15.0;float fw=2.0+h21(cell+5.3)*0.7;\n"
    "  float3 q=float3(local.x,p.y-ht*0.5,local.y);float bld=sdBox(q,float3(fw,ht*0.5,fw));\n"
    "  if(bld<res.x)res=float2(bld,1.0);return res;}\n"
    "float3 nrmCity(float3 p){float2 e=float2(0.01,0.0);return normalize(float3(mapCity(p+e.xyy).x-mapCity(p-e.xyy).x,mapCity(p+e.yxy).x-mapCity(p-e.yxy).x,mapCity(p+e.yyx).x-mapCity(p-e.yyx).x));}\n"
    "float marchCity(float3 ro,float3 rd,out float mid){float t=0.0;mid=-1.0;[loop]for(int i=0;i<130;i++){float2 r=mapCity(ro+rd*t);if(r.x<0.002*t+0.002){mid=r.y;return t;}t+=r.x*0.85;if(t>90.0)break;}return -1.0;}\n"
    "float3 windows(float3 p,float3 n){float2 uv=(abs(n.x)>0.5)?p.zy:p.xy;\n"
    "  float2 g=floor(uv*1.3);float2 f=frac(uv*1.3);\n"
    "  float win=step(0.12,f.x)*step(f.x,0.88)*step(0.12,f.y)*step(f.y,0.88);\n"
    "  float lit=step(0.58,h11(dot(g,float2(12.3,7.7))));\n"
    "  float3 wc=lerp(float3(1.0,0.68,0.28),float3(0.35,0.8,1.0),h11(dot(g,float2(3.1,9.7))));\n"
    "  return wc*win*lit*2.4;}\n"
    "float3 cityShade(float3 p,float3 n,float3 rd,float mid){\n"
    "  if(mid<0.5)return float3(0.006,0.006,0.010);\n"
    "  float3 base=float3(0.016,0.016,0.024);\n"
    "  float3 win=(abs(n.y)<0.5)?windows(p,n):float3(0.0,0.0,0.0);\n"
    "  float3 L=normalize(float3(0.3,0.85,0.25));float dif=saturate(dot(n,L))*0.06;\n"
    "  float fres=pow(1.0-saturate(dot(n,-rd)),5.0);\n"
    "  return base+base*dif+win+float3(0.05,0.035,0.11)*fres;}\n"
    "float4 PSMain(VSOut inp):SV_TARGET{float2 uv=inp.ndc;uv.x*=iAspect;\n"
    "  float3 ro=float3(3.5,2.6+sin(iTime*0.3)*0.5,iTime*4.0);\n"
    "  float3 ta=ro+float3(sin(iTime*0.15)*0.3,-0.12,1.0);\n"
    "  float3 fw=normalize(ta-ro),rt=normalize(cross(float3(0,1,0),fw)),up=cross(fw,rt);float3 rd=normalize(uv.x*rt+uv.y*up+1.5*fw);\n"
    "  float3 fog=float3(0.012,0.008,0.022);\n"
    "  float mid;float t=marchCity(ro,rd,mid);float3 col;\n"
    "  if(t>0.0){float3 p=ro+rd*t;float3 n=nrmCity(p);col=cityShade(p,n,rd,mid);\n"
    "    if(mid<0.5){float3 r=reflect(rd,n);float m2;float t2=marchCity(p+n*0.02,r,m2);\n"
    "      float3 refl=(t2>0.0)?cityShade(p+r*t2,nrmCity(p+r*t2),r,m2):(fog+float3(0.04,0.02,0.07)*pow(saturate(r.y+0.1),3.0));\n"
    "      float fres=0.15+0.85*pow(1.0-saturate(dot(n,-rd)),4.0);col=lerp(col,refl,fres*0.8);}\n"
    "    col=lerp(col,fog,1.0-exp(-0.02*t));\n"
    "  }else col=fog+float3(0.05,0.025,0.09)*pow(saturate(rd.y+0.1),3.0);\n"
    "  col=col/(col+0.85);col=pow(saturate(col),1.0/2.2);return float4(col,1.0);}\n";

static StoyState g_city;
static int city_init(ID3D11Device *d, ID3D11DeviceContext *c, int w, int h) {
    (void)c; (void)w; (void)h; return stoy_init(d, &g_city, kCityHLSL);
}
static void city_frame(ID3D11DeviceContext *c, double t, float a) { stoy_frame(c, &g_city, t, a); }
static void city_cleanup(void) { stoy_cleanup(&g_city); }

// ========================= GEOMETRY-SHADER scene ===========================
// A mesh exploder: the geometry shader receives each triangle, computes its face
// normal, pushes the whole triangle outward along that normal by a time-varying
// amount, flat-shades it (diffuse + specular from the face normal) and emits it.
// This is the only scene that exercises the geometry-shader STAGE (DXVK implements
// GS on top of Vulkan; on Turnip/Adreno it is emulated, so this doubles as a GS
// capability + perf probe). Source mesh = a procedurally built UV-sphere triangle
// list. CULL_NONE so exploded shards render from both sides.
static const char *kGSHLSL =
    "cbuffer CB : register(b0) { row_major float4x4 world; row_major float4x4 viewProj; float4 misc; };\n"
    "struct VSOut { float3 wpos : TEXCOORD0; };\n"
    "VSOut VSMain(float3 pos : POSITION) { VSOut o; o.wpos = mul(float4(pos,1.0), world).xyz; return o; }\n"
    "struct GSOut { float4 pos : SV_POSITION; float3 col : COLOR; };\n"
    "[maxvertexcount(3)]\n"
    "void GSMain(triangle VSOut i[3], inout TriangleStream<GSOut> stream) {\n"
    "  float3 n = normalize(cross(i[1].wpos - i[0].wpos, i[2].wpos - i[0].wpos));\n"
    "  float3 L = normalize(float3(0.5,0.8,0.6));\n"
    "  float3 base = 0.5 + 0.5*cos(float3(0.0,2.0,4.0) + misc.y*0.3);\n"
    "  float dif = saturate(dot(n,L));\n"
    "  [unroll] for (int k=0;k<3;k++){\n"
    "    GSOut o; float3 wp = i[k].wpos + n*misc.x;\n"
    "    o.pos = mul(float4(wp,1.0), viewProj);\n"
    "    float3 V = normalize(float3(0.0,0.0,7.0) - wp); float3 H = normalize(L+V);\n"
    "    float spec = pow(saturate(dot(n,H)), 40.0);\n"
    "    o.col = base*(0.25 + dif) + spec*0.6; stream.Append(o);\n"
    "  }\n"
    "}\n"
    "float4 PSMain(GSOut i) : SV_TARGET { return float4(i.col, 1.0); }\n";

typedef struct {
    float pos[3];
} PosVtx;

typedef struct {
    float world[16];
    float viewProj[16];
    float misc[4];
} GSCB;

static struct {
    ID3D11VertexShader *vs;
    ID3D11GeometryShader *gs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vbo, *cbo;
    ID3D11RasterizerState *rs;
    int vcount;
} g_gs;

static int gs_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *vsb = compile_hlsl(kGSHLSL, "VSMain", "vs_4_0");
    ID3DBlob *gsb = compile_hlsl(kGSHLSL, "GSMain", "gs_4_0");
    ID3DBlob *psb = compile_hlsl(kGSHLSL, "PSMain", "ps_4_0");
    if (!vsb || !gsb || !psb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_gs.vs);
    HRESULT hg = ID3D11Device_CreateGeometryShader(dev, ID3D10Blob_GetBufferPointer(gsb),
                                                   ID3D10Blob_GetBufferSize(gsb), NULL, &g_gs.gs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_gs.ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, il, 1, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &g_gs.layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(gsb);
    ID3D10Blob_Release(psb);
    if (FAILED(hg) || !g_gs.gs) {
        fail_box("Geometry shaders (gs_4_0) are not available on this Direct3D 11 device.");
        return 1;
    }

    // Build a UV-sphere triangle list (positions only; the GS derives face normals).
    const int rings = 16, sectors = 32;
    const int vc = rings * sectors * 6;
    PosVtx *verts = (PosVtx *)malloc((size_t)vc * sizeof(PosVtx));
    if (!verts) return 1;
    int n = 0;
    for (int ri = 0; ri < rings; ri++) {
        float t0 = (float)ri / rings * 3.14159265f;
        float t1 = (float)(ri + 1) / rings * 3.14159265f;
        for (int si = 0; si < sectors; si++) {
            float p0 = (float)si / sectors * 6.2831853f;
            float p1 = (float)(si + 1) / sectors * 6.2831853f;
#define AIO_SP(th, ph, dst) \
    do { (dst)[0] = sinf(th) * cosf(ph); (dst)[1] = cosf(th); (dst)[2] = sinf(th) * sinf(ph); } while (0)
            float a[3], b[3], c[3], d[3];
            AIO_SP(t0, p0, a);
            AIO_SP(t1, p0, b);
            AIO_SP(t1, p1, c);
            AIO_SP(t0, p1, d);
#undef AIO_SP
            memcpy(verts[n++].pos, a, 12);
            memcpy(verts[n++].pos, b, 12);
            memcpy(verts[n++].pos, c, 12);
            memcpy(verts[n++].pos, a, 12);
            memcpy(verts[n++].pos, c, 12);
            memcpy(verts[n++].pos, d, 12);
        }
    }
    g_gs.vcount = n;
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = (UINT)(n * sizeof(PosVtx));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = verts;
    ID3D11Device_CreateBuffer(dev, &vbd, &sr, &g_gs.vbo);
    free(verts);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(GSCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_gs.cbo);

    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // shards seen from behind must still render
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &g_gs.rs);
    return (g_gs.vs && g_gs.gs && g_gs.ps && g_gs.layout && g_gs.vbo && g_gs.cbo && g_gs.rs) ? 0 : 1;
}

static void gs_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    Mat4 world = mat_rotate(0.3f, 1.0f, 0.2f, (float)t * 0.6f);
    Mat4 viewProj = mat_mul(mat_translate(0.0f, 0.0f, -7.0f),
                            mat_perspective(0.6f, aspect, 0.1f, 100.0f));
    float explode = (sinf((float)t * 1.2f) * 0.5f + 0.5f) * 1.8f;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_gs.cbo, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        GSCB *cb = (GSCB *)m.pData;
        memcpy(cb->world, world.m, sizeof(world.m));
        memcpy(cb->viewProj, viewProj.m, sizeof(viewProj.m));
        cb->misc[0] = explode;
        cb->misc[1] = (float)t;
        cb->misc[2] = cb->misc[3] = 0.0f;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_gs.cbo, 0);
    }
    UINT stride = sizeof(PosVtx), offset = 0;
    ID3D11DeviceContext_RSSetState(ctx, g_gs.rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_gs.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_gs.vbo, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(ctx, g_gs.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_gs.cbo);
    ID3D11DeviceContext_GSSetShader(ctx, g_gs.gs, NULL, 0);
    ID3D11DeviceContext_GSSetConstantBuffers(ctx, 0, 1, &g_gs.cbo);
    ID3D11DeviceContext_PSSetShader(ctx, g_gs.ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, (UINT)g_gs.vcount, 0);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);  // unbind GS after the draw
}

static void gs_cleanup(void) {
    if (g_gs.rs) ID3D11RasterizerState_Release(g_gs.rs);
    if (g_gs.cbo) ID3D11Buffer_Release(g_gs.cbo);
    if (g_gs.vbo) ID3D11Buffer_Release(g_gs.vbo);
    if (g_gs.layout) ID3D11InputLayout_Release(g_gs.layout);
    if (g_gs.ps) ID3D11PixelShader_Release(g_gs.ps);
    if (g_gs.gs) ID3D11GeometryShader_Release(g_gs.gs);
    if (g_gs.vs) ID3D11VertexShader_Release(g_gs.vs);
    memset(&g_gs, 0, sizeof(g_gs));
}

// Shared 144-byte constant buffer for the lit PN scenes: an MVP matrix, a second
// matrix (world for cel, world-view for matcap), and a spare float4.
typedef struct {
    float mvp[16];
    float m2[16];
    float v4[4];
} PNCB;

// Creates the POSITION+NORMAL input layout from a VS blob (offsets 0 and 12).
static void make_pn_layout(ID3D11Device *dev, ID3DBlob *vsb, ID3D11InputLayout **out) {
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), out);
}

// Creates a CULL_NONE solid rasterizer state (shared default for the lit scenes).
static void make_cullnone_rs(ID3D11Device *dev, ID3D11RasterizerState **out) {
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, out);
}

// ============================== CEL-SHADING scene ===========================
// Toon shading: the diffuse term dot(N,L) is quantised into a few flat bands and
// a dark silhouette outline is drawn where the surface turns away from the camera.
// A classic non-photoreal look; tests banded lighting + a rim/outline test. Mesh
// = a torus (the bands read clearly on its curvature).
static const char *kCelHLSL =
    "cbuffer CB : register(b0){ row_major float4x4 mvp; row_major float4x4 world; float4 eye; };\n"
    "struct VSIn{ float3 pos:POSITION; float3 nrm:NORMAL; };\n"
    "struct VSOut{ float4 pos:SV_POSITION; float3 wn:TEXCOORD0; float3 wp:TEXCOORD1; };\n"
    "VSOut VSMain(VSIn i){ VSOut o; o.pos=mul(float4(i.pos,1.0),mvp);\n"
    "  o.wn=mul(float4(i.nrm,0.0),world).xyz; o.wp=mul(float4(i.pos,1.0),world).xyz; return o; }\n"
    "float4 PSMain(VSOut i):SV_TARGET{\n"
    "  float3 N=normalize(i.wn); float3 L=normalize(float3(0.6,0.8,0.5));\n"
    "  float d=saturate(dot(N,L));\n"
    "  float3 c1=float3(0.10,0.16,0.40), c2=float3(0.16,0.42,0.80);\n"
    "  float3 c3=float3(0.45,0.72,0.95), c4=float3(0.92,0.97,1.0);\n"
    "  float3 col = d<0.25?c1 : (d<0.5?c2 : (d<0.78?c3 : c4));\n"
    "  float3 V=normalize(eye.xyz - i.wp);\n"
    "  if (dot(N,V) < 0.3) col=float3(0.02,0.02,0.05);\n"
    "  return float4(col,1.0);\n"
    "}\n";

static struct {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vbo, *cbo;
    ID3D11RasterizerState *rs;
    int vcount;
} g_cel;

static int cel_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *vsb = compile_hlsl(kCelHLSL, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(kCelHLSL, "PSMain", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_cel.vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_cel.ps);
    make_pn_layout(dev, vsb, &g_cel.layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    const int nring = 48, nside = 24;
    int vc = nring * nside * 6;
    PNVertex *verts = (PNVertex *)malloc((size_t)vc * sizeof(PNVertex));
    if (!verts) return 1;
    g_cel.vcount = build_torus_pn(verts, nring, nside, 1.15f, 0.45f);
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = (UINT)(g_cel.vcount * sizeof(PNVertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = verts;
    ID3D11Device_CreateBuffer(dev, &vbd, &sr, &g_cel.vbo);
    free(verts);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(PNCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_cel.cbo);
    make_cullnone_rs(dev, &g_cel.rs);
    return (g_cel.vs && g_cel.ps && g_cel.layout && g_cel.vbo && g_cel.cbo && g_cel.rs) ? 0 : 1;
}

static void cel_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    Mat4 world = mat_rotate(0.4f, 1.0f, 0.25f, (float)t * 0.6f);
    Mat4 mvp = mat_mul(mat_mul(world, mat_translate(0.0f, 0.0f, -4.0f)),
                       mat_perspective(0.7f, aspect, 0.1f, 100.0f));
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_cel.cbo, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        PNCB *cb = (PNCB *)m.pData;
        memcpy(cb->mvp, mvp.m, sizeof(mvp.m));
        memcpy(cb->m2, world.m, sizeof(world.m));
        cb->v4[0] = 0.0f;
        cb->v4[1] = 0.0f;
        cb->v4[2] = 4.0f;  // eye at +z (camera pulled back via -z translate)
        cb->v4[3] = 0.0f;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_cel.cbo, 0);
    }
    UINT stride = sizeof(PNVertex), offset = 0;
    ID3D11DeviceContext_RSSetState(ctx, g_cel.rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_cel.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_cel.vbo, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(ctx, g_cel.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_cel.cbo);
    ID3D11DeviceContext_PSSetShader(ctx, g_cel.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_cel.cbo);
    ID3D11DeviceContext_Draw(ctx, (UINT)g_cel.vcount, 0);
}

static void cel_cleanup(void) {
    if (g_cel.rs) ID3D11RasterizerState_Release(g_cel.rs);
    if (g_cel.cbo) ID3D11Buffer_Release(g_cel.cbo);
    if (g_cel.vbo) ID3D11Buffer_Release(g_cel.vbo);
    if (g_cel.layout) ID3D11InputLayout_Release(g_cel.layout);
    if (g_cel.ps) ID3D11PixelShader_Release(g_cel.ps);
    if (g_cel.vs) ID3D11VertexShader_Release(g_cel.vs);
    memset(&g_cel, 0, sizeof(g_cel));
}

// ============================ MATCAP / ENV-MAP scene ========================
// Material-capture (matcap) shading: the view-space surface normal indexes a
// lit-sphere image to fake reflections/lighting that stay locked to the camera.
// Here the matcap is generated PROCEDURALLY in the shader (a chrome-ball look:
// bright centre, an offset specular hotspot, a rim) so there is NO texture asset.
// Mesh = a sphere; as it spins the matcap gives a reflective-metal illusion.
static const char *kMatcapHLSL =
    "cbuffer CB : register(b0){ row_major float4x4 mvp; row_major float4x4 worldView; float4 pad; };\n"
    "struct VSIn{ float3 pos:POSITION; float3 nrm:NORMAL; };\n"
    "struct VSOut{ float4 pos:SV_POSITION; float3 vn:TEXCOORD0; };\n"
    "VSOut VSMain(VSIn i){ VSOut o; o.pos=mul(float4(i.pos,1.0),mvp);\n"
    "  o.vn=normalize(mul(float4(i.nrm,0.0),worldView).xyz); return o; }\n"
    "float3 matcap(float2 n){ float r=length(n);\n"
    "  float3 base=lerp(float3(0.75,0.82,0.95), float3(0.12,0.16,0.26), saturate(r));\n"
    "  float2 hp=n-float2(0.32,0.42); float spec=exp(-dot(hp,hp)*7.0);\n"
    "  float rim=smoothstep(0.7,1.0,r);\n"
    "  return base + spec*1.3 + rim*float3(0.10,0.16,0.30); }\n"
    "float4 PSMain(VSOut i):SV_TARGET{ float3 n=normalize(i.vn); return float4(matcap(n.xy),1.0); }\n";

static struct {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vbo, *cbo;
    ID3D11RasterizerState *rs;
    int vcount;
} g_mat;

static int mat_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *vsb = compile_hlsl(kMatcapHLSL, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(kMatcapHLSL, "PSMain", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_mat.vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_mat.ps);
    make_pn_layout(dev, vsb, &g_mat.layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    const int rings = 32, sectors = 64;
    int vc = rings * sectors * 6;
    PNVertex *verts = (PNVertex *)malloc((size_t)vc * sizeof(PNVertex));
    if (!verts) return 1;
    g_mat.vcount = build_sphere_pn(verts, rings, sectors);
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = (UINT)(g_mat.vcount * sizeof(PNVertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = verts;
    ID3D11Device_CreateBuffer(dev, &vbd, &sr, &g_mat.vbo);
    free(verts);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(PNCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_mat.cbo);
    make_cullnone_rs(dev, &g_mat.rs);
    return (g_mat.vs && g_mat.ps && g_mat.layout && g_mat.vbo && g_mat.cbo && g_mat.rs) ? 0 : 1;
}

static void mat_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    Mat4 world = mat_rotate(0.3f, 1.0f, 0.15f, (float)t * 0.5f);
    Mat4 view = mat_translate(0.0f, 0.0f, -3.2f);
    Mat4 worldView = mat_mul(world, view);
    Mat4 mvp = mat_mul(worldView, mat_perspective(0.7f, aspect, 0.1f, 100.0f));
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_mat.cbo, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        PNCB *cb = (PNCB *)m.pData;
        memcpy(cb->mvp, mvp.m, sizeof(mvp.m));
        memcpy(cb->m2, worldView.m, sizeof(worldView.m));
        cb->v4[0] = cb->v4[1] = cb->v4[2] = cb->v4[3] = 0.0f;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_mat.cbo, 0);
    }
    UINT stride = sizeof(PNVertex), offset = 0;
    ID3D11DeviceContext_RSSetState(ctx, g_mat.rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, g_mat.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_mat.vbo, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(ctx, g_mat.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_mat.cbo);
    ID3D11DeviceContext_PSSetShader(ctx, g_mat.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_mat.cbo);
    ID3D11DeviceContext_Draw(ctx, (UINT)g_mat.vcount, 0);
}

static void mat_cleanup(void) {
    if (g_mat.rs) ID3D11RasterizerState_Release(g_mat.rs);
    if (g_mat.cbo) ID3D11Buffer_Release(g_mat.cbo);
    if (g_mat.vbo) ID3D11Buffer_Release(g_mat.vbo);
    if (g_mat.layout) ID3D11InputLayout_Release(g_mat.layout);
    if (g_mat.ps) ID3D11PixelShader_Release(g_mat.ps);
    if (g_mat.vs) ID3D11VertexShader_Release(g_mat.vs);
    memset(&g_mat, 0, sizeof(g_mat));
}

// ============================= DRAW-STRESS scene ============================
// An NxN grid of small cubes, each issued as its OWN draw call (NOT instanced)
// with its own per-object constant-buffer update. The bottleneck is CPU/driver
// submission overhead, not the GPU - a different axis from every other scene
// (which are GPU-bound). N cbuffer updates + N draws per frame (N = --draws, the
// grid is sized to fill the view at any count). Reuses
// the spin shader (POSITION+COLOR cube).
static int g_ds_draws = 1024;  // draw count for the draw-stress scene (--draws N)

void aio_d3d11_set_draws(int n) {
    if (n < 1) n = 1;
    if (n > 8192) n = 8192;
    g_ds_draws = n;
}

static struct {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vbo, *cbo;
} g_ds;

static int ds_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    // Per-count benchmark label so each draw count records its own result.
    char lbl[40];
    snprintf(lbl, sizeof(lbl), "D3D11 Draw %d", g_ds_draws);
    aio_bench_set_label(lbl);
    ID3DBlob *vsb = compile_hlsl(kSpinHLSL, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(kSpinHLSL, "PSMain", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_ds.vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_ds.ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &g_ds.layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);

    ColVertex verts[36];
    build_color_cube(verts);
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = sizeof(verts);
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sr;
    memset(&sr, 0, sizeof(sr));
    sr.pSysMem = verts;
    ID3D11Device_CreateBuffer(dev, &vbd, &sr, &g_ds.vbo);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(Mat4);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_ds.cbo);
    return (g_ds.vs && g_ds.ps && g_ds.layout && g_ds.vbo && g_ds.cbo) ? 0 : 1;
}

static void ds_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    int count = g_ds_draws;
    int cols = (int)ceilf(sqrtf((float)count));
    if (cols < 1) cols = 1;
    int rows = (count + cols - 1) / cols;
    // Keep the grid filling the view at any count: spacing shrinks as count grows.
    const float spread = 9.0f;
    float sx = (cols > 1) ? (2.0f * spread) / (cols - 1) : 2.0f * spread;
    float sy = (rows > 1) ? (2.0f * spread) / (rows - 1) : 2.0f * spread;
    float step = (sx < sy) ? sx : sy;
    float scale = step * 0.42f;
    float ox = -((cols - 1) * step) * 0.5f, oy = -((rows - 1) * step) * 0.5f;
    Mat4 vp = mat_mul(mat_translate(0.0f, 0.0f, -18.0f), mat_perspective(1.0f, aspect, 0.1f, 100.0f));

    UINT stride = sizeof(ColVertex), offset = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, g_ds.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &g_ds.vbo, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(ctx, g_ds.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_ds.cbo);
    ID3D11DeviceContext_PSSetShader(ctx, g_ds.ps, NULL, 0);

    for (int i = 0; i < count; i++) {
        int gx = i % cols, gy = i / cols;
        float fx = ox + gx * step, fy = oy + gy * step;
        float ang = (float)t * 0.8f + (gx + gy) * 0.20f;
        Mat4 model = mat_mul(mat_mul(mat_scale(scale), mat_rotate(0.5f, 1.0f, 0.3f, ang)),
                             mat_translate(fx, fy, 0.0f));
        Mat4 mvp = mat_mul(model, vp);
        upload_mat(ctx, g_ds.cbo, mvp);            // one cbuffer update per object
        ID3D11DeviceContext_Draw(ctx, 36, 0);      // ...and one draw call per object
    }
}

static void ds_cleanup(void) {
    if (g_ds.cbo) ID3D11Buffer_Release(g_ds.cbo);
    if (g_ds.vbo) ID3D11Buffer_Release(g_ds.vbo);
    if (g_ds.layout) ID3D11InputLayout_Release(g_ds.layout);
    if (g_ds.ps) ID3D11PixelShader_Release(g_ds.ps);
    if (g_ds.vs) ID3D11VertexShader_Release(g_ds.vs);
    memset(&g_ds, 0, sizeof(g_ds));
}

// =============================== ATOMICS scene ==============================
// A histogram built entirely with GPU atomics: a compute shader scatters tens of
// thousands of threads into N bins via InterlockedAdd, then the bins are drawn as
// animated bars (read back as an SRV in the vertex shader, one instanced draw, no
// vertex buffer). Tests UAV atomics (InterlockedAdd), ClearUnorderedAccessViewUint
// and a structured-buffer SRV in the VS - things a translation layer (DXVK->Turnip)
// can mishandle, so it is a correctness probe as much as a perf one.
#define ATOM_BINS 64
#define ATOM_THREADS 65536
#define ATOM_GROUPS (ATOM_THREADS / 64)

static const char *kAtomHLSL =
    "cbuffer CB : register(b0){ float time; uint nthreads; uint nbins; float maxcount; };\n"
    "RWStructuredBuffer<uint> bins : register(u0);\n"
    "[numthreads(64,1,1)]\n"
    "void CSMain(uint3 id : SV_DispatchThreadID){\n"
    "  uint i = id.x; if (i >= nthreads) return;\n"
    "  float v = sin(i*0.0131 + time)*0.5 + 0.5;\n"
    "  v = frac(v + sin(i*0.00071 + time*0.6)*0.25 + 0.5);\n"
    "  uint b = (uint)(v * nbins); if (b >= nbins) b = nbins - 1;\n"
    "  InterlockedAdd(bins[b], 1);\n"
    "}\n"
    "StructuredBuffer<uint> binsR : register(t0);\n"
    "struct VSOut { float4 pos:SV_POSITION; float3 col:COLOR; };\n"
    "VSOut VSMain(uint vid:SV_VertexID, uint iid:SV_InstanceID){\n"
    "  float2 quad[6] = {float2(0,0),float2(1,0),float2(1,1),float2(0,0),float2(1,1),float2(0,1)};\n"
    "  float2 q = quad[vid];\n"
    "  float h = saturate((float)binsR[iid] / maxcount);\n"
    "  float bw = (2.0/nbins) * 0.82;\n"
    "  float cx = -1.0 + (iid + 0.5) * (2.0/nbins);\n"
    "  float x = cx + (q.x - 0.5)*bw;\n"
    "  float y = -0.92 + q.y * (h*1.7 + 0.02);\n"
    "  VSOut o; o.pos = float4(x, y, 0.0, 1.0);\n"
    "  o.col = 0.5 + 0.5*cos(float3(0.0,2.0,4.0) + h*3.0 + iid*0.05);\n"
    "  return o;\n"
    "}\n"
    "float4 PSMain(VSOut i):SV_TARGET { return float4(i.col, 1.0); }\n";

typedef struct {
    float time;
    unsigned nthreads, nbins;
    float maxcount;
} AtomCB;

static struct {
    ID3D11ComputeShader *cs;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *bins, *cb;
    ID3D11UnorderedAccessView *uav;
    ID3D11ShaderResourceView *srv;
    ID3D11RasterizerState *rs;
} g_atom;

static int atom_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, int w, int h) {
    (void)ctx;
    (void)w;
    (void)h;
    ID3DBlob *csb = compile_hlsl(kAtomHLSL, "CSMain", "cs_5_0");
    ID3DBlob *vsb = compile_hlsl(kAtomHLSL, "VSMain", "vs_5_0");
    ID3DBlob *psb = compile_hlsl(kAtomHLSL, "PSMain", "ps_5_0");
    if (!csb || !vsb || !psb) return 1;
    HRESULT hc = ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(csb),
                                                  ID3D10Blob_GetBufferSize(csb), NULL, &g_atom.cs);
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &g_atom.vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &g_atom.ps);
    ID3D10Blob_Release(csb);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    if (FAILED(hc) || !g_atom.cs) {
        fail_box("Compute shaders (cs_5_0) are not available on this Direct3D 11 device.");
        return 1;
    }

    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(unsigned) * ATOM_BINS;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(unsigned);
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &g_atom.bins);

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof(ud));
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = ATOM_BINS;
    ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)g_atom.bins, &ud, &g_atom.uav);

    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.NumElements = ATOM_BINS;
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_atom.bins, &sd, &g_atom.srv);

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(AtomCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_atom.cb);

    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;  // the bar quads are CCW; don't cull them
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &g_atom.rs);
    return (g_atom.cs && g_atom.vs && g_atom.ps && g_atom.bins && g_atom.uav && g_atom.srv &&
            g_atom.cb && g_atom.rs)
               ? 0
               : 1;
}

static void atom_frame(ID3D11DeviceContext *ctx, double t, float aspect) {
    (void)aspect;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_atom.cb, 0, D3D11_MAP_WRITE_DISCARD,
                                          0, &m))) {
        AtomCB *c = (AtomCB *)m.pData;
        c->time = (float)t;
        c->nthreads = ATOM_THREADS;
        c->nbins = ATOM_BINS;
        c->maxcount = (float)ATOM_THREADS / ATOM_BINS * 2.6f;
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_atom.cb, 0);
    }

    // Clear the bins, then scatter into them with atomics.
    UINT zero[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(ctx, g_atom.uav, zero);
    UINT initc = 0;
    ID3D11DeviceContext_CSSetShader(ctx, g_atom.cs, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &g_atom.cb);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &g_atom.uav, &initc);
    ID3D11DeviceContext_Dispatch(ctx, ATOM_GROUPS, 1, 1);

    // Unbind the UAV + compute shader before reading the bins as an SRV.
    ID3D11UnorderedAccessView *nu = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nu, &initc);
    ID3D11DeviceContext_CSSetShader(ctx, NULL, NULL, 0);

    // Draw the bins as bars (one instanced draw, no vertex buffer).
    ID3D11DeviceContext_RSSetState(ctx, g_atom.rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_atom.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &g_atom.cb);
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &g_atom.srv);
    ID3D11DeviceContext_PSSetShader(ctx, g_atom.ps, NULL, 0);
    ID3D11DeviceContext_DrawInstanced(ctx, 6, ATOM_BINS, 0, 0);

    ID3D11ShaderResourceView *ns = NULL;
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &ns);
}

static void atom_cleanup(void) {
    if (g_atom.rs) ID3D11RasterizerState_Release(g_atom.rs);
    if (g_atom.cb) ID3D11Buffer_Release(g_atom.cb);
    if (g_atom.srv) ID3D11ShaderResourceView_Release(g_atom.srv);
    if (g_atom.uav) ID3D11UnorderedAccessView_Release(g_atom.uav);
    if (g_atom.bins) ID3D11Buffer_Release(g_atom.bins);
    if (g_atom.ps) ID3D11PixelShader_Release(g_atom.ps);
    if (g_atom.vs) ID3D11VertexShader_Release(g_atom.vs);
    if (g_atom.cs) ID3D11ComputeShader_Release(g_atom.cs);
    memset(&g_atom, 0, sizeof(g_atom));
}

// ============================== scene registry ==============================
static const D3D11Scene kScenes[] = {
    {"spin", "D3D11 Cube", spin_init, spin_frame, spin_cleanup},
    {"textured", "D3D11 Textured", tex_init, tex_frame, tex_cleanup},
    {"instanced", "D3D11 Instanced", inst_init, inst_frame, inst_cleanup},
    {"tess", "D3D11 Tessellation", tess_init, tess_frame, tess_cleanup},
    {"compute", "D3D11 Compute Particles", comp_init, comp_frame, comp_cleanup},
    {"dolphin", "D3D11 Dolphin", dol_init, dol_frame, dol_cleanup},
    {"raymarch", "D3D11 Raymarch SDF", ray_init, ray_frame, ray_cleanup},
    {"ocean", "D3D11 Ocean", ocean_init, ocean_frame, ocean_cleanup},
    {"mandelbulb", "D3D11 Mandelbulb", bulb_init, bulb_frame, bulb_cleanup},
    {"nebula", "D3D11 Nebula", nebula_init, nebula_frame, nebula_cleanup},
    {"showcase", "D3D11 Showcase", show_init, show_frame, show_cleanup},
    {"space", "D3D11 Space", space_init, space_frame, space_cleanup},
    {"city", "D3D11 Cityscape", city_init, city_frame, city_cleanup},
    {"gsexplode", "D3D11 GS Exploder", gs_init, gs_frame, gs_cleanup},
    {"cel", "D3D11 Cel Shading", cel_init, cel_frame, cel_cleanup},
    {"matcap", "D3D11 Matcap", mat_init, mat_frame, mat_cleanup},
    {"atomics", "D3D11 Atomics", atom_init, atom_frame, atom_cleanup},
    {"drawstress", "D3D11 Draw Stress", ds_init, ds_frame, ds_cleanup},
};

static const D3D11Scene *pick_scene(const char *name) {
    if (name)
        for (size_t i = 0; i < sizeof(kScenes) / sizeof(kScenes[0]); i++)
            if (strcmp(kScenes[i].name, name) == 0) return &kScenes[i];
    return &kScenes[0];  // default: spin
}

// ================================ the runner ================================
int aio_run_d3d11_cube(HINSTANCE hinst, const char *scene_name) {
    const D3D11Scene *scene = pick_scene(scene_name);
    const char *api = scene->label;
    const char *cls = "AIOD3D11Cube";

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = d3d11_wndproc;
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

    HMODULE d3d11lib = LoadLibraryA("d3d11.dll");
    PFN_D3D11CreateDeviceAndSwapChain p_create =
        d3d11lib ? (PFN_D3D11CreateDeviceAndSwapChain)GetProcAddress(d3d11lib,
                                                                     "D3D11CreateDeviceAndSwapChain")
                 : NULL;
    if (!p_create) {
        fail_box(
            "Direct3D 11 is not available in this container.\n\n"
            "Could not load d3d11.dll (is DXVK installed?).");
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

    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *swap = NULL;
    HRESULT hr = p_create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want,
                          (UINT)(sizeof(want) / sizeof(want[0])), D3D11_SDK_VERSION, &scd, &swap,
                          &dev, &got, &ctx);
    if (FAILED(hr)) {
        fail_box(
            "Direct3D 11 is not available in this container.\n\n"
            "Could not create a D3D11 device + swapchain (no d3d11.dll / DXVK?).");
        DestroyWindow(hwnd);
        return 1;
    }

    ID3D11Texture2D *backbuf = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&backbuf);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)backbuf, NULL, &rtv);

    D3D11_TEXTURE2D_DESC dd;
    memset(&dd, 0, sizeof(dd));
    dd.Width = g_w;
    dd.Height = g_h;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D *depth_tex = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11Device_CreateTexture2D(dev, &dd, NULL, &depth_tex);
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)depth_tex, NULL, &dsv);

    D3D11_DEPTH_STENCIL_DESC dsd;
    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    ID3D11DepthStencilState *dss = NULL;
    ID3D11Device_CreateDepthStencilState(dev, &dsd, &dss);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, dss, 1);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);

    D3D11_VIEWPORT vp;
    memset(&vp, 0, sizeof(vp));
    vp.Width = (float)g_w;
    vp.Height = (float)g_h;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);

    // Shader compiler (runtime, dynamic).
    HMODULE d3dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!d3dc) d3dc = LoadLibraryA("d3dcompiler_43.dll");
    g_compile = d3dc ? (PFN_D3DCompile)GetProcAddress(d3dc, "D3DCompile") : NULL;
    if (!g_compile) {
        fail_box(
            "Could not load d3dcompiler (D3DCompile) in this container.\n\n"
            "The HLSL shaders for the Direct3D 11 scene can't be compiled.");
        DestroyWindow(hwnd);
        return 1;
    }

    if (scene->init(dev, ctx, g_w, g_h) != 0) {
        DestroyWindow(hwnd);
        return 1;
    }

    aio_hud_create(hinst);
    char hud0[96];
    snprintf(hud0, sizeof(hud0), "%s  -  measuring...", api);
    aio_hud_update(hwnd, hud0);

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
            if (msg.message == WM_QUIT) {
                g_quit = 1;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_quit) break;

        // Apply a pending window resize: resize the swapchain + recreate RTV/DSV +
        // viewport, and refresh the aspect ratio (so circles stay round, not oval).
        if (g_resize_pending) {
            g_resize_pending = 0;
            int nw = g_resize_w, nh = g_resize_h;
            if (nw > 0 && nh > 0 && (nw != g_w || nh != g_h)) {
                ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
                if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
                if (backbuf) { ID3D11Texture2D_Release(backbuf); backbuf = NULL; }
                if (dsv) { ID3D11DepthStencilView_Release(dsv); dsv = NULL; }
                if (depth_tex) { ID3D11Texture2D_Release(depth_tex); depth_tex = NULL; }
                if (SUCCEEDED(IDXGISwapChain_ResizeBuffers(swap, 0, nw, nh, DXGI_FORMAT_UNKNOWN, 0))) {
                    g_w = nw;
                    g_h = nh;
                    IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&backbuf);
                    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)backbuf, NULL, &rtv);
                    dd.Width = g_w;
                    dd.Height = g_h;
                    ID3D11Device_CreateTexture2D(dev, &dd, NULL, &depth_tex);
                    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)depth_tex, NULL, &dsv);
                    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
                    vp.Width = (float)g_w;
                    vp.Height = (float)g_h;
                    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
                    aspect = (g_h > 0) ? (float)g_w / (float)g_h : 1.0f;
                }
            }
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / (double)qpf.QuadPart;

        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        scene->frame(ctx, t, aspect);
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
            char hud[96], title[160];
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
        // Keyed by the per-scene label so each DX11 scene gets its own result file.
        char *res = aio_bench_finish(api, total);
        if (res) {
            aio_bench_show_result(res);
            free(res);
        }
    }

    aio_hud_destroy();
    scene->cleanup();

    if (dss) ID3D11DepthStencilState_Release(dss);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
    if (depth_tex) ID3D11Texture2D_Release(depth_tex);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (backbuf) ID3D11Texture2D_Release(backbuf);
    if (swap) IDXGISwapChain_Release(swap);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);

    DestroyWindow(hwnd);
    return 0;
}
