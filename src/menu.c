// AIO Graphics Test - app shell (Win32).
//
// A persistent left sidebar (the menu, always visible) + a content pane on the
// right. GPU Info opens IN-FRAME as a tabbed Vulkan/OpenGL view (like the
// standalone GPUInfo.exe). Cube tests open in a NEW window (a separate process,
// so the menu stays usable and you can switch between tests).
//
// Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

#include <windows.h>
#include <commctrl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu.h"
#include "gpuinfo.h"

#define SB_W 150
#define HEADER_H 26
#define FOOT_H 24  // reserved strip at the bottom for the footnote
#define ID_FIRST_BUTTON 1000
#define ID_TAB 2000

typedef struct {
    const char *label;
    int action;  // AioMode
} SbItem;

static SbItem g_items[] = {
    {"Cube  -  Vulkan", AIO_MODE_CUBE_VK},     {"Cube  -  OpenGL", AIO_MODE_CUBE_GL},
    {"Cube  -  DirectDraw", AIO_MODE_CUBE_DDRAW},
    {"Cube  -  Direct3D 8", AIO_MODE_CUBE_DX8}, {"Cube  -  Direct3D 9", AIO_MODE_CUBE_DX9},
    {"Cube  -  Direct3D 10", AIO_MODE_CUBE_DX10},
    {"Cube  -  Direct3D 11", AIO_MODE_CUBE_DX11}, {"Cube  -  Direct3D 12", AIO_MODE_CUBE_DX12},
    {"GPU Info", AIO_MODE_GPUINFO},
    {"Benchmark", AIO_MODE_BENCH},             {"Semaphore Probe", AIO_MODE_SEMAPHORE},
    {"Exit", AIO_MODE_EXIT},
};
#define NITEMS ((int)(sizeof(g_items) / sizeof(g_items[0])))

static HINSTANCE g_hinst;
static HWND g_header;
static HWND g_sidebar[NITEMS];
static HWND g_foot_a, g_foot_heart, g_foot_b;  // footnote: "Built with" [red heart] "for ..."
static HWND g_version;  // bottom-right version label
static HFONT g_ui_font;
static HFONT g_ui_font_bold;
static HFONT g_header_font;
static HFONT g_mono_font;

// Content views.
static HWND g_tab;       // GPU Info tab control
static HWND g_edit_vk;   // GPU Info: Vulkan tab text
static HWND g_edit_gl;   // GPU Info: OpenGL tab text
static HWND g_placeholder;

#define ID_CB_FIRST 3000  // content-area buttons (Benchmark + scene-picker views)
#define MAX_CB 20
static HWND g_cbtn[MAX_CB];
static HWND g_cbtn_avg[MAX_CB];      // bold "Avg N" label next to each benchmark button
static HWND g_cbtn_result[MAX_CB];   // "Min N   Max N" label next to each benchmark button
static const char *g_cbtn_arg[MAX_CB];
static const char *g_cbtn_label[MAX_CB];  // API label for the result file name
static HANDLE g_cbtn_proc[MAX_CB];   // running benchmark process (polled)
static int g_cbtn_n;
static int g_cb_bench;  // 1 = Benchmark view (poll + show result); 0 = launch-only
static int g_is_probe;        // semaphore-probe view active (verdict logic)
static float g_probe_avg[2];  // [0] = timeline avg FPS, [1] = binary avg FPS
static HWND g_verdict;        // probe verdict label
#define ID_RUN_ALL 3500
static HWND g_run_all;        // "Run All" sweep button (Benchmark view)
static int g_sweep_active;    // sequential run-all sweep in progress
static int g_sweep_idx;       // current row in the sweep
#define ID_DUR_FIRST 3600     // duration buttons (15/30/45/60 s)
#define ID_VSYNC 3620
static HWND g_dur_btn[4];
static HWND g_dur_label;
static HWND g_vsync_chk;
static int g_bench_secs = 15;   // selected benchmark duration
static int g_vsync_ui = 0;      // vsync toggle state
static int g_bench_append = 0;  // launch_bench_row appends --bench/--vsync (Benchmark view)

static void get_content_rect(HWND frame, RECT *out) {
    RECT rc;
    GetClientRect(frame, &rc);
    out->left = SB_W + 10;
    out->top = 10 + HEADER_H;
    out->right = rc.right - 10;
    out->bottom = rc.bottom - 10 - FOOT_H;  // leave room for the footnote
}

