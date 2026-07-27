// AIO Graphics Test - Bionic-FG validation harness (Direct3D 11 source).
//
// A deterministic, instrumented *frame source* for validating the Bannerlator
// Bionic-FG Vulkan frame-generation layer. It is NOT a frame generator and NOT
// a benchmark: it renders REAL frames at a precise, known cap and bakes machine-
// readable markers into every frame so that an external high-speed capture (see
// tools/fg_analyze.py) can prove whether the frames the layer INSERTS are:
//   1. actually present   (throughput doubled/tripled/quadrupled),
//   2. genuinely interpolated (not duplicated / not extrapolated),
//   3. spatially correct  (inserted frame sits at the true midpoint of motion),
//   4. evenly paced       (no 33/0/33/0 judder).
//
// Why a *source* and not a measurer: Bionic-FG sits BELOW the app in the Vulkan
// swapchain, so this process can only ever see (and honestly report) its own
// REAL frames. It therefore provides the ground truth - a known cadence and a
// deterministic image - and leaves the counting of generated frames to a capture
// of the actual on-screen output, decoded against the results file we write.
//
// Every on-screen element is a pure function of the integer real-frame index
// `fi` (wall-clock is used ONLY to pace presents, never to place geometry), so
// the analyzer can reconstruct the exact expected state for any real frame and
// any interpolation phase between two of them.
//
// Baked signals (all flat-shaded, hard-edged, so interpolation blends cleanly):
//   * FLICKER block  - inverts fully every real frame. A generated frame between
//                      two reals blends to ~50% grey => unmistakable "this frame
//                      was interpolated" flag, and a live ghosting indicator.
//   * BARCODE strip   - start pattern + 20-bit real-frame index + parity + stop.
//                      Crisp+valid => REAL frame; blurred/parity-fail => GENERATED;
//                      crisp+repeated index => DUPLICATE (a broken FG).
//   * PHASE swatch    - hue cycles per real frame; interpolated frames show an
//                      intermediate hue (a second, independent phase estimator).
//   * VELOCITY marker - bright square sweeping at a fixed px/frame. A correct
//                      generated frame lands it at the motion midpoint; a
//                      duplicate leaves it on a real position. => interpolation
//                      accuracy in pixels.
//   * ROTOR / BARS / OCCLUDER - motion-quality stressors (rotation, fast thin
//                      edges, dis-occlusion) for the visual/artifact pass.
//
// d3d11.dll and d3dcompiler are loaded dynamically (as the rest of this repo
// does), so the .exe has no static dependency and runs under Wine+DXVK where the
// container provides them at runtime.
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

// ------------------------------------------------------------------ config ---
#define FGT_VERSION "1"
#define BARCODE_DATA_BITS 20              // real-frame index range: 0 .. 2^20-1
#define MAX_VERTS 24000                   // CPU quad scratch (6 verts / quad)

// Element bitmask - a "scene" is a set of these.
enum {
    EL_BARCODE  = 1 << 0,   // top strip: start + index + parity + stop
    EL_FLICKER  = 1 << 1,   // full-invert block (primary real/gen discriminator)
    EL_PHASE    = 1 << 2,   // hue-cycling swatch (independent phase signal)
    EL_MARKER   = 1 << 3,   // constant-velocity sweep (interpolation accuracy)
    EL_ROTOR    = 1 << 4,   // rotating spoke (angular motion)
    EL_BARS     = 1 << 5,   // fast thin vertical bars (ghosting stress)
    EL_OCCLUDER = 1 << 6,   // sweeping occluder over the marker (dis-occlusion)
};
#define SCENE_VERIFY (EL_BARCODE | EL_FLICKER | EL_PHASE | EL_MARKER)
#define SCENE_MOTION (SCENE_VERIFY | EL_ROTOR | EL_BARS)
#define SCENE_STRESS (SCENE_VERIFY | EL_OCCLUDER | EL_BARS)
#define SCENE_FULL   (SCENE_VERIFY | EL_ROTOR | EL_BARS | EL_OCCLUDER)

