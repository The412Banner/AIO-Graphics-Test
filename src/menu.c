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

// Dark theme palette + brushes (created in aio_run_shell, used by WM_CTLCOLOR*).
#define DARK_BG    RGB(26, 28, 33)    // window / panel background
#define DARK_CTL   RGB(38, 41, 48)    // control (edit/readout) background
#define DARK_TEXT  RGB(228, 230, 234) // primary text
#define DARK_DIM   RGB(150, 156, 166) // dim / secondary text
static HBRUSH g_br_bg;
static HBRUSH g_br_ctl;
static WNDPROC g_btn_oldproc;  // saved system BUTTON proc (buttons are subclassed for dark paint)
static WNDPROC g_edit_oldproc;  // saved system EDIT proc (report edits subclassed for a dark scrollbar)

// Content views.
static HWND g_tab;       // (unused — GPU Info is now two stacked panes, no tabs)
static HWND g_cap_vk;    // GPU Info: "Vulkan" caption (top pane)
static HWND g_cap_gl;    // GPU Info: "OpenGL" caption (bottom pane)
static HWND g_edit_vk;   // GPU Info: Vulkan report (top, scrollable)
static HWND g_edit_gl;   // GPU Info: OpenGL report (bottom, scrollable)
static HWND g_placeholder;

#define ID_CB_FIRST 3000  // content-area buttons (Benchmark + scene-picker views)
#define MAX_CB 40
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
static int g_sweep_active;    // sequential run-selected sweep in progress
#define ID_DUR_FIRST 3600     // duration buttons (15/30/45/60 s)
#define ID_VSYNC 3620
static HWND g_dur_btn[4];
static HWND g_dur_label;
static HWND g_vsync_chk;
static int g_bench_secs = 15;   // selected benchmark duration
static int g_vsync_ui = 0;      // vsync toggle state
static int g_bench_append = 0;  // launch_bench_row appends --bench/--vsync (Benchmark view)

// Benchmark selection + collapsible D3D11 group (Benchmark view only).
#define ID_CHK_FIRST 3700     // per-row selection checkbox (ID_CHK_FIRST + row index)
#define ID_D3D11_HDR 3760     // "Direct3D 11" group select-all checkbox
#define ID_D3D11_EXPAND 3761  // D3D11 expand/collapse toggle
static int g_cbtn_sel[MAX_CB];        // per-row selection state (source of truth, survives rebuild)
static int g_cbtn_d3d11[MAX_CB];      // 1 = this row is a D3D11 sub-scene (grid cell)
static const char *g_cbtn_short[MAX_CB];  // short label (grid cells show "<short>  <avg>")
static HWND g_d3d11_chk, g_d3d11_btn;     // D3D11 group select-all checkbox + expand toggle
static HWND g_results_btn;                 // "Benchmark Results" button (Benchmark view)
static HWND g_draw_label;                  // "Draw stress:" sub-label (picker + bench group)
#define ID_SHOW_RESULTS 3762
#define ID_RESULTS_BACK 3763
#define ID_RESULTS_COMBO 3764
static HWND g_results_combo;               // run picker on the Results screen
static int g_results_sel = 0;              // selected run: 0 = current session, 1.. = history
static char g_run_ts[24];                  // timestamp of the in-progress Run-Selected sweep
static int g_d3d11_expanded = 1;          // is the D3D11 grid shown?

// Persisted benchmark run history (survives app restarts).
#define AIO_HIST_FILE "AIO-Graphics-Test_history.txt"
#define MAX_HIST_RUNS 40
#define MAX_HIST_ROWS 24
typedef struct {
    char label[40];
    float avg, mn, mx;
} HistRow;
typedef struct {
    char ts[24];
    int secs;
    HistRow rows[MAX_HIST_ROWS];
    int nrows;
} HistRun;
static HistRun g_hist[MAX_HIST_RUNS];  // loaded oldest..newest
static int g_hist_n;
static int g_run_list[MAX_CB], g_run_n, g_run_pos;  // "Run Selected" sweep list
#define ID_SELECT_ALL 3765
static HWND g_selall_btn;  // "Select All" / "Clear All" toggle (Benchmark view)
#define ID_DX11_DEMOS 3766  // "Demo Scenes ->" button (DX11 feature picker)
#define ID_DX11_BACK 3767   // "<- Back" button (DX11 demo-scenes picker)

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
    if (g_cap_vk) { DestroyWindow(g_cap_vk); g_cap_vk = NULL; }
    if (g_cap_gl) { DestroyWindow(g_cap_gl); g_cap_gl = NULL; }
    if (g_tab) { DestroyWindow(g_tab); g_tab = NULL; }
    if (g_placeholder) { DestroyWindow(g_placeholder); g_placeholder = NULL; }
    if (g_verdict) { DestroyWindow(g_verdict); g_verdict = NULL; }
    if (g_run_all) { DestroyWindow(g_run_all); g_run_all = NULL; }
    if (g_dur_label) { DestroyWindow(g_dur_label); g_dur_label = NULL; }
    if (g_vsync_chk) { DestroyWindow(g_vsync_chk); g_vsync_chk = NULL; }
    if (g_d3d11_chk) { DestroyWindow(g_d3d11_chk); g_d3d11_chk = NULL; }
    if (g_d3d11_btn) { DestroyWindow(g_d3d11_btn); g_d3d11_btn = NULL; }
    if (g_results_btn) { DestroyWindow(g_results_btn); g_results_btn = NULL; }
    if (g_selall_btn) { DestroyWindow(g_selall_btn); g_selall_btn = NULL; }
    if (g_results_combo) { DestroyWindow(g_results_combo); g_results_combo = NULL; }
    if (g_draw_label) { DestroyWindow(g_draw_label); g_draw_label = NULL; }
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
#define MAX_CACHE 32
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