static void destroy_content(void) {
    if (g_edit_vk) { DestroyWindow(g_edit_vk); g_edit_vk = NULL; }
    if (g_edit_gl) { DestroyWindow(g_edit_gl); g_edit_gl = NULL; }
    if (g_tab) { DestroyWindow(g_tab); g_tab = NULL; }
    if (g_placeholder) { DestroyWindow(g_placeholder); g_placeholder = NULL; }
    if (g_verdict) { DestroyWindow(g_verdict); g_verdict = NULL; }
    if (g_run_all) { DestroyWindow(g_run_all); g_run_all = NULL; }
    if (g_dur_label) { DestroyWindow(g_dur_label); g_dur_label = NULL; }
    if (g_vsync_chk) { DestroyWindow(g_vsync_chk); g_vsync_chk = NULL; }
    for (int i = 0; i < 4; i++)
        if (g_dur_btn[i]) { DestroyWindow(g_dur_btn[i]); g_dur_btn[i] = NULL; }
    g_is_probe = 0;
    g_sweep_active = 0;
    g_bench_append = 0;
    for (int i = 0; i < g_cbtn_n; i++) {
        if (g_cbtn[i]) DestroyWindow(g_cbtn[i]);
        g_cbtn[i] = NULL;
        if (g_cbtn_avg[i]) DestroyWindow(g_cbtn_avg[i]);
        g_cbtn_avg[i] = NULL;
        if (g_cbtn_result[i]) DestroyWindow(g_cbtn_result[i]);
        g_cbtn_result[i] = NULL;
        if (g_cbtn_proc[i]) {
            CloseHandle(g_cbtn_proc[i]);
            g_cbtn_proc[i] = NULL;
        }
    }
    g_cbtn_n = 0;
}

// In-memory result cache (this session only; empty at launch, gone on close), so
// already-run results reappear when you switch content views. Keyed by the test
// label; NOT read from the on-disk files (those would leak last session's runs).
#define MAX_CACHE 24
static struct {
    char label[40];
    float avgF;
    char avg[48];
    char mm[96];
    int used;
} g_cache[MAX_CACHE];

static int cache_find(const char *label) {
    if (!label) return -1;
    for (int i = 0; i < MAX_CACHE; i++)
        if (g_cache[i].used && strcmp(g_cache[i].label, label) == 0) return i;
    return -1;
}

static void cache_store(const char *label, float avgF, const char *avg, const char *mm) {
    if (!label) return;
    int i = cache_find(label);
    if (i < 0)
        for (i = 0; i < MAX_CACHE; i++)
            if (!g_cache[i].used) break;
    if (i >= MAX_CACHE) return;
    g_cache[i].used = 1;
    g_cache[i].avgF = avgF;
    snprintf(g_cache[i].label, sizeof(g_cache[i].label), "%s", label);
    snprintf(g_cache[i].avg, sizeof(g_cache[i].avg), "%s", avg);
    snprintf(g_cache[i].mm, sizeof(g_cache[i].mm), "%s", mm);
}

// Build the timeline-vs-binary probe verdict text.
static void probe_verdict(float tl, float bn, char *out, size_t n) {
    float ratio = (tl > 0.0f) ? bn / tl : 0.0f;
    if (ratio > 1.15f)
        snprintf(out, n, "Timeline regression CONFIRMED: binary is %.2fx faster (%.0f vs %.0f FPS).",
                 ratio, bn, tl);
    else if (ratio > 0.0f && ratio < 0.87f)
        snprintf(out, n, "Binary is slower (%.2fx) - timeline is better here (%.0f vs %.0f FPS).",
                 ratio, bn, tl);
    else
        snprintf(out, n,
                 "No significant difference (%.0f vs %.0f FPS) - this DXVK likely ignores the "
                 "toggle, or isn't affected.",
                 tl, bn);
}

// Repopulate any result-bearing rows in the current view from the cache (called
// after a view builds its buttons). Generic: works for any future view too.
static void restore_cached_results(void) {
    for (int i = 0; i < g_cbtn_n; i++) {
        int ci = cache_find(g_cbtn_label[i]);
        if (ci < 0) continue;
        if (g_cbtn_avg[i]) SetWindowTextA(g_cbtn_avg[i], g_cache[ci].avg);
        if (g_cbtn_result[i]) SetWindowTextA(g_cbtn_result[i], g_cache[ci].mm);
        if (g_is_probe && i < 2) g_probe_avg[i] = g_cache[ci].avgF;
    }
    if (g_is_probe && g_verdict && g_probe_avg[0] > 0.0f && g_probe_avg[1] > 0.0f) {
        char v[160];
        probe_verdict(g_probe_avg[0], g_probe_avg[1], v, sizeof(v));
        SetWindowTextA(g_verdict, v);
    }
}