typedef struct {
    const char *name;
    unsigned    mask;
} Scene;
static const Scene kScenes[] = {
    {"verify", SCENE_VERIFY},
    {"motion", SCENE_MOTION},
    {"stress", SCENE_STRESS},
    {"full",   SCENE_FULL},
};
static const int kSceneCount = (int)(sizeof(kScenes) / sizeof(kScenes[0]));

// --------------------------------------------------------------- app state ---
static int      g_w = 1280, g_h = 720;
static int      g_quit = 0;
static int      g_paused = 0;
static int      g_vsync = 0;
static int      g_cap = 30;               // target REAL fps
static int      g_scene = 0;              // index into kScenes
static double   g_duration = 0.0;         // headless auto-exit seconds (0 = run until closed)
static char     g_out[MAX_PATH] = "fgtest_results.json";
static char     g_arch[16] =
#if defined(__x86_64__) || defined(_M_X64)
    "x86_64";
#else
    "i686";
#endif

static volatile int g_resize_pending = 0, g_resize_w = 0, g_resize_h = 0;

// Frame-timing accumulators (REAL frames only).
static uint64_t g_real_frames = 0;
static double   g_ft_sum = 0, g_ft_sumsq = 0, g_ft_min = 1e30, g_ft_max = 0;
static uint64_t g_dropped = 0;            // real frames that overran 1.5x their budget
static double   g_first_present = 0, g_last_present = 0;

// ------------------------------------------------------------ small helpers ---
static double qpc_freq = 0;
static double now_s(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / qpc_freq;
}

static void fail_box(const char *msg) {
    MessageBoxA(NULL, msg, "AIO Graphics Test - Bionic-FG Tester", MB_OK | MB_ICONERROR);
}

// hsv (h in [0,1)) -> rgb, s=v=1. Used for the phase swatch.
static void hue_rgb(float h, float *r, float *g, float *b) {
    float x = fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f);
    float c = 1.0f - x;
    int   seg = (int)(h * 6.0f) % 6;
    switch (seg) {
        case 0: *r = 1; *g = c; *b = 0; break;
        case 1: *r = c; *g = 1; *b = 0; break;
        case 2: *r = 0; *g = 1; *b = c; break;
        case 3: *r = 0; *g = c; *b = 1; break;
        case 4: *r = c; *g = 0; *b = 1; break;
        default:*r = 1; *g = 0; *b = c; break;
    }
}

// -------------------------------------------------------------- COM / D3D11 ---
typedef HRESULT(WINAPI *PFN_D3D11CreateDeviceAndSwapChain)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);
static PFN_D3DCompile g_compile;

static ID3DBlob *compile_hlsl(const char *src, const char *entry, const char *target) {
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = g_compile(src, strlen(src), "fgtest.hlsl", NULL, NULL, entry, target, 0, 0,
                           &blob, &err);
    if (FAILED(hr)) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Shader '%s' (%s) failed:\n\n%s", entry, target,
                 err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        fail_box(msg);
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return blob;
}

// 2D flat-colour vertex: NDC position + RGBA. Barcode/markers are all quads.
typedef struct { float x, y; float r, g, b, a; } Vtx;
static Vtx  g_verts[MAX_VERTS];
static int  g_nverts = 0;

// Push an axis-aligned rect given in PIXELS (top-left origin), colour in 0..1.
static void push_quad(float px, float py, float pw, float ph, float r, float g, float b) {
    if (g_nverts + 6 > MAX_VERTS) return;
    float x0 = px / g_w * 2.0f - 1.0f;
    float x1 = (px + pw) / g_w * 2.0f - 1.0f;
    float y0 = 1.0f - py / g_h * 2.0f;              // top
    float y1 = 1.0f - (py + ph) / g_h * 2.0f;       // bottom
    Vtx q[6] = {
        {x0, y0, r, g, b, 1}, {x1, y0, r, g, b, 1}, {x1, y1, r, g, b, 1},
        {x0, y0, r, g, b, 1}, {x1, y1, r, g, b, 1}, {x0, y1, r, g, b, 1},
    };
    memcpy(&g_verts[g_nverts], q, sizeof(q));
    g_nverts += 6;
}