// --- persisted run history ---
// Append a "RUN <timestamp> <secs>" header at the start of a Run-Selected sweep,
// then one "<label> <avg> <min> <max>" line per result as it completes. Tab-sep.
static void hist_append_run_header(const char *ts, int secs) {
    FILE *f = fopen(AIO_HIST_FILE, "a");
    if (f) {
        fprintf(f, "RUN\t%s\t%d\n", ts, secs);
        fclose(f);
    }
}
static void hist_append_row(const char *label, float a, float mn, float mx) {
    FILE *f = fopen(AIO_HIST_FILE, "a");
    if (f) {
        fprintf(f, "%s\t%.1f\t%.1f\t%.1f\n", label, a, mn, mx);
        fclose(f);
    }
}
// Load the history file into g_hist[] (keeps the newest MAX_HIST_RUNS runs).
static void hist_load(void) {
    g_hist_n = 0;
    FILE *f = fopen(AIO_HIST_FILE, "r");
    if (!f) return;
    char line[256];
    HistRun *cur = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "RUN\t", 4) == 0) {
            if (g_hist_n >= MAX_HIST_RUNS) {  // drop the oldest, keep the newest
                memmove(&g_hist[0], &g_hist[1], (MAX_HIST_RUNS - 1) * sizeof(HistRun));
                g_hist_n = MAX_HIST_RUNS - 1;
            }
            cur = &g_hist[g_hist_n++];
            memset(cur, 0, sizeof(*cur));
            char *ts = line + 4, *tab = strchr(ts, '\t');
            if (tab) {
                *tab = '\0';
                cur->secs = atoi(tab + 1);
            }
            snprintf(cur->ts, sizeof(cur->ts), "%s", ts);
        } else if (cur && cur->nrows < MAX_HIST_ROWS) {
            char *t1 = strchr(line, '\t');
            if (!t1) continue;
            *t1 = '\0';
            float a = 0, mn = 0, mx = 0;
            if (sscanf(t1 + 1, "%f\t%f\t%f", &a, &mn, &mx) == 3) {
                HistRow *r = &cur->rows[cur->nrows++];
                snprintf(r->label, sizeof(r->label), "%s", line);
                r->avg = a;
                r->mn = mn;
                r->mx = mx;
            }
        }
    }
    fclose(f);
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

// Draw a filled triangle (scrollbar arrow).
static void dark_tri(HDC dc, int cx, int cy, int d, int up, COLORREF c) {
    POINT p[3];
    if (up) { p[0].x = cx; p[0].y = cy - d; p[1].x = cx - d; p[1].y = cy + d; p[2].x = cx + d; p[2].y = cy + d; }
    else    { p[0].x = cx; p[0].y = cy + d; p[1].x = cx - d; p[1].y = cy - d; p[2].x = cx + d; p[2].y = cy - d; }
    HBRUSH b = CreateSolidBrush(c);
    HPEN pn = CreatePen(PS_SOLID, 1, c);
    HBRUSH ob = (HBRUSH)SelectObject(dc, b);
    HPEN op = (HPEN)SelectObject(dc, pn);
    Polygon(dc, p, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(pn);
}

// Overpaint the EDIT's (light, system) vertical scrollbar with a flat dark one.
static void paint_dark_vscroll(HWND e) {
    if (!IsWindowVisible(e)) return;
    RECT wr;
    GetWindowRect(e, &wr);
    int W = wr.right - wr.left, H = wr.bottom - wr.top;
    int sbw = GetSystemMetrics(SM_CXVSCROLL);
    if (sbw <= 0 || H <= 2 * sbw) return;
    HDC dc = GetWindowDC(e);
    RECT track = {W - sbw, sbw, W, H - sbw};
    HBRUSH tb = CreateSolidBrush(DARK_CTL);
    FillRect(dc, &track, tb);
    DeleteObject(tb);
    RECT up = {W - sbw, 0, W, sbw}, dn = {W - sbw, H - sbw, W, H};
    HBRUSH ab = CreateSolidBrush(RGB(48, 52, 60));
    FillRect(dc, &up, ab);
    FillRect(dc, &dn, ab);
    DeleteObject(ab);
    int cx = W - sbw / 2;
    dark_tri(dc, cx, sbw / 2, sbw / 5, 1, DARK_TEXT);
    dark_tri(dc, cx, H - sbw / 2, sbw / 5, 0, DARK_TEXT);
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    if (GetScrollInfo(e, SB_VERT, &si)) {
        int trackH = H - 2 * sbw;
        int range = si.nMax - si.nMin;
        if (range < 1) range = 1;
        int page = si.nPage > 0 ? (int)si.nPage : 1;
        int thumbH = (int)((double)trackH * (double)page / (double)(range + 1));
        if (thumbH < 18) thumbH = 18;
        if (thumbH > trackH) thumbH = trackH;
        int denom = range - page + 1;
        if (denom < 1) denom = 1;
        int thumbY = sbw + (int)((double)(trackH - thumbH) * (double)(si.nPos - si.nMin) / (double)denom);
        RECT th = {W - sbw + 2, thumbY, W - 2, thumbY + thumbH};
        HBRUSH hb = CreateSolidBrush(RGB(96, 102, 112));
        FillRect(dc, &th, hb);
        DeleteObject(hb);
    }
    ReleaseDC(e, dc);
}

static LRESULT CALLBACK edit_subproc(HWND e, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCPAINT || m == WM_VSCROLL || m == WM_MOUSEWHEEL || m == WM_KEYUP ||
        m == WM_LBUTTONUP || m == WM_NCMOUSEMOVE || m == WM_NCLBUTTONDOWN ||
        m == WM_NCLBUTTONUP || m == WM_NCMOUSELEAVE) {
        LRESULT r = CallWindowProcA(g_edit_oldproc, e, m, w, l);
        paint_dark_vscroll(e);  // overpaint the system scrollbar dark after it (re)draws on hover/drag
        return r;
    }
    return CallWindowProcA(g_edit_oldproc, e, m, w, l);
}