static HWND make_report_edit(HWND frame, const RECT *r, const char *text) {
    HWND e = CreateWindowA("EDIT", "",
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                           r->left, r->top, r->right - r->left, r->bottom - r->top, frame, NULL,
                           g_hinst, NULL);
    SendMessage(e, EM_SETLIMITTEXT, 0, 0);
    if (g_mono_font) SendMessage(e, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    if (text) SetWindowTextA(e, text);
    return e;
}

static void show_gpuinfo(HWND frame) {
    destroy_content();
    SetWindowTextA(g_header, "GPU Info  (Vulkan / OpenGL)");

    RECT cr;
    get_content_rect(frame, &cr);

    g_tab = CreateWindowA(WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE, cr.left, cr.top,
                          cr.right - cr.left, cr.bottom - cr.top, frame, (HMENU)(INT_PTR)ID_TAB,
                          g_hinst, NULL);
    if (g_ui_font) SendMessage(g_tab, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    TCITEMA ti;
    memset(&ti, 0, sizeof(ti));
    ti.mask = TCIF_TEXT;
    ti.pszText = (char *)"Vulkan";
    SendMessage(g_tab, TCM_INSERTITEMA, 0, (LPARAM)&ti);
    ti.pszText = (char *)"OpenGL";
    SendMessage(g_tab, TCM_INSERTITEMA, 1, (LPARAM)&ti);

    RECT dr = cr;
    SendMessage(g_tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&dr);

    char *vk = aio_gpuinfo_build_vk_text();
    char *gl = aio_gpuinfo_build_gl_text();
    g_edit_vk = make_report_edit(frame, &dr, vk ? vk : "(no Vulkan data)");
    g_edit_gl = make_report_edit(frame, &dr, gl ? gl : "(no OpenGL data)");
    if (vk) free(vk);
    if (gl) free(gl);

    // Vulkan tab selected first.
    ShowWindow(g_edit_vk, SW_SHOW);
    ShowWindow(g_edit_gl, SW_HIDE);
}

static void show_placeholder(HWND frame, const char *title, const char *msg) {
    destroy_content();
    SetWindowTextA(g_header, title);
    RECT cr;
    get_content_rect(frame, &cr);
    g_placeholder = CreateWindowA("STATIC", msg, WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, cr.top,
                                  cr.right - cr.left, cr.bottom - cr.top, frame, NULL, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_placeholder, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
}

static void show_benchmark(HWND frame) {
    destroy_content();
    g_cb_bench = 1;  // these buttons run a benchmark and poll for a result file
    SetWindowTextA(g_header, "Benchmark");
    RECT cr;
    get_content_rect(frame, &cr);

    g_bench_append = 1;  // rows get --bench <secs> [+ --vsync] appended at launch
    g_placeholder = CreateWindowA(
        "STATIC", "Benchmark one API, or Run All to sweep them. Per-frame data -> CSV.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, cr.top, cr.right - cr.left - 200, 22, frame, NULL,
        g_hinst, NULL);
    if (g_ui_font) SendMessage(g_placeholder, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    // One-tap sequential sweep of every row.
    g_run_all = CreateWindowA("BUTTON", "Run All  (sequential)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              cr.right - 190, cr.top, 190, 32, frame, (HMENU)(INT_PTR)ID_RUN_ALL,
                              g_hinst, NULL);
    if (g_ui_font) SendMessage(g_run_all, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    // Controls: benchmark length (15/30/45/60 s) + vsync toggle.
    char dl[32];
    snprintf(dl, sizeof(dl), "Length (%ds):", g_bench_secs);
    g_dur_label = CreateWindowA("STATIC", dl, WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, cr.top + 50,
                                96, 22, frame, NULL, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_dur_label, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    static const int durs[4] = {15, 30, 45, 60};
    for (int i = 0; i < 4; i++) {
        char db[8];
        snprintf(db, sizeof(db), "%ds", durs[i]);
        g_dur_btn[i] = CreateWindowA("BUTTON", db, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     cr.left + 100 + i * 52, cr.top + 46, 48, 26, frame,
                                     (HMENU)(INT_PTR)(ID_DUR_FIRST + i), g_hinst, NULL);
        if (g_ui_font) SendMessage(g_dur_btn[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    }
    g_vsync_chk = CreateWindowA("BUTTON", "Vsync", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                cr.left + 100 + 4 * 52 + 24, cr.top + 48, 80, 22, frame,
                                (HMENU)(INT_PTR)ID_VSYNC, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_vsync_chk, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessage(g_vsync_chk, BM_SETCHECK, g_vsync_ui ? BST_CHECKED : BST_UNCHECKED, 0);

    // apilabels MUST match the label each backend passes to aio_bench_finish (the
    // DX11 ones = scene->label); args are the base specs (--bench/--vsync appended).
    static const char *labels[] = {
        "Vulkan",            "OpenGL",         "DDraw: D3D7",    "DDraw: 2D Blit",
        "D3D8: Cube",        "D3D9: Cube",     "D3D10: Cube",
        "D3D11: Cube",       "D3D11: Instanced", "D3D11: Tessellate", "D3D11: Compute",
        "D3D11: Dolphin",    "D3D11: Raymarch", "D3D11: GS Explode", "D3D11: Cel",
        "D3D11: Matcap",     "D3D12: Cube",
    };
    static const char *args[] = {
        "vk", "gl", "dx7", "ddraw2d", "dx8", "dx9", "dx10", "dx11 --scene spin",
        "dx11 --scene instanced", "dx11 --scene tess", "dx11 --scene compute",
        "dx11 --scene dolphin", "dx11 --scene raymarch", "dx11 --scene gsexplode",
        "dx11 --scene cel", "dx11 --scene matcap", "dx12",
    };
    static const char *apilabels[] = {
        "Vulkan",          "OpenGL",         "Direct3D 7 (DirectDraw)", "DirectDraw 2D",
        "Direct3D 8",      "Direct3D 9",     "Direct3D 10",
        "D3D11 Cube",      "D3D11 Instanced", "D3D11 Tessellation", "D3D11 Compute Particles",
        "D3D11 Dolphin",   "D3D11 Raymarch SDF", "D3D11 GS Exploder", "D3D11 Cel Shading",
        "D3D11 Matcap",    "Direct3D 12",
    };
    g_cbtn_n = (int)(sizeof(args) / sizeof(args[0]));
    int y = cr.top + 86;
    for (int i = 0; i < g_cbtn_n; i++) {
        g_cbtn_arg[i] = args[i];
        g_cbtn_label[i] = apilabels[i];
        g_cbtn_proc[i] = NULL;
        g_cbtn[i] = CreateWindowA("BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, cr.left,
                                  y, 200, 28, frame, (HMENU)(INT_PTR)(ID_CB_FIRST + i), g_hinst, NULL);
        if (g_ui_font) SendMessage(g_cbtn[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        // Two labels right of the button (filled when the run finishes): a BOLD
        // "Avg N" then a normal "Min N   Max N".
        g_cbtn_avg[i] = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left + 212,
                                      y + 6, 86, 24, frame, NULL, g_hinst, NULL);
        if (g_ui_font_bold) SendMessage(g_cbtn_avg[i], WM_SETFONT, (WPARAM)g_ui_font_bold, TRUE);
        g_cbtn_result[i] =
            CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left + 300, y + 6,
                          (cr.right - (cr.left + 300)), 24, frame, NULL, g_hinst, NULL);
        if (g_ui_font) SendMessage(g_cbtn_result[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        y += 34;
    }
    restore_cached_results();  // re-show results already run this session
}

// Direct3D 11 test-suite picker: each button launches one DX11 scene in a new
// window. These are launch-only (no benchmark polling).
static void show_dx11_scenes(HWND frame) {
    destroy_content();
    g_cb_bench = 0;  // launch-only buttons
    SetWindowTextA(g_header, "Cube - Direct3D 11 (DXVK)");
    RECT cr;
    get_content_rect(frame, &cr);

    g_placeholder = CreateWindowA(
        "STATIC",
        "Direct3D 11 test suite (tests the DXVK path). Each opens in a new window;\n"
        "the menu stays here. Press Esc in a test window to close it.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, cr.top, cr.right - cr.left, 56, frame, NULL,
        g_hinst, NULL);
    if (g_ui_font) SendMessage(g_placeholder, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    static const char *labels[] = {"Spinning cube",        "Textured cube",
                                   "Instanced (512 cubes)", "Tessellation (sphere)",
                                   "Compute particles",     "Dolphin (swim)",
                                   "Raymarch SDF (shader)", "GS mesh exploder",
                                   "Cel shading (torus)",   "Matcap (chrome ball)"};
    static const char *args[] = {"dx11 --scene spin",      "dx11 --scene textured",
                                 "dx11 --scene instanced", "dx11 --scene tess",
                                 "dx11 --scene compute",   "dx11 --scene dolphin",
                                 "dx11 --scene raymarch",  "dx11 --scene gsexplode",
                                 "dx11 --scene cel",       "dx11 --scene matcap"};
    g_cbtn_n = (int)(sizeof(args) / sizeof(args[0]));
    int y = cr.top + 70;
    for (int i = 0; i < g_cbtn_n; i++) {
        g_cbtn_arg[i] = args[i];
        g_cbtn_label[i] = NULL;
        g_cbtn_proc[i] = NULL;
        g_cbtn_result[i] = NULL;
        g_cbtn_avg[i] = NULL;
        g_cbtn[i] = CreateWindowA("BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  cr.left, y, 240, 34, frame, (HMENU)(INT_PTR)(ID_CB_FIRST + i),
                                  g_hinst, NULL);
        if (g_ui_font) SendMessage(g_cbtn[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        y += 44;
    }
}

// DirectDraw picker: the legacy ddraw path DXVK does NOT implement (ddraw ->
// wined3d -> GL). Two launch-only buttons: the Direct3D 7 immediate-mode cube
// and a pure-2D DirectDraw blit test.
static void show_ddraw_scenes(HWND frame) {
    destroy_content();
    g_cb_bench = 0;  // launch-only buttons
    SetWindowTextA(g_header, "Cube - DirectDraw (legacy DX5/6/7)");
    RECT cr;
    get_content_rect(frame, &cr);

    g_placeholder = CreateWindowA(
        "STATIC",
        "DirectDraw is the legacy DX5/6/7 path - DXVK does NOT handle it, so this\n"
        "exercises Wine's ddraw -> wined3d -> OpenGL stack instead (the path old games\n"
        "actually use). Each opens in a new window; press Esc to close it.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, cr.top, cr.right - cr.left, 70, frame, NULL,
        g_hinst, NULL);
    if (g_ui_font) SendMessage(g_placeholder, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    static const char *labels[] = {"Direct3D 7 cube (immediate mode)", "2D DirectDraw blit test"};
    static const char *args[] = {"dx7", "ddraw2d"};
    g_cbtn_n = (int)(sizeof(args) / sizeof(args[0]));
    int y = cr.top + 84;
    for (int i = 0; i < g_cbtn_n; i++) {
        g_cbtn_arg[i] = args[i];
        g_cbtn_label[i] = NULL;
        g_cbtn_proc[i] = NULL;
        g_cbtn_result[i] = NULL;
        g_cbtn_avg[i] = NULL;
        g_cbtn[i] = CreateWindowA("BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  cr.left, y, 260, 34, frame, (HMENU)(INT_PTR)(ID_CB_FIRST + i),
                                  g_hinst, NULL);
        if (g_ui_font) SendMessage(g_cbtn[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        y += 44;
    }
}

// Semaphore probe: benchmark the same heavy DXVK workload (instanced D3D11) with
// timeline vs binary semaphores, to measure the Turnip-kgsl timeline-semaphore
// regression. Reuses the benchmark buttons (poll + show result).
static void show_semaphore_probe(HWND frame) {
    destroy_content();
    g_cb_bench = 1;
    g_is_probe = 1;
    g_probe_avg[0] = g_probe_avg[1] = 0.0f;
    SetWindowTextA(g_header, "Semaphore Probe (DXVK / Turnip)");
    RECT cr;
    get_content_rect(frame, &cr);

    g_placeholder = CreateWindowA(
        "STATIC",
        "Benchmarks the instanced D3D11 cube (heavy DXVK load) twice: timeline vs binary\n"
        "semaphores. On Turnip-kgsl the timeline path can serialize the finish thread and\n"
        "roughly halve FPS. If the two runs differ below, your DXVK honors\n"
        "DXVK_DISABLE_TIMELINE_SEMAPHORES - i.e. a binary-semaphore-capable build.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, cr.top, cr.right - cr.left, 84, frame, NULL,
        g_hinst, NULL);
    if (g_ui_font) SendMessage(g_placeholder, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    static const char *labels[] = {"Timeline semaphores (15s)", "Binary semaphores (15s)"};
    static const char *args[] = {"dx11 --scene instanced --bench 15 --semaphore timeline",
                                 "dx11 --scene instanced --bench 15 --semaphore binary"};
    static const char *apilabels[] = {"DXVK Timeline", "DXVK Binary"};
    g_cbtn_n = 2;
    int y = cr.top + 92;
    for (int i = 0; i < g_cbtn_n; i++) {
        g_cbtn_arg[i] = args[i];
        g_cbtn_label[i] = apilabels[i];
        g_cbtn_proc[i] = NULL;
        g_cbtn[i] = CreateWindowA("BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  cr.left, y, 220, 32, frame, (HMENU)(INT_PTR)(ID_CB_FIRST + i),
                                  g_hinst, NULL);
        if (g_ui_font) SendMessage(g_cbtn[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        g_cbtn_avg[i] = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left + 232,
                                      y + 6, 86, 24, frame, NULL, g_hinst, NULL);
        if (g_ui_font_bold) SendMessage(g_cbtn_avg[i], WM_SETFONT, (WPARAM)g_ui_font_bold, TRUE);
        g_cbtn_result[i] =
            CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left + 320, y + 6,
                          (cr.right - (cr.left + 320)), 24, frame, NULL, g_hinst, NULL);
        if (g_ui_font) SendMessage(g_cbtn_result[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        y += 40;
    }
    // Verdict line (filled once both runs finish).
    g_verdict = CreateWindowA("STATIC", "Run both, then a verdict appears here.",
                              WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, y + 8,
                              cr.right - cr.left, 48, frame, NULL, g_hinst, NULL);
    if (g_ui_font_bold) SendMessage(g_verdict, WM_SETFONT, (WPARAM)g_ui_font_bold, TRUE);
    restore_cached_results();  // re-show probe results already run this session
}

static void layout_content(HWND frame) {
    RECT cr;
    get_content_rect(frame, &cr);
    RECT hr;
    GetClientRect(frame, &hr);
    MoveWindow(g_header, SB_W + 10, 8, hr.right - (SB_W + 10) - 10, HEADER_H - 4, TRUE);
    if (g_tab) {
        MoveWindow(g_tab, cr.left, cr.top, cr.right - cr.left, cr.bottom - cr.top, TRUE);
        RECT dr = cr;
        SendMessage(g_tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&dr);
        if (g_edit_vk) MoveWindow(g_edit_vk, dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top, TRUE);
        if (g_edit_gl) MoveWindow(g_edit_gl, dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top, TRUE);
    }
    if (g_placeholder)
        MoveWindow(g_placeholder, cr.left, cr.top, cr.right - cr.left, cr.bottom - cr.top, TRUE);
}

#define FOOT_A "Built with "
#define FOOT_B " for the Emulation Community"

// Center the footnote ("Built with [red heart] for the Emulation Community") in
// the reserved strip at the bottom of the window.
static void layout_footnote(HWND frame) {
    if (!g_foot_a) return;
    RECT rc;
    GetClientRect(frame, &rc);
    HDC hdc = GetDC(frame);
    HFONT old = (HFONT)SelectObject(hdc, g_ui_font);
    SIZE sa, sh, sb;
    GetTextExtentPoint32A(hdc, FOOT_A, (int)strlen(FOOT_A), &sa);
    GetTextExtentPoint32W(hdc, L"\u2665", 1, &sh);
    GetTextExtentPoint32A(hdc, FOOT_B, (int)strlen(FOOT_B), &sb);
    SelectObject(hdc, old);
    ReleaseDC(frame, hdc);
    int total = sa.cx + sh.cx + sb.cx;
    int x = (rc.right - total) / 2;
    if (x < 4) x = 4;
    int y = rc.bottom - FOOT_H + 4, h = 18;
    MoveWindow(g_foot_a, x, y, sa.cx + 2, h, TRUE);
    MoveWindow(g_foot_heart, x + sa.cx, y, sh.cx + 2, h, TRUE);
    MoveWindow(g_foot_b, x + sa.cx + sh.cx, y, sb.cx + 2, h, TRUE);
    if (g_version) {  // bottom-right corner
        SIZE sv;
        HDC h2 = GetDC(frame);
        HFONT o2 = (HFONT)SelectObject(h2, g_ui_font);
        GetTextExtentPoint32A(h2, AIO_VERSION, (int)strlen(AIO_VERSION), &sv);
        SelectObject(h2, o2);
        ReleaseDC(frame, h2);
        MoveWindow(g_version, rc.right - sv.cx - 10, y, sv.cx + 4, h, TRUE);
    }
}

// Launches a cube/benchmark in a new window. Returns the process handle (caller
// closes it) or NULL on failure.
static HANDLE launch_cube_window(const char *api) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char cmd[MAX_PATH + 40];
    snprintf(cmd, sizeof(cmd), "\"%s\" --cube %s", exe, api);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }
    return NULL;
}

// Launch benchmark row i (in its own process) and start polling for its result.
static void launch_bench_row(HWND frame, int i) {
    if (i < 0 || i >= g_cbtn_n) return;
    if (g_cbtn_proc[i]) CloseHandle(g_cbtn_proc[i]);
    char arg[160];
    if (g_bench_append)  // Benchmark view: append the chosen duration + vsync,
                         // and during a sweep auto-close the popup so it proceeds.
        snprintf(arg, sizeof(arg), "%s --bench %d%s%s", g_cbtn_arg[i], g_bench_secs,
                 g_vsync_ui ? " --vsync" : "", g_sweep_active ? " --autoclose 3" : "");
    else  // probe view: args already complete (--bench 15 --semaphore ...)
        snprintf(arg, sizeof(arg), "%s", g_cbtn_arg[i]);
    g_cbtn_proc[i] = launch_cube_window(arg);
    if (g_cbtn_avg[i]) SetWindowTextA(g_cbtn_avg[i], "");
    if (g_cbtn_result[i]) SetWindowTextA(g_cbtn_result[i], "running...");
    SetTimer(frame, 1, 500, NULL);
}

static void on_select(HWND frame, int action) {
    switch (action) {
        case AIO_MODE_EXIT:
            DestroyWindow(frame);
            break;
        case AIO_MODE_GPUINFO:
            show_gpuinfo(frame);
            break;
        case AIO_MODE_CUBE_VK: {
            HANDLE h = launch_cube_window("vk");
            if (h) CloseHandle(h);
            show_placeholder(frame, "Cube - Vulkan",
                             "Launched the Vulkan cube in a new window.\n\n"
                             "The menu stays here - switch back any time, or launch another test.");
            break;
        }
        case AIO_MODE_CUBE_GL: {
            HANDLE h = launch_cube_window("gl");
            if (h) CloseHandle(h);
            show_placeholder(frame, "Cube - OpenGL",
                             "Launched the OpenGL cube in a new window.\n\n"
                             "The menu stays here - switch back any time, or launch another test.");
            break;
        }
        case AIO_MODE_CUBE_DDRAW:
            show_ddraw_scenes(frame);  // pick D3D7 cube or 2D blit
            break;
        case AIO_MODE_CUBE_DX10: {
            HANDLE h = launch_cube_window("dx10");
            if (h) CloseHandle(h);
            show_placeholder(frame, "Cube - Direct3D 10",
                             "Launched the Direct3D 10 cube in a new window (tests the DXVK d3d10 path).\n\n"
                             "The menu stays here - switch back any time, or launch another test.");
            break;
        }
        case AIO_MODE_CUBE_DX11:
            show_dx11_scenes(frame);  // pick a scene from the DX11 test suite
            break;
        case AIO_MODE_CUBE_DX12: {
            HANDLE h = launch_cube_window("dx12");
            if (h) CloseHandle(h);
            show_placeholder(frame, "Cube - Direct3D 12",
                             "Launched the Direct3D 12 cube in a new window (tests the VKD3D path).\n\n"
                             "The menu stays here - switch back any time, or launch another test.");
            break;
        }
        case AIO_MODE_CUBE_DX9: {
            HANDLE h = launch_cube_window("dx9");
            if (h) CloseHandle(h);
            show_placeholder(frame, "Cube - Direct3D 9",
                             "Launched the Direct3D 9 cube in a new window (tests the DXVK d3d9 path).\n\n"
                             "The menu stays here - switch back any time, or launch another test.");
            break;
        }
        case AIO_MODE_CUBE_DX8: {
            HANDLE h = launch_cube_window("dx8");
            if (h) CloseHandle(h);
            show_placeholder(frame, "Cube - Direct3D 8",
                             "Launched the Direct3D 8 cube in a new window (DXVK d3d8 -> d3d9 path).\n\n"
                             "The menu stays here - switch back any time, or launch another test.");
            break;
        }
        case AIO_MODE_BENCH:
            show_benchmark(frame);
            break;
        case AIO_MODE_SEMAPHORE:
            show_semaphore_probe(frame);
            break;
        default:
            break;
    }
}

static LRESULT CALLBACK shell_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            for (int i = 0; i < NITEMS; i++) {
                g_sidebar[i] = CreateWindowA("BUTTON", g_items[i].label,
                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 6, 6 + i * 30,
                                             SB_W - 12, 26, hwnd, (HMENU)(INT_PTR)(ID_FIRST_BUTTON + i),
                                             g_hinst, NULL);
                if (g_ui_font) SendMessage(g_sidebar[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
            }
            g_header = CreateWindowA("STATIC", "AIO Graphics Test", WS_CHILD | WS_VISIBLE | SS_LEFT, SB_W + 10,
                                     8, 300, HEADER_H - 4, hwnd, NULL, g_hinst, NULL);
            if (g_header_font) SendMessage(g_header, WM_SETFONT, (WPARAM)g_header_font, TRUE);
            // Footnote ("Built with [heart] for the Emulation Community") + version.
            g_foot_a = CreateWindowA("STATIC", FOOT_A, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                                     hwnd, NULL, g_hinst, NULL);
            g_foot_heart = CreateWindowW(L"STATIC", L"\u2665", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0,
                                         10, 10, hwnd, NULL, g_hinst, NULL);
            g_foot_b = CreateWindowA("STATIC", FOOT_B, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                                     hwnd, NULL, g_hinst, NULL);
            g_version = CreateWindowA("STATIC", AIO_VERSION, WS_CHILD | WS_VISIBLE | SS_RIGHT, 0, 0,
                                      10, 10, hwnd, NULL, g_hinst, NULL);
            if (g_ui_font) {
                SendMessage(g_foot_a, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
                SendMessage(g_foot_heart, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
                SendMessage(g_foot_b, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
                SendMessage(g_version, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
            }
            layout_footnote(hwnd);
            show_placeholder(hwnd, "AIO Graphics Test",
                             "Select a test from the menu on the left.\n\n"
                             "GPU Info opens here; cube tests open in a new window.");
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == ID_RUN_ALL && g_cb_bench && g_cbtn_n > 0) {  // sequential sweep
                g_sweep_active = 1;
                g_sweep_idx = 0;
                launch_bench_row(hwnd, 0);
                return 0;
            }
            if (id >= ID_DUR_FIRST && id < ID_DUR_FIRST + 4) {  // benchmark length
                static const int durs[4] = {15, 30, 45, 60};
                g_bench_secs = durs[id - ID_DUR_FIRST];
                if (g_dur_label) {
                    char dl[32];
                    snprintf(dl, sizeof(dl), "Length (%ds):", g_bench_secs);
                    SetWindowTextA(g_dur_label, dl);
                }
                return 0;
            }
            if (id == ID_VSYNC) {  // vsync toggle
                g_vsync_ui = (SendMessage(g_vsync_chk, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                return 0;
            }
            int cb = id - ID_CB_FIRST;
            if (cb >= 0 && cb < g_cbtn_n) {  // content-area buttons
                if (g_cb_bench) {            // Benchmark/probe view: poll for a result file
                    g_sweep_active = 0;      // a manual click cancels any sweep
                    launch_bench_row(hwnd, cb);
                } else {  // Scene picker: fire-and-forget launch in a new window
                    HANDLE h = launch_cube_window(g_cbtn_arg[cb]);
                    if (h) CloseHandle(h);
                }
                return 0;
            }
            int idx = id - ID_FIRST_BUTTON;
            if (idx >= 0 && idx < NITEMS) on_select(hwnd, g_items[idx].action);
            return 0;
        }
        case WM_NOTIFY: {
            LPNMHDR nh = (LPNMHDR)lParam;
            if (g_tab && nh->hwndFrom == g_tab && nh->code == TCN_SELCHANGE) {
                int sel = (int)SendMessage(g_tab, TCM_GETCURSEL, 0, 0);
                ShowWindow(g_edit_vk, sel == 0 ? SW_SHOW : SW_HIDE);
                ShowWindow(g_edit_gl, sel == 1 ? SW_SHOW : SW_HIDE);
            }
            return 0;
        }
        case WM_TIMER: {
            // Poll running benchmark processes; when one exits, show its result.
            for (int i = 0; i < g_cbtn_n; i++) {
                if (g_cbtn_proc[i] && WaitForSingleObject(g_cbtn_proc[i], 0) == WAIT_OBJECT_0) {
                    CloseHandle(g_cbtn_proc[i]);
                    g_cbtn_proc[i] = NULL;
                    char path[160], buf[256];
                    snprintf(path, sizeof(path), "AIO-Graphics-Test_bench_%s.txt", g_cbtn_label[i]);
                    FILE *f = fopen(path, "r");
                    if (f) {
                        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                        buf[n] = '\0';
                        fclose(f);
                        // File is "avg|min|max" FPS. Show bold "Avg N" + "Min N   Max N".
                        float a = 0, mn = 0, mx = 0;
                        char avgtxt[48], mmtxt[96];
                        if (sscanf(buf, "%f|%f|%f", &a, &mn, &mx) == 3) {
                            snprintf(avgtxt, sizeof(avgtxt), "Avg %.0f", a);
                            snprintf(mmtxt, sizeof(mmtxt), "Min %.0f   Max %.0f", mn, mx);
                        } else {
                            avgtxt[0] = '\0';
                            snprintf(mmtxt, sizeof(mmtxt), "%s", buf);
                        }
                        if (g_cbtn_avg[i]) SetWindowTextA(g_cbtn_avg[i], avgtxt);
                        if (g_cbtn_result[i]) SetWindowTextA(g_cbtn_result[i], mmtxt);
                        // Cache the result so it survives switching content views.
                        if (a > 0.0f) cache_store(g_cbtn_label[i], a, avgtxt, mmtxt);
                        // Semaphore probe: record avg, and once both runs are in,
                        // judge the timeline-vs-binary result.
                        if (g_is_probe && i < 2 && a > 0.0f) {
                            g_probe_avg[i] = a;
                            float tl = g_probe_avg[0], bn = g_probe_avg[1];
                            if (tl > 0.0f && bn > 0.0f && g_verdict) {
                                char vtxt[160];
                                probe_verdict(tl, bn, vtxt, sizeof(vtxt));
                                SetWindowTextA(g_verdict, vtxt);
                            }
                        }
                    } else if (g_cbtn_result[i]) {
                        SetWindowTextA(g_cbtn_result[i], "(no result file)");
                    }
                    // Run-All sweep: when the current row finishes, start the next.
                    if (g_sweep_active && i == g_sweep_idx) {
                        g_sweep_idx++;
                        if (g_sweep_idx < g_cbtn_n)
                            launch_bench_row(hwnd, g_sweep_idx);
                        else
                            g_sweep_active = 0;  // sweep complete
                    }
                }
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HWND ctl = (HWND)lParam;
            if (ctl == g_foot_heart || ctl == g_foot_a || ctl == g_foot_b || ctl == g_version) {
                HDC hdc = (HDC)wParam;
                SetTextColor(hdc, ctl == g_foot_heart ? RGB(214, 69, 79) : RGB(112, 112, 120));
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            break;
        }
        case WM_SIZE:
            layout_content(hwnd);
            layout_footnote(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int aio_run_shell(HINSTANCE hInstance) {
    g_hinst = hInstance;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_ui_font = CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_ui_font_bold = CreateFontA(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_header_font = CreateFontA(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_mono_font = CreateFontA(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                              FIXED_PITCH | FF_MODERN, "Consolas");

    const char *cls = "AIOGraphicsTestShell";
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = shell_wndproc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconA(hInstance, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassA(&wc);

    int w = 840, h = 800;  // tall enough for the full benchmark list (17 rows)
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;

    HWND hwnd = CreateWindowA(cls, "AIO Graphics Test", WS_OVERLAPPEDWINDOW, sx, sy, w, h, NULL, NULL,
                              hInstance, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (IsDialogMessage(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_ui_font) DeleteObject(g_ui_font);
    if (g_header_font) DeleteObject(g_header_font);
    if (g_mono_font) DeleteObject(g_mono_font);
    UnregisterClassA(cls, hInstance);
    return 0;
}