// A thin rotated bar (the rotor spoke): quad from center out, rotated.
static void push_rotor(float cx, float cy, float len, float thick, float ang,
                       float r, float g, float b) {
    if (g_nverts + 6 > MAX_VERTS) return;
    float dx = cosf(ang), dy = sinf(ang);          // along-bar
    float nx = -dy, ny = dx;                        // across-bar
    float hx = nx * thick * 0.5f, hy = ny * thick * 0.5f;
    // Corners in pixel space: base (cx,cy) to tip (cx+dx*len, cy+dy*len).
    float ax = cx + hx, ay = cy + hy;
    float bx = cx - hx, by = cy - hy;
    float tx = cx + dx * len, ty = cy + dy * len;
    float cx2 = tx + hx, cy2 = ty + hy;
    float dx2 = tx - hx, dy2 = ty - hy;
#define NDCX(P) ((P) / g_w * 2.0f - 1.0f)
#define NDCY(P) (1.0f - (P) / g_h * 2.0f)
    Vtx q[6] = {
        {NDCX(ax),  NDCY(ay),  r, g, b, 1}, {NDCX(cx2), NDCY(cy2), r, g, b, 1},
        {NDCX(dx2), NDCY(dy2), r, g, b, 1}, {NDCX(ax),  NDCY(ay),  r, g, b, 1},
        {NDCX(bx),  NDCY(by),  r, g, b, 1}, {NDCX(dx2), NDCY(dy2), r, g, b, 1},
    };
#undef NDCX
#undef NDCY
    memcpy(&g_verts[g_nverts], q, sizeof(q));
    g_nverts += 6;
}

// ------------------------------------------------------------ scene builder ---
// Barcode geometry (kept in one place so the results file can describe it and
// the analyzer can reconstruct it exactly).
#define BC_START_CELLS 3                              // 1,0,1 locator
#define BC_STOP_CELLS  2                              // 1,0 terminator
#define BC_CELLS (BC_START_CELLS + BARCODE_DATA_BITS + 1 /*parity*/ + BC_STOP_CELLS)
static float bc_cell_w(void)  { return (float)g_w / (BC_CELLS + 2); }  // +2 quiet margin
static float bc_strip_h(void) { return g_h * 0.07f < 24 ? 24.0f : g_h * 0.07f; }

static int marker_size(void)   { return g_h / 12 < 24 ? 24 : g_h / 12; }
// Sweep step chosen so the marker crosses the screen in ~2 s of REAL frames.
static float marker_step(void) {
    float span = (float)(g_w - marker_size());
    return span / (float)(g_cap * 2);
}
static int marker_y(void) { return g_h / 2 - marker_size() / 2; }