static HWND make_report_edit(HWND frame, const RECT *r, const char *text) {
    HWND e = CreateWindowA("EDIT", "",
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                           r->left, r->top, r->right - r->left, r->bottom - r->top, frame, NULL,
                           g_hinst, NULL);
    SendMessage(e, EM_SETLIMITTEXT, 0, 0);
    if (g_mono_font) SendMessage(e, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
    if (text) SetWindowTextA(e, text);
    WNDPROC old = (WNDPROC)SetWindowLongPtrA(e, GWLP_WNDPROC, (LONG_PTR)edit_subproc);
    if (!g_edit_oldproc) g_edit_oldproc = old;
    paint_dark_vscroll(e);  // dark scrollbar from the first frame (initial NC paint already ran light)
    return e;
}

// GPU Info layout: two stacked panes (Vulkan on top, OpenGL below), each a
// caption + an independently-scrollable report edit, split 50/50. No tabs.
static void layout_gpuinfo(const RECT *cr) {
    if (!g_edit_vk || !g_edit_gl) return;
    int x = cr->left, w = cr->right - cr->left, H = cr->bottom - cr->top;
    int capH = 22, gap = 8;
    int editH = (H - 2 * capH - gap) / 2;
    if (editH < 40) editH = 40;
    int y = cr->top;
    MoveWindow(g_cap_vk, x, y, w, capH, TRUE);   y += capH;
    MoveWindow(g_edit_vk, x, y, w, editH, TRUE); y += editH + gap;
    MoveWindow(g_cap_gl, x, y, w, capH, TRUE);   y += capH;
    MoveWindow(g_edit_gl, x, y, w, editH, TRUE);
}

static void show_gpuinfo(HWND frame) {
    destroy_content();
    SetWindowTextA(g_header, "GPU Info  (Vulkan / OpenGL)");

    RECT cr;
    get_content_rect(frame, &cr);

    g_cap_vk = CreateWindowA("STATIC", "Vulkan", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                             frame, NULL, g_hinst, NULL);
    g_cap_gl = CreateWindowA("STATIC", "OpenGL", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                             frame, NULL, g_hinst, NULL);
    if (g_ui_font_bold) {
        SendMessage(g_cap_vk, WM_SETFONT, (WPARAM)g_ui_font_bold, TRUE);
        SendMessage(g_cap_gl, WM_SETFONT, (WPARAM)g_ui_font_bold, TRUE);
    }

    char *vk = aio_gpuinfo_build_vk_text();
    char *gl = aio_gpuinfo_build_gl_text();
    g_edit_vk = make_report_edit(frame, &cr, vk ? vk : "(no Vulkan data)");
    g_edit_gl = make_report_edit(frame, &cr, gl ? gl : "(no OpenGL data)");
    if (vk) free(vk);
    if (gl) free(gl);

    layout_gpuinfo(&cr);  // both panes visible + stacked
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

// The benchmark rows. The contiguous run with d3d11==1 is the collapsible group;
// .label is what each row/grid cell shows, .apilabel MUST match the label each
// backend passes to aio_bench_finish (so result files + cache line up).
typedef struct {
    const char *label, *arg, *apilabel;
    int d3d11;
} BenchRow;
static const BenchRow kBenchRows[] = {
    {"Vulkan", "vk", "Vulkan", 0},
    {"VK: Phong", "vk --scene phong", "Vulkan Phong", 0},
    {"OpenGL", "gl", "OpenGL", 0},
    {"DDraw: D3D7", "dx7", "Direct3D 7 (DirectDraw)", 0},
    {"DDraw: 2D Blit", "ddraw2d", "DirectDraw 2D", 0},
    {"D3D8: Cube", "dx8", "Direct3D 8", 0},
    {"D3D9: Cube", "dx9", "Direct3D 9", 0},
    {"D3D10: Cube", "dx10", "Direct3D 10", 0},
    {"Cube", "dx11 --scene spin", "D3D11 Cube", 1},
    {"Instanced", "dx11 --scene instanced", "D3D11 Instanced", 1},
    {"Tessellate", "dx11 --scene tess", "D3D11 Tessellation", 1},
    {"Compute", "dx11 --scene compute", "D3D11 Compute Particles", 1},
    {"Dolphin", "dx11 --scene dolphin", "D3D11 Dolphin", 1},
    {"Raymarch", "dx11 --scene raymarch", "D3D11 Raymarch SDF", 1},
    {"Ocean", "dx11 --scene ocean", "D3D11 Ocean", 1},
    {"Ocean v2", "dx11 --scene ocean2", "D3D11 Ocean v2", 1},
    {"Mandelbulb", "dx11 --scene mandelbulb", "D3D11 Mandelbulb", 1},
    {"Nebula", "dx11 --scene nebula", "D3D11 Nebula", 1},
    {"Nebula HD", "dx11 --scene nebula2", "D3D11 Nebula (detailed)", 1},
    {"Showcase", "dx11 --scene showcase", "D3D11 Showcase", 1},
    {"Space", "dx11 --scene space", "D3D11 Space", 1},
    {"Desert", "dx11 --scene desert", "D3D11 Desert", 1},
    {"Cityscape", "dx11 --scene city", "D3D11 Cityscape", 1},
    {"GS Explode", "dx11 --scene gsexplode", "D3D11 GS Exploder", 1},
    {"Cel", "dx11 --scene cel", "D3D11 Cel Shading", 1},
    {"Matcap", "dx11 --scene matcap", "D3D11 Matcap", 1},
    {"Atomics", "dx11 --scene atomics", "D3D11 Atomics", 1},
    {"Draw 128", "dx11 --scene drawstress --draws 128", "D3D11 Draw 128", 1},
    {"Draw 256", "dx11 --scene drawstress --draws 256", "D3D11 Draw 256", 1},
    {"Draw 512", "dx11 --scene drawstress --draws 512", "D3D11 Draw 512", 1},
    {"Draw 1024", "dx11 --scene drawstress --draws 1024", "D3D11 Draw 1024", 1},
    {"Draw 2048", "dx11 --scene drawstress --draws 2048", "D3D11 Draw 2048", 1},
    {"D3D12: Cube", "dx12", "Direct3D 12", 0},
};
#define N_BENCH_ROWS ((int)(sizeof(kBenchRows) / sizeof(kBenchRows[0])))

// A full-width benchmark row: a selection checkbox (label) + Avg + Min/Max labels.
static void make_bench_top_row(HWND frame, int i, RECT *cr, int *py) {
    int y = *py;
    g_cbtn[i] = CreateWindowA("BUTTON", g_cbtn_short[i], WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              cr->left, y + 2, 200, 22, frame, (HMENU)(INT_PTR)(ID_CHK_FIRST + i),
                              g_hinst, NULL);
    if (g_ui_font) SendMessage(g_cbtn[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessage(g_cbtn[i], BM_SETCHECK, g_cbtn_sel[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    g_cbtn_avg[i] = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT, cr->left + 212, y + 6,
                                  86, 24, frame, NULL, g_hinst, NULL);
    if (g_ui_font_bold) SendMessage(g_cbtn_avg[i], WM_SETFONT, (WPARAM)g_ui_font_bold, TRUE);
    g_cbtn_result[i] =
        CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT, cr->left + 300, y + 6,
                      (cr->right - (cr->left + 300)), 24, frame, NULL, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_cbtn_result[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    *py = y + 34;
}

static void show_benchmark(HWND frame) {
    destroy_content();
    g_cb_bench = 1;  // these buttons run a benchmark and poll for a result file
    SetWindowTextA(g_header, "Benchmark");
    RECT cr;
    get_content_rect(frame, &cr);

    g_bench_append = 1;  // rows get --bench <secs> [+ --vsync] appended at launch
    g_placeholder = CreateWindowA(
        "STATIC", "Tick the tests to run, then Run Selected.", WS_CHILD | WS_VISIBLE | SS_LEFT,
        cr.left, cr.top, cr.right - cr.left - 330, 22, frame, NULL, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_placeholder, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    // Top-right: open the Results screen, and run the selected rows sequentially.
    g_results_btn = CreateWindowA("BUTTON", "Benchmark Results", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  cr.right - 320, cr.top, 125, 32, frame,
                                  (HMENU)(INT_PTR)ID_SHOW_RESULTS, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_results_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    g_run_all = CreateWindowA("BUTTON", "Run Selected", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
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

    // Select All / Clear All toggle: label reflects whether everything is already ticked.
    int all_sel = 1;
    for (int k = 0; k < N_BENCH_ROWS; k++)
        if (!g_cbtn_sel[k]) { all_sel = 0; break; }
    g_selall_btn = CreateWindowA("BUTTON", all_sel ? "Clear All" : "Select All",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, cr.right - 120, cr.top + 46,
                                 120, 26, frame, (HMENU)(INT_PTR)ID_SELECT_ALL, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_selall_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    // Nothing is selected by default; the user ticks tests (or uses Select All).
    // g_cbtn_sel[] is the persistent source of truth (survives expand/collapse + nav).
    g_cbtn_n = N_BENCH_ROWS;
    int firstD = -1, nD = 0;
    for (int i = 0; i < g_cbtn_n; i++) {
        g_cbtn_arg[i] = kBenchRows[i].arg;
        g_cbtn_label[i] = kBenchRows[i].apilabel;
        g_cbtn_short[i] = kBenchRows[i].label;
        g_cbtn_d3d11[i] = kBenchRows[i].d3d11;
        g_cbtn_proc[i] = NULL;
        g_cbtn[i] = g_cbtn_avg[i] = g_cbtn_result[i] = NULL;
        if (kBenchRows[i].d3d11) {
            if (firstD < 0) firstD = i;
            nD++;
        }
    }

    int y = cr.top + 86;
    int i = 0;
    for (; i < firstD; i++) make_bench_top_row(frame, i, &cr, &y);  // rows before D3D11

    // Collapsible D3D11 group: select-all checkbox + an expand/collapse toggle.
    int allD = 1;
    for (int k = firstD; k < firstD + nD; k++)
        if (!g_cbtn_sel[k]) allD = 0;
    g_d3d11_chk = CreateWindowA("BUTTON", "Direct3D 11", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                cr.left, y + 2, 150, 22, frame, (HMENU)(INT_PTR)ID_D3D11_HDR, g_hinst,
                                NULL);
    if (g_ui_font) SendMessage(g_d3d11_chk, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessage(g_d3d11_chk, BM_SETCHECK, allD ? BST_CHECKED : BST_UNCHECKED, 0);
    g_d3d11_btn = CreateWindowA("BUTTON", g_d3d11_expanded ? "Hide scenes" : "Show scenes",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, cr.left + 156, y, 110, 26, frame,
                                (HMENU)(INT_PTR)ID_D3D11_EXPAND, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_d3d11_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    y += 34;

    if (g_d3d11_expanded) {
        // 3-column grid of the normal D3D11 scenes; the draw-stress counts are
        // pulled out into a compact "Draw Stress:" sub-row below (else they'd add
        // many rows and overflow).
        int cell = 0, firstDraw = -1;
        for (int k = 0; k < nD; k++) {
            int idx = firstD + k;
            if (strstr(kBenchRows[idx].arg, "drawstress")) {
                if (firstDraw < 0) firstDraw = idx;
                continue;
            }
            int col = cell % 3, row = cell / 3;
            int cx = cr.left + 24 + col * 150, cy = y + row * 26;
            g_cbtn[idx] =
                CreateWindowA("BUTTON", kBenchRows[idx].label, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              cx, cy, 145, 22, frame, (HMENU)(INT_PTR)(ID_CHK_FIRST + idx), g_hinst,
                              NULL);
            if (g_ui_font) SendMessage(g_cbtn[idx], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
            SendMessage(g_cbtn[idx], BM_SETCHECK, g_cbtn_sel[idx] ? BST_CHECKED : BST_UNCHECKED, 0);
            cell++;
        }
        y += ((cell + 2) / 3) * 26 + 6;
        if (firstDraw >= 0) {  // Draw Stress sub-checkboxes (which counts to run)
            static const char *counts[] = {"128", "256", "512", "1024", "2048"};
            g_draw_label = CreateWindowA("STATIC", "Draw Stress:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         cr.left + 24, y + 2, 88, 22, frame, NULL, g_hinst, NULL);
            if (g_ui_font) SendMessage(g_draw_label, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
            int dx = cr.left + 116, ci = 0;
            for (int idx = firstDraw; idx < firstD + nD; idx++) {
                if (!strstr(kBenchRows[idx].arg, "drawstress")) continue;
                g_cbtn[idx] = CreateWindowA("BUTTON", counts[ci], WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                            dx, y, 66, 22, frame, (HMENU)(INT_PTR)(ID_CHK_FIRST + idx),
                                            g_hinst, NULL);
                if (g_ui_font) SendMessage(g_cbtn[idx], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
                SendMessage(g_cbtn[idx], BM_SETCHECK, g_cbtn_sel[idx] ? BST_CHECKED : BST_UNCHECKED, 0);
                dx += 70;
                ci++;
            }
            y += 28;
        }
    }

    for (i = firstD + nD; i < g_cbtn_n; i++) make_bench_top_row(frame, i, &cr, &y);  // D3D12 etc.
    restore_cached_results();  // re-show results already run this session
}

// One result line on the Results screen: label + bold "Avg N" + "Min N  Max N".
static void make_result_row(HWND frame, RECT *cr, int *py, int i, const char *label,
                            const char *avg, const char *mm) {
    int y = *py;
    g_cbtn_label[i] = label;
    g_cbtn[i] = CreateWindowA("STATIC", label, WS_CHILD | WS_VISIBLE | SS_LEFT, cr->left, y, 220, 22,
                              frame, NULL, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_cbtn[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    g_cbtn_avg[i] = CreateWindowA("STATIC", avg, WS_CHILD | WS_VISIBLE | SS_LEFT, cr->left + 230, y, 90,
                                  22, frame, NULL, g_hinst, NULL);
    if (g_ui_font_bold) SendMessage(g_cbtn_avg[i], WM_SETFONT, (WPARAM)g_ui_font_bold, TRUE);
    g_cbtn_result[i] = CreateWindowA("STATIC", mm, WS_CHILD | WS_VISIBLE | SS_LEFT, cr->left + 326, y,
                                     cr->right - (cr->left + 326), 22, frame, NULL, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_cbtn_result[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    *py = y + 28;
}

// Benchmark Results screen: a run picker (current session + timestamped previous
// runs loaded from disk) and the selected run's full Avg/Min/Max per test.
static void show_bench_results(HWND frame) {
    destroy_content();
    g_cb_bench = 0;  // static results: no checkbox/sweep/poll logic
    SetWindowTextA(g_header, "Benchmark Results");
    RECT cr;
    get_content_rect(frame, &cr);
    hist_load();

    g_run_all = CreateWindowA("BUTTON", "< Back to Benchmark", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              cr.right - 190, cr.top, 190, 32, frame, (HMENU)(INT_PTR)ID_RESULTS_BACK,
                              g_hinst, NULL);
    if (g_ui_font) SendMessage(g_run_all, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    g_placeholder = CreateWindowA(
        "STATIC", "Pick a run to view. Per-frame data is in AIO-Graphics-Test_bench.csv.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, cr.top, cr.right - cr.left - 200, 22, frame, NULL,
        g_hinst, NULL);
    if (g_ui_font) SendMessage(g_placeholder, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    // Run picker: "Current session" + each saved run, newest first.
    g_results_combo =
        CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, cr.left,
                      cr.top + 30, 340, 260, frame, (HMENU)(INT_PTR)ID_RESULTS_COMBO, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_results_combo, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageA(g_results_combo, CB_ADDSTRING, 0, (LPARAM) "Current session (live)");
    for (int r = g_hist_n - 1; r >= 0; r--) {
        char it[80];
        snprintf(it, sizeof(it), "%s   (%ds, %d tests)", g_hist[r].ts, g_hist[r].secs, g_hist[r].nrows);
        SendMessageA(g_results_combo, CB_ADDSTRING, 0, (LPARAM)it);
    }
    if (g_results_sel < 0 || g_results_sel > g_hist_n) g_results_sel = 0;
    SendMessage(g_results_combo, CB_SETCURSEL, g_results_sel, 0);

    int y = cr.top + 72;
    g_cbtn_n = 0;
    if (g_results_sel == 0) {  // current session, from the in-memory cache (canonical order)
        for (int r = 0; r < N_BENCH_ROWS && g_cbtn_n < MAX_CB; r++) {
            int ci = cache_find(kBenchRows[r].apilabel);
            if (ci < 0) continue;
            make_result_row(frame, &cr, &y, g_cbtn_n, kBenchRows[r].apilabel, g_cache[ci].avg,
                            g_cache[ci].mm);
            g_cbtn_n++;
        }
    } else {  // a saved run (sel 1 = newest = g_hist[g_hist_n-1])
        HistRun *run = &g_hist[g_hist_n - g_results_sel];
        for (int r = 0; r < run->nrows && g_cbtn_n < MAX_CB; r++) {
            char avg[32], mm[48];
            snprintf(avg, sizeof(avg), "Avg %.0f", run->rows[r].avg);
            snprintf(mm, sizeof(mm), "Min %.0f   Max %.0f", run->rows[r].mn, run->rows[r].mx);
            make_result_row(frame, &cr, &y, g_cbtn_n, run->rows[r].label, avg, mm);
            g_cbtn_n++;
        }
    }
    if (g_cbtn_n == 0) {
        g_verdict = CreateWindowA(
            "STATIC",
            g_results_sel == 0 ? "No benchmarks have been run yet this session." : "(empty run)",
            WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, y, cr.right - cr.left, 22, frame, NULL, g_hinst,
            NULL);
        if (g_ui_font) SendMessage(g_verdict, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    }
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

    // Feature / pipeline tests. The visual showpieces live under "Demo Scenes".
    static const char *labels[] = {"Spinning cube",        "Textured cube",
                                   "Instanced (512 cubes)", "Tessellation (sphere)",
                                   "Compute particles",     "GS mesh exploder",
                                   "Atomics (histogram)",   "Dolphin (swim)",
                                   "Draw stress 128",       "Draw stress 256",
                                   "Draw stress 512",       "Draw stress 1024",
                                   "Draw stress 2048"};
    static const char *args[] = {"dx11 --scene spin",      "dx11 --scene textured",
                                 "dx11 --scene instanced", "dx11 --scene tess",
                                 "dx11 --scene compute",   "dx11 --scene gsexplode",
                                 "dx11 --scene atomics",   "dx11 --scene dolphin",
                                 "dx11 --scene drawstress --draws 128",
                                 "dx11 --scene drawstress --draws 256",
                                 "dx11 --scene drawstress --draws 512",
                                 "dx11 --scene drawstress --draws 1024",
                                 "dx11 --scene drawstress --draws 2048"};
    static const char *counts[] = {"128", "256", "512", "1024", "2048"};
    g_cbtn_n = (int)(sizeof(args) / sizeof(args[0]));
    int y = cr.top + 70;
    const int n_full = g_cbtn_n - 5;  // feature buttons; last 5 = draw counts
    const int colw = 220, rowh = 38;
    int i = 0;
    for (; i < n_full; i++) {  // feature tests as launch buttons in a 2-column grid
        int col = i % 2, row = i / 2;
        g_cbtn_arg[i] = args[i];
        g_cbtn_label[i] = NULL;
        g_cbtn_proc[i] = NULL;
        g_cbtn_result[i] = NULL;
        g_cbtn_avg[i] = NULL;
        g_cbtn[i] = CreateWindowA("BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  cr.left + col * (colw + 12), y + row * rowh, colw, 30, frame,
                                  (HMENU)(INT_PTR)(ID_CB_FIRST + i), g_hinst, NULL);
        if (g_ui_font) SendMessage(g_cbtn[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    }
    y += ((n_full + 1) / 2) * rowh + 10;
    // Draw stress: one "Draw stress:" label + the five draw counts on a single row.
    g_draw_label = CreateWindowA("STATIC", "Draw stress:", WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left,
                                 y + 8, 96, 22, frame, NULL, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_draw_label, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    int dx = cr.left + 100;
    for (; i < g_cbtn_n; i++) {
        g_cbtn_arg[i] = args[i];
        g_cbtn_label[i] = NULL;
        g_cbtn_proc[i] = NULL;
        g_cbtn_result[i] = NULL;
        g_cbtn_avg[i] = NULL;
        g_cbtn[i] = CreateWindowA("BUTTON", counts[i - n_full], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  dx, y, 62, 30, frame, (HMENU)(INT_PTR)(ID_CB_FIRST + i), g_hinst,
                                  NULL);
        if (g_ui_font) SendMessage(g_cbtn[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        dx += 68;
    }
    // Link to the procedural demo-scene gallery.
    g_run_all = CreateWindowA("BUTTON", "Demo Scenes  >", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              cr.left, y + 44, 220, 32, frame, (HMENU)(INT_PTR)ID_DX11_DEMOS, g_hinst,
                              NULL);
    if (g_ui_font) SendMessage(g_run_all, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
}

// Direct3D 11 demo-scene gallery: the procedural showpieces (raymarched), each
// launch-only. Reached from the DX11 feature picker's "Demo Scenes" button.
static void show_dx11_demos(HWND frame) {
    destroy_content();
    g_cb_bench = 0;
    SetWindowTextA(g_header, "Direct3D 11 - Demo Scenes");
    RECT cr;
    get_content_rect(frame, &cr);

    g_placeholder = CreateWindowA(
        "STATIC",
        "Procedural showcase scenes (ray-marched: reflections, shadows, lighting, moving objects).\n"
        "Each opens in a new window; press Esc to close it.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, cr.top, cr.right - cr.left, 56, frame, NULL, g_hinst,
        NULL);
    if (g_ui_font) SendMessage(g_placeholder, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    static const char *labels[] = {"Raymarch SDF",            "Ocean (raymarched)",
                                   "Ocean v2",                "Mandelbulb fractal",
                                   "Volumetric nebula",       "Detailed Nebula",
                                   "Showcase (reflect+shadow)", "Space (planet+asteroids)",
                                   "Desert (dunes)",          "Cityscape (neon city)",
                                   "Cel shading (torus)",     "Matcap (chrome ball)"};
    static const char *args[] = {"dx11 --scene raymarch",   "dx11 --scene ocean",
                                 "dx11 --scene ocean2",      "dx11 --scene mandelbulb",
                                 "dx11 --scene nebula",      "dx11 --scene nebula2",
                                 "dx11 --scene showcase",    "dx11 --scene space",
                                 "dx11 --scene desert",      "dx11 --scene city",
                                 "dx11 --scene cel",         "dx11 --scene matcap"};
    g_cbtn_n = (int)(sizeof(args) / sizeof(args[0]));
    int y = cr.top + 70;
    const int colw = 220, rowh = 38;
    for (int i = 0; i < g_cbtn_n; i++) {
        int col = i % 2, row = i / 2;
        g_cbtn_arg[i] = args[i];
        g_cbtn_label[i] = NULL;
        g_cbtn_proc[i] = NULL;
        g_cbtn_result[i] = NULL;
        g_cbtn_avg[i] = NULL;
        g_cbtn[i] = CreateWindowA("BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  cr.left + col * (colw + 12), y + row * rowh, colw, 30, frame,
                                  (HMENU)(INT_PTR)(ID_CB_FIRST + i), g_hinst, NULL);
        if (g_ui_font) SendMessage(g_cbtn[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    }
    y += ((g_cbtn_n + 1) / 2) * rowh + 12;
    g_run_all = CreateWindowA("BUTTON", "<  Back to D3D11 tests", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              cr.left, y, 220, 32, frame, (HMENU)(INT_PTR)ID_DX11_BACK, g_hinst, NULL);
    if (g_ui_font) SendMessage(g_run_all, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
}

// Vulkan scene picker: native-Vulkan tests (no DXVK). Launch-only buttons.
static void show_vk_scenes(HWND frame) {
    destroy_content();
    g_cb_bench = 0;
    SetWindowTextA(g_header, "Cube - Vulkan (native)");
    RECT cr;
    get_content_rect(frame, &cr);

    g_placeholder = CreateWindowA(
        "STATIC",
        "Native Vulkan tests (no DXVK). Each opens in a new window; the menu stays\n"
        "here. Press Esc in a test window to close it.",
        WS_CHILD | WS_VISIBLE | SS_LEFT, cr.left, cr.top, cr.right - cr.left, 56, frame, NULL, g_hinst,
        NULL);
    if (g_ui_font) SendMessage(g_placeholder, WM_SETFONT, (WPARAM)g_ui_font, TRUE);

    static const char *labels[] = {"Spinning cube (textured)", "Phong-lit cube",
                                   "Mesh shader probe"};
    static const char *args[] = {"vk", "vk --scene phong", "vk --scene meshshader"};
    g_cbtn_n = (int)(sizeof(args) / sizeof(args[0]));
    int y = cr.top + 70;
    for (int i = 0; i < g_cbtn_n; i++) {
        g_cbtn_arg[i] = args[i];
        g_cbtn_label[i] = NULL;
        g_cbtn_proc[i] = NULL;
        g_cbtn_result[i] = NULL;
        g_cbtn_avg[i] = NULL;
        g_cbtn[i] = CreateWindowA("BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, cr.left,
                                  y, 240, 34, frame, (HMENU)(INT_PTR)(ID_CB_FIRST + i), g_hinst, NULL);
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
    if (g_edit_vk) layout_gpuinfo(&cr);
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

// Rebuild a content view. The window is WS_EX_COMPOSITED, so the destroy-old +
// create-many-children churn is double-buffered and presented atomically (no
// top-to-bottom paint scan, no black unpainted boxes). Used for in-view rebuilds.
// Dark-theme button painting. We SUBCLASS (not owner-draw) so the native BUTTON
// keeps all its behaviour — auto-toggle, check state, focus, click — and we only
// override WM_PAINT to draw a dark face / dark checkbox. Push buttons and the
// stateful benchmark checkboxes both keep working; only the look changes.
static LRESULT CALLBACK btn_subproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) return 1;  // fully painted in WM_PAINT
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        LONG_PTR style = GetWindowLongPtrA(h, GWL_STYLE);
        UINT bt = (UINT)(style & 0x0F);
        int isCheck = (bt == BS_CHECKBOX || bt == BS_AUTOCHECKBOX || bt == BS_3STATE ||
                       bt == BS_AUTO3STATE || bt == BS_RADIOBUTTON || bt == BS_AUTORADIOBUTTON);
        LRESULT bst = SendMessageA(h, BM_GETSTATE, 0, 0);
        int pushed = (bst & BST_PUSHED) != 0;
        int checked = (SendMessageA(h, BM_GETCHECK, 0, 0) == BST_CHECKED);
        int enabled = IsWindowEnabled(h);
        char txt[256];
        GetWindowTextA(h, txt, (int)sizeof(txt));
        HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
        HFONT of = f ? (HFONT)SelectObject(dc, f) : NULL;
        SetBkMode(dc, TRANSPARENT);
        COLORREF txtc = enabled ? DARK_TEXT : DARK_DIM;
        if (isCheck) {
            FillRect(dc, &rc, g_br_bg);
            int s = 16, by = (rc.bottom - s) / 2, bx = 2;
            RECT box = {bx, by, bx + s, by + s};
            HBRUSH boxbr = CreateSolidBrush(checked ? RGB(70, 130, 200) : DARK_CTL);
            FillRect(dc, &box, boxbr);
            DeleteObject(boxbr);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 126, 136));
            HPEN op = (HPEN)SelectObject(dc, pen);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, box.left, box.top, box.right, box.bottom);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
            if (checked) {
                HPEN cp = CreatePen(PS_SOLID, 2, RGB(240, 244, 250));
                HPEN ocp = (HPEN)SelectObject(dc, cp);
                MoveToEx(dc, box.left + 3, by + s / 2, NULL);
                LineTo(dc, box.left + s / 2 - 1, box.bottom - 4);
                LineTo(dc, box.right - 3, box.top + 3);
                SelectObject(dc, ocp);
                DeleteObject(cp);
            }
            SetTextColor(dc, txtc);
            RECT tr = rc;
            tr.left = bx + s + 6;
            DrawTextA(dc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            COLORREF face = pushed ? RGB(34, 37, 44) : RGB(48, 52, 60);
            HBRUSH fb = CreateSolidBrush(face);
            FillRect(dc, &rc, fb);
            DeleteObject(fb);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(82, 88, 98));
            HPEN op = (HPEN)SelectObject(dc, pen);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
            SetTextColor(dc, txtc);
            RECT tr = rc;
            if (pushed) {
                tr.left += 1;
                tr.top += 1;
            }
            DrawTextA(dc, txt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (of) SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    return CallWindowProcA(g_btn_oldproc, h, msg, wp, lp);
}

static BOOL CALLBACK theme_btn_cb(HWND child, LPARAM lp) {
    (void)lp;
    char cls[16];
    GetClassNameA(child, cls, (int)sizeof(cls));
    if (lstrcmpiA(cls, "Button") == 0 &&
        (WNDPROC)GetWindowLongPtrA(child, GWLP_WNDPROC) != btn_subproc) {
        WNDPROC old = (WNDPROC)SetWindowLongPtrA(child, GWLP_WNDPROC, (LONG_PTR)btn_subproc);
        if (!g_btn_oldproc) g_btn_oldproc = old;
    }
    return TRUE;
}

// Subclass every BUTTON under the frame for dark paint (idempotent: re-themes the
// persistent sidebar + any freshly built content buttons each call).
static void theme_buttons(HWND frame) { EnumChildWindows(frame, theme_btn_cb, 0); }

static void rebuild_view(HWND frame, void (*build)(HWND)) {
    build(frame);
    theme_buttons(frame);
    InvalidateRect(frame, NULL, TRUE);
}

static void on_select(HWND frame, int action) {
    if (action == AIO_MODE_EXIT) {
        DestroyWindow(frame);
        return;
    }
    switch (action) {
        case AIO_MODE_GPUINFO:
            show_gpuinfo(frame);
            break;
        case AIO_MODE_CUBE_VK:
            show_vk_scenes(frame);  // pick the textured cube or the Phong-lit cube
            break;
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
    theme_buttons(frame);  // dark-theme any buttons the selected view just created
    InvalidateRect(frame, NULL, TRUE);  // WS_EX_COMPOSITED presents the new view atomically
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
            theme_buttons(hwnd);  // dark-theme the sidebar + initial view
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (HIWORD(wParam) == CBN_SELCHANGE && id == ID_RESULTS_COMBO) {  // run picker changed
                g_results_sel = (int)SendMessage(g_results_combo, CB_GETCURSEL, 0, 0);
                rebuild_view(hwnd, show_bench_results);
                return 0;
            }
            if (id == ID_RUN_ALL && g_cb_bench && g_cbtn_n > 0) {  // run selected rows in sequence
                g_run_n = 0;
                for (int k = 0; k < g_cbtn_n; k++)
                    if (g_cbtn_sel[k]) g_run_list[g_run_n++] = k;
                if (g_run_n > 0) {
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    snprintf(g_run_ts, sizeof(g_run_ts), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear,
                             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                    hist_append_run_header(g_run_ts, g_bench_secs);  // new timestamped run record
                    g_sweep_active = 1;
                    g_run_pos = 0;
                    launch_bench_row(hwnd, g_run_list[0]);
                }
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
            {
                int chk = id - ID_CHK_FIRST;  // a per-row selection checkbox toggled
                if (chk >= 0 && chk < g_cbtn_n) {
                    g_cbtn_sel[chk] =
                        (g_cbtn[chk] && SendMessage(g_cbtn[chk], BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1
                                                                                                    : 0;
                    if (g_cbtn_d3d11[chk] && g_d3d11_chk) {  // keep the group header in sync
                        int all = 1;
                        for (int k = 0; k < g_cbtn_n; k++)
                            if (g_cbtn_d3d11[k] && !g_cbtn_sel[k]) all = 0;
                        SendMessage(g_d3d11_chk, BM_SETCHECK, all ? BST_CHECKED : BST_UNCHECKED, 0);
                    }
                    return 0;
                }
            }
            if (id == ID_D3D11_HDR) {  // select/deselect all D3D11 scenes
                int on = (SendMessage(g_d3d11_chk, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                for (int k = 0; k < g_cbtn_n; k++)
                    if (g_cbtn_d3d11[k]) {
                        g_cbtn_sel[k] = on;
                        if (g_cbtn[k])
                            SendMessage(g_cbtn[k], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
                    }
                return 0;
            }
            if (id == ID_SELECT_ALL) {  // toggle: all ticked -> clear all, else select all
                int all = 1;
                for (int k = 0; k < g_cbtn_n; k++)
                    if (!g_cbtn_sel[k]) { all = 0; break; }
                int on = all ? 0 : 1;
                for (int k = 0; k < g_cbtn_n; k++) g_cbtn_sel[k] = on;
                rebuild_view(hwnd, show_benchmark);  // refresh every checkbox + the label
                return 0;
            }
            if (id == ID_D3D11_EXPAND) {  // show/hide the D3D11 scene grid
                g_d3d11_expanded = !g_d3d11_expanded;
                rebuild_view(hwnd, show_benchmark);
                return 0;
            }
            if (id == ID_SHOW_RESULTS) {
                g_results_sel = 0;  // default to the current-session view
                rebuild_view(hwnd, show_bench_results);
                return 0;
            }
            if (id == ID_RESULTS_BACK) {
                rebuild_view(hwnd, show_benchmark);
                return 0;
            }
            if (id == ID_DX11_DEMOS) {  // open the demo-scene gallery
                rebuild_view(hwnd, show_dx11_demos);
                return 0;
            }
            if (id == ID_DX11_BACK) {  // back to the DX11 feature picker
                rebuild_view(hwnd, show_dx11_scenes);
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
                        // During a Run-Selected sweep, log to the persisted history record.
                        if (g_sweep_active && a > 0.0f) hist_append_row(g_cbtn_label[i], a, mn, mx);
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
                    // Run-Selected sweep: when the current row finishes, start the next.
                    if (g_sweep_active && g_run_pos < g_run_n && i == g_run_list[g_run_pos]) {
                        g_run_pos++;
                        if (g_run_pos < g_run_n)
                            launch_bench_row(hwnd, g_run_list[g_run_pos]);
                        else
                            g_sweep_active = 0;  // sweep complete
                    }
                }
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HWND ctl = (HWND)lParam;
            HDC hdc = (HDC)wParam;
            // Read-only multiline EDITs report via WM_CTLCOLORSTATIC; they need an
            // OPAQUE bg or the (unselected) text renders invisibly. Detect them by
            // CLASS NAME, not pointer: the edit paints once at creation before
            // g_edit_vk/g_edit_gl are assigned, and would otherwise cache the wrong
            // (transparent label) colors until the user clicks it.
            if (msg == WM_CTLCOLORSTATIC) {
                char ccls[8];
                GetClassNameA(ctl, ccls, (int)sizeof(ccls));
                if (lstrcmpiA(ccls, "Edit") == 0) {
                    SetTextColor(hdc, DARK_TEXT);
                    SetBkColor(hdc, DARK_CTL);
                    SetBkMode(hdc, OPAQUE);
                    return (LRESULT)g_br_ctl;
                }
            }
            COLORREF txt = DARK_TEXT;
            if (ctl == g_foot_heart) txt = RGB(214, 69, 79);
            else if (ctl == g_foot_a || ctl == g_foot_b || ctl == g_version) txt = DARK_DIM;
            SetTextColor(hdc, txt);
            SetBkColor(hdc, DARK_BG);
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)g_br_bg;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, DARK_TEXT);
            SetBkColor(hdc, DARK_CTL);
            return (LRESULT)g_br_ctl;
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

    g_br_bg = CreateSolidBrush(DARK_BG);     // dark theme brushes (window/panel + controls)
    g_br_ctl = CreateSolidBrush(DARK_CTL);

    const char *cls = "AIOGraphicsTestShell";
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = shell_wndproc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconA(hInstance, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_br_bg;  // dark theme
    wc.lpszClassName = cls;
    RegisterClassA(&wc);

    int w = 840, h = 800;  // tall enough for the full benchmark list (17 rows)
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;

    // WS_EX_COMPOSITED double-buffers child painting: on a view rebuild the new
    // controls are composited off-screen and presented at once, so the brief
    // unpainted state (which Wine draws as black boxes) never reaches the screen.
    HWND hwnd = CreateWindowExA(WS_EX_COMPOSITED, cls, "AIO Graphics Test",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, sx, sy, w, h, NULL, NULL,
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
    if (g_br_bg) DeleteObject(g_br_bg);
    if (g_br_ctl) DeleteObject(g_br_ctl);
    UnregisterClassA(cls, hInstance);
    return 0;
}