// Build all quads for REAL frame index `fi` under the active element mask.
static void build_scene(uint64_t fi, unsigned mask) {
    g_nverts = 0;

    // --- FLICKER: full invert every real frame (primary real/gen flag). ---
    if (mask & EL_FLICKER) {
        float v = (fi & 1) ? 1.0f : 0.0f;
        float bw = bc_cell_w() * 2.0f;
        push_quad(0, bc_strip_h(), bw, bw, v, v, v);
    }

    // --- BARCODE strip along the very top. ---
    if (mask & EL_BARCODE) {
        float cw = bc_cell_w(), h = bc_strip_h();
        float x = cw;                                  // 1-cell quiet margin
        int   bits[BC_CELLS], n = 0;
        bits[n++] = 1; bits[n++] = 0; bits[n++] = 1;   // start
        int parity = 0;
        for (int i = 0; i < BARCODE_DATA_BITS; i++) {
            int bit = (int)((fi >> i) & 1);
            bits[n++] = bit;
            parity ^= bit;
        }
        bits[n++] = parity;
        bits[n++] = 1; bits[n++] = 0;                  // stop
        for (int i = 0; i < n; i++, x += cw) {
            float v = bits[i] ? 1.0f : 0.0f;
            push_quad(x, 0, cw - 1, h, v, v, v);       // -1px keeps cell edges crisp
        }
    }

    // --- PHASE swatch: hue advances one step per real frame. ---
    if (mask & EL_PHASE) {
        float r, g, b;
        hue_rgb(fmodf((float)(fi % 60) / 60.0f, 1.0f), &r, &g, &b);
        float s = bc_strip_h();
        push_quad((float)g_w - s * 2.0f, bc_strip_h(), s * 2.0f, s, r, g, b);
    }

    // --- BARS: fast thin white vertical bars (ghosting stress). ---
    if (mask & EL_BARS) {
        int   nbar = 4;
        float step = (float)g_w / (float)(g_cap);      // ~1 screen / sec => fast
        for (int i = 0; i < nbar; i++) {
            float bx = fmodf((float)fi * step + i * (g_w / (float)nbar), (float)g_w);
            push_quad(bx, g_h * 0.25f, 3.0f, g_h * 0.5f, 1, 1, 1);
        }
    }

    // --- ROTOR: spoke rotating a fixed angle per real frame. ---
    if (mask & EL_ROTOR) {
        float ang = (float)fi * 0.10f;                 // rad / real frame
        float cx = g_w * 0.5f, cy = g_h * 0.62f;
        float len = g_h * 0.28f;
        push_rotor(cx, cy, len, 6.0f, ang, 0.2f, 1.0f, 0.4f);
        push_rotor(cx, cy, len, 6.0f, ang + 3.14159f, 1.0f, 0.4f, 0.2f);
    }

    // --- MARKER: constant-velocity sweep (interpolation accuracy probe). ---
    if (mask & EL_MARKER) {
        int   sz = marker_size();
        float span = (float)(g_w - sz);
        float pos = fmodf((float)fi * marker_step(), span * 2.0f);
        float mx = pos <= span ? pos : span * 2.0f - pos;   // ping-pong (no wrap jump)
        push_quad(mx, (float)marker_y(), (float)sz, (float)sz, 0.1f, 0.9f, 1.0f);
    }

    // --- OCCLUDER: dark block sweeping across, passing over the marker. ---
    if (mask & EL_OCCLUDER) {
        int   sz = marker_size();
        float span = (float)(g_w - sz * 3);
        float pos = fmodf((float)fi * marker_step() * 1.3f, span * 2.0f);
        float ox = pos <= span ? pos : span * 2.0f - pos;
        push_quad(ox, (float)marker_y() - sz, (float)sz * 3, (float)sz * 3, 0.05f, 0.05f, 0.08f);
    }
}

// ----------------------------------------------------------------- results ---
static void write_results(const char *backend) {
    FILE *f = fopen(g_out, "wb");
    if (!f) return;
    double dur = (g_last_present > g_first_present) ? (g_last_present - g_first_present) : 0.0;
    double avg = g_real_frames ? g_ft_sum / (double)g_real_frames : 0.0;
    double var = g_real_frames ? (g_ft_sumsq / (double)g_real_frames - avg * avg) : 0.0;
    double sd  = var > 0 ? sqrt(var) : 0.0;
    double mfps = dur > 0 ? (double)(g_real_frames - 1) / dur : 0.0;
    fprintf(f,
        "{\n"
        "  \"tool\": \"aio-fgtest\",\n"
        "  \"version\": \"%s\",\n"
        "  \"backend\": \"%s\",\n"
        "  \"arch\": \"%s\",\n"
        "  \"config\": {\n"
        "    \"cap_fps\": %d,\n"
        "    \"vsync\": %s,\n"
        "    \"scene\": \"%s\",\n"
        "    \"element_mask\": %u,\n"
        "    \"width\": %d,\n"
        "    \"height\": %d\n"
        "  },\n"
        "  \"geometry\": {\n"
        "    \"barcode\": { \"cells\": %d, \"start_cells\": %d, \"data_bits\": %d,\n"
        "                 \"stop_cells\": %d, \"cell_w_px\": %.3f, \"strip_h_px\": %.3f,\n"
        "                 \"quiet_margin_cells\": 1 },\n"
        "    \"flicker\": { \"x_px\": 0, \"y_px\": %.3f, \"size_px\": %.3f,\n"
        "                 \"note\": \"invert every real frame; ~0.5 grey => generated\" },\n"
        "    \"phase\": { \"period_frames\": 60, \"note\": \"hue = (fi%%60)/60\" },\n"
        "    \"marker\": { \"size_px\": %d, \"y_px\": %d, \"step_px_per_frame\": %.5f,\n"
        "                \"motion\": \"pingpong\", \"span_px\": %d,\n"
        "                \"pos_fn\": \"p=fmod(fi*step,2*span); x = p<=span ? p : 2*span-p\" }\n"
        "  },\n"
        "  \"run\": {\n"
        "    \"real_frames\": %llu,\n"
        "    \"duration_s\": %.4f,\n"
        "    \"measured_real_fps\": %.3f,\n"
        "    \"frametime_ms\": { \"avg\": %.3f, \"min\": %.3f, \"max\": %.3f, \"stddev\": %.3f },\n"
        "    \"dropped_real_frames\": %llu\n"
        "  }\n"
        "}\n",
        FGT_VERSION, backend, g_arch,
        g_cap, g_vsync ? "true" : "false", kScenes[g_scene].name, kScenes[g_scene].mask, g_w, g_h,
        BC_CELLS, BC_START_CELLS, BARCODE_DATA_BITS, BC_STOP_CELLS, bc_cell_w(), bc_strip_h(),
        bc_strip_h(), bc_cell_w() * 2.0f,
        marker_size(), marker_y(), marker_step(), g_w - marker_size(),
        (unsigned long long)g_real_frames, dur, mfps,
        avg * 1000.0, (g_ft_min > 1e29 ? 0 : g_ft_min) * 1000.0, g_ft_max * 1000.0, sd * 1000.0,
        (unsigned long long)g_dropped);
    fclose(f);
}

// ------------------------------------------------------------- window proc ---
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CLOSE: g_quit = 1; return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) {
                g_resize_w = LOWORD(l); g_resize_h = HIWORD(l); g_resize_pending = 1;
            }
            return 0;
        case WM_KEYDOWN:
            switch (w) {
                case VK_ESCAPE: g_quit = 1; break;
                case VK_SPACE:  g_paused = !g_paused; break;
                case 'V':       g_vsync = !g_vsync; break;
                case VK_OEM_4:  if (g_cap > 5)  g_cap -= 5; break;   // '['  cap down
                case VK_OEM_6:  if (g_cap < 240) g_cap += 5; break;  // ']'  cap up
                case '1': case '2': case '3': case '4':
                    if ((int)(w - '1') < kSceneCount) g_scene = (int)(w - '1');
                    break;
            }
            return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

// ------------------------------------------------------------- arg parsing ---
static void parse_args(const char *cmdline) {
    // Tokenize lpCmdLine into an argv-style array (avoids a shell32 dependency),
    // then walk it with simple lookahead for flags that take a value.
    static char buf[1024];
    strncpy(buf, cmdline ? cmdline : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *argv[64]; int argc = 0;
    for (char *t = strtok(buf, " \t"); t && argc < 64; t = strtok(NULL, " \t")) argv[argc++] = t;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if      (!strcmp(a, "--cap") && v)      { g_cap = atoi(v); i++; }
        else if (!strcmp(a, "--duration") && v) { g_duration = atof(v); i++; }
        else if (!strcmp(a, "--width") && v)    { g_w = atoi(v); i++; }
        else if (!strcmp(a, "--height") && v)   { g_h = atoi(v); i++; }
        else if (!strcmp(a, "--out") && v)      { strncpy(g_out, v, sizeof(g_out) - 1); i++; }
        else if (!strcmp(a, "--scene") && v)    {
            for (int s = 0; s < kSceneCount; s++)
                if (!strcmp(kScenes[s].name, v)) g_scene = s;
            i++;
        }
        else if (!strcmp(a, "--vsync"))    g_vsync = 1;
        else if (!strcmp(a, "--selftest")) { if (g_duration <= 0) g_duration = 8.0; }
    }
    if (g_cap < 5)   g_cap = 5;
    if (g_cap > 240) g_cap = 240;
    if (g_w < 320)   g_w = 320;
    if (g_h < 240)   g_h = 240;
}

// -------------------------------------------------------------------- main ---
static const char *kVS =
    "struct VSIn  { float2 pos : POSITION; float4 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
    "VSOut VSMain(VSIn i){ VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.col = i.col; return o; }\n";
static const char *kPS =
    "struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
    "float4 PSMain(VSOut i) : SV_Target { return i.col; }\n";

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE hprev, LPSTR cmdline, int nshow) {
    (void)hprev; (void)nshow;
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf); qpc_freq = (double)qf.QuadPart;
    parse_args(cmdline);
    timeBeginPeriod(1);

    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "AIOFGTestWnd";
    RegisterClassA(&wc);

    RECT r = {0, 0, g_w, g_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("AIOFGTestWnd",
        "AIO Graphics Test  -  Bionic-FG source harness (D3D11)   [ [ ] cap  V vsync  Space pause  1-4 scene  Esc ]",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, NULL, NULL, hinst, NULL);
    if (!hwnd) return 1;
    RECT cr; GetClientRect(hwnd, &cr);
    g_w = cr.right - cr.left; g_h = cr.bottom - cr.top;

    HMODULE d3d11lib = LoadLibraryA("d3d11.dll");
    PFN_D3D11CreateDeviceAndSwapChain p_create = d3d11lib ?
        (PFN_D3D11CreateDeviceAndSwapChain)GetProcAddress(d3d11lib, "D3D11CreateDeviceAndSwapChain") : NULL;
    if (!p_create) { fail_box("Direct3D 11 not available (no d3d11.dll / DXVK?)."); return 1; }

    DXGI_SWAP_CHAIN_DESC scd; memset(&scd, 0, sizeof(scd));
    scd.BufferCount = 2;
    scd.BufferDesc.Width = g_w; scd.BufferDesc.Height = g_h;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd; scd.SampleDesc.Count = 1; scd.Windowed = TRUE;

    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; IDXGISwapChain *swap = NULL;
    HRESULT hr = p_create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want,
                          (UINT)(sizeof(want) / sizeof(want[0])), D3D11_SDK_VERSION,
                          &scd, &swap, &dev, &got, &ctx);
    if (FAILED(hr)) { fail_box("Could not create D3D11 device + swapchain (DXVK missing?)."); return 1; }

    ID3D11Texture2D *backbuf = NULL; ID3D11RenderTargetView *rtv = NULL;
    IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&backbuf);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)backbuf, NULL, &rtv);

    D3D11_VIEWPORT vp; memset(&vp, 0, sizeof(vp));
    vp.Width = (float)g_w; vp.Height = (float)g_h; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);

    HMODULE d3dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!d3dc) d3dc = LoadLibraryA("d3dcompiler_43.dll");
    g_compile = d3dc ? (PFN_D3DCompile)GetProcAddress(d3dc, "D3DCompile") : NULL;
    if (!g_compile) { fail_box("Could not load d3dcompiler (D3DCompile)."); return 1; }

    ID3DBlob *vsb = compile_hlsl(kVS, "VSMain", "vs_4_0");
    ID3DBlob *psb = compile_hlsl(kPS, "PSMain", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11InputLayout *layout = NULL;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &layout);

    D3D11_BUFFER_DESC bd; memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(g_verts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *vb = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &vb);

    // Fixed-function-ish state: no cull, no depth (all 2D).
    D3D11_RASTERIZER_DESC rsd; memset(&rsd, 0, sizeof(rsd));
    rsd.FillMode = D3D11_FILL_SOLID; rsd.CullMode = D3D11_CULL_NONE;
    ID3D11RasterizerState *rs = NULL;
    ID3D11Device_CreateRasterizerState(dev, &rsd, &rs);
    ID3D11DeviceContext_RSSetState(ctx, rs);

    // ---- pacing state ----
    double start = now_s();
    double next_deadline = start;
    double prev_present = 0;
    uint64_t fi = 0;                  // real-frame index baked into the image
    char title[256];
    double last_title = 0;
    double measured_fps = 0;
    uint64_t frames_in_window = 0; double window_start = start;

    MSG msg;
    while (!g_quit) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_quit = 1;
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        if (g_quit) break;

        if (g_resize_pending) {
            g_resize_pending = 0;
            int nw = g_resize_w, nh = g_resize_h;
            if (nw > 0 && nh > 0 && (nw != g_w || nh != g_h)) {
                ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
                if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
                if (backbuf) { ID3D11Texture2D_Release(backbuf); backbuf = NULL; }
                IDXGISwapChain_ResizeBuffers(swap, 2, nw, nh, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
                g_w = nw; g_h = nh;
                IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&backbuf);
                ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)backbuf, NULL, &rtv);
                vp.Width = (float)g_w; vp.Height = (float)g_h;
                ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            }
        }

        // ---- precise pace to the REAL cap (skip when vsync drives cadence) ----
        double target_dt = 1.0 / (double)g_cap;
        if (!g_vsync) {
            double t = now_s();
            if (t < next_deadline) {
                double remain = next_deadline - t;
                if (remain > 0.002) Sleep((DWORD)((remain - 0.001) * 1000.0));   // coarse
                while (now_s() < next_deadline) { /* spin the last ~1 ms */ }
            }
            next_deadline += target_dt;
            // If we fell far behind (e.g. a stall), resync to avoid a burst.
            double slack = now_s() - next_deadline;
            if (slack > target_dt) next_deadline = now_s();
        }

        // ---- build + upload this real frame's deterministic geometry ----
        build_scene(fi, kScenes[g_scene].mask);
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vb, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, g_verts, (size_t)g_nverts * sizeof(Vtx));
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vb, 0);
        }

        float clear[4] = {0.06f, 0.06f, 0.08f, 1.0f};
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        UINT stride = sizeof(Vtx), off = 0;
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, (UINT)g_nverts, 0);

        IDXGISwapChain_Present(swap, g_vsync ? 1 : 0, 0);

        // ---- account this REAL present ----
        double tp = now_s();
        if (prev_present > 0) {
            double dt = tp - prev_present;
            g_ft_sum += dt; g_ft_sumsq += dt * dt;
            if (dt < g_ft_min) g_ft_min = dt;
            if (dt > g_ft_max) g_ft_max = dt;
            if (dt > target_dt * 1.5) g_dropped++;
        }
        if (g_first_present == 0) g_first_present = tp;
        g_last_present = tp;
        prev_present = tp;
        g_real_frames++;
        if (!g_paused) fi++;             // pause freezes the image but keeps the window live

        // live title stats (~4 Hz)
        frames_in_window++;
        if (tp - window_start >= 0.25) {
            measured_fps = (double)frames_in_window / (tp - window_start);
            frames_in_window = 0; window_start = tp;
        }
        if (tp - last_title >= 0.25) {
            last_title = tp;
            snprintf(title, sizeof(title),
                     "AIO Bionic-FG Tester (D3D11)  |  cap %d  measured %.1f fps  |  vsync %s  scene %s%s",
                     g_cap, measured_fps, g_vsync ? "on" : "off", kScenes[g_scene].name,
                     g_paused ? "  [PAUSED]" : "");
            SetWindowTextA(hwnd, title);
        }

        if (g_duration > 0 && (tp - start) >= g_duration) g_quit = 1;
    }

    write_results("D3D11");
    timeEndPeriod(1);
    return 0;
}
