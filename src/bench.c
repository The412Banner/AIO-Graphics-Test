// AIO Graphics Test - benchmark instrumentation.
//
// Stores per-frame times for a timed run, then computes avg / min / max / 1%-low
// FPS and writes a CSV. "1% low" = the average FPS over the slowest 1% of frames
// (the standard frametime-percentile definition).
//
// Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

#define AIO_BENCH_CSV "AIO-Graphics-Test_bench.csv"
#define AIO_BENCH_TITLE "AIO Graphics Test - Benchmark"

int aio_autoclose_sec = 0;

// Dismisses the result popup after aio_autoclose_sec seconds (finds it by title
// and sends the OK command). No-op if the user already closed it.
static DWORD WINAPI aio_box_closer(LPVOID p) {
    Sleep((DWORD)(int)(INT_PTR)p * 1000u);
    HWND h = FindWindowA(NULL, AIO_BENCH_TITLE);
    if (h) PostMessageA(h, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
    return 0;
}

void aio_bench_show_result(const char *text) {
    if (!text) return;
    HANDLE th = NULL;
    if (aio_autoclose_sec > 0)
        th = CreateThread(NULL, 0, aio_box_closer, (LPVOID)(INT_PTR)aio_autoclose_sec, 0, NULL);
    MessageBoxA(NULL, text, AIO_BENCH_TITLE, MB_OK | MB_ICONINFORMATION);
    if (th) CloseHandle(th);
}

static double *g_ft;   // per-frame times (ms)
static size_t g_n;     // sample count
static size_t g_cap;   // capacity
static int g_seconds;  // requested duration
static int g_active;
static double g_warmup_ms;   // discard frames until this much run time has elapsed
static double g_elapsed_ms;  // wall time of all frames so far (incl. discarded warm-up)
static char g_label_override[64];  // if set, overrides the result label (probe modes)
int aio_vsync = 0;                 // shared vsync flag (set from --vsync)

void aio_bench_set_label(const char *label) {
    if (label && label[0])
        snprintf(g_label_override, sizeof(g_label_override), "%s", label);
    else
        g_label_override[0] = '\0';
}

// Frames faster than this (ms) are measurement artifacts, not real rendered
// frames (a windowed Present alone costs more than this), so they're dropped to
// keep Max FPS meaningful. 0.02 ms == 50000 FPS, well above any real result.
#define AIO_BENCH_MIN_FT_MS 0.02
// Warm-up window (ms): frames in the first slice of the run are excluded from
// the stats. Time-based, not a frame count, because driver-side pipeline/shader
// compilation (notably VKD3D on first D3D12 draw) can stall a frame several
// frames in - a fixed count let that ~1 s spike pollute Min FPS.
#define AIO_BENCH_WARMUP_MS 500.0

void aio_bench_begin(int seconds) {
    g_seconds = seconds;
    g_active = 1;
    g_n = 0;
    g_cap = 4096;
    g_warmup_ms = AIO_BENCH_WARMUP_MS;
    g_elapsed_ms = 0.0;
    g_ft = (double *)calloc(g_cap, sizeof(double));
}

int aio_bench_active(void) { return g_active; }
int aio_bench_seconds(void) { return g_seconds; }

void aio_bench_add(double frame_ms) {
    if (!g_active || !g_ft || frame_ms <= 0.0) return;
    // Drop every frame that starts inside the warm-up window. Testing elapsed
    // time *before* adding this frame means a single huge stall frame within the
    // window (e.g. a first-use shader compile) is itself excluded.
    if (g_elapsed_ms < g_warmup_ms) {
        g_elapsed_ms += frame_ms;
        return;
    }
    g_elapsed_ms += frame_ms;
    if (frame_ms < AIO_BENCH_MIN_FT_MS) return;  // drop impossible sub-frame outliers
    if (g_n >= g_cap) {
        size_t nc = g_cap * 2;
        if (nc < g_cap || nc > SIZE_MAX / sizeof(double)) return;
        double *p = (double *)realloc(g_ft, nc * sizeof(double));
        if (!p) return;
        g_ft = p;
        g_cap = nc;
    }
    g_ft[g_n++] = frame_ms;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

char *aio_bench_finish(const char *api_label, double total_seconds) {
    if (!api_label) api_label = "";
    if (g_label_override[0]) api_label = g_label_override;  // probe modes override the label

    if (!g_ft || g_n == 0) {
        g_active = 0;
        if (g_ft) { free(g_ft); g_ft = NULL; }
        char *s = (char *)malloc(64);
        if (s) snprintf(s, 64, "No frames were captured.");
        return s;
    }

    double minft = g_ft[0], maxft = g_ft[0], sum = 0.0;
    for (size_t i = 0; i < g_n; i++) {
        double v = g_ft[i];
        sum += v;
        if (v < minft) minft = v;
        if (v > maxft) maxft = v;
    }

    // Average over the measured (post-warm-up) frames: total kept frames / total
    // kept frame time. Using the kept-frame sum rather than the full wall-clock
    // duration keeps the average unbiased when warm-up frames are trimmed.
    double avg_fps = (sum > 0.0) ? (1000.0 * (double)g_n / sum) : 0.0;
    double max_fps = (minft > 0.0) ? 1000.0 / minft : 0.0;  // fallback: single fastest frame
    double min_fps = (maxft > 0.0) ? 1000.0 / maxft : 0.0;  // slowest frame = real worst case

    // 1% low: average FPS over the slowest 1% of frames. Also use the sorted
    // frametimes to make Max robust.
    double *sorted = (double *)calloc(g_n, sizeof(double));
    double low1_fps = 0.0;
    if (sorted) {
        memcpy(sorted, g_ft, g_n * sizeof(double));
        qsort(sorted, g_n, sizeof(double), cmp_double);  // ascending (fastest first)
        size_t k = g_n / 100;
        if (k < 1) k = 1;
        double s2 = 0.0;
        for (size_t i = 0; i < k; i++) s2 += sorted[g_n - 1 - i];  // slowest k frametimes
        low1_fps = (s2 > 0.0) ? 1000.0 * (double)k / s2 : 0.0;
        // Robust Max FPS: report the 1%-fastest frametime, not the single fastest -
        // a stray sub-frame timing glitch otherwise yields an absurd "Max" (tens of
        // thousands of FPS). Mirrors the 1%-low on the slow end.
        if (k < g_n && sorted[k] > 0.0) max_fps = 1000.0 / sorted[k];
        free(sorted);
    }

    FILE *f = fopen(AIO_BENCH_CSV, "w");
    if (f) {
        fprintf(f, "# AIO Graphics Test benchmark - %s\n", api_label);
        fprintf(f, "# frames,%lu,seconds,%.3f\n", (unsigned long)g_n, total_seconds);
        fprintf(f, "# avg_fps,%.2f,min_fps,%.2f,max_fps,%.2f,low1pct_fps,%.2f\n", avg_fps, min_fps,
                max_fps, low1_fps);
        fprintf(f, "frame,frame_ms,fps\n");
        for (size_t i = 0; i < g_n; i++) {
            double v = g_ft[i];
            fprintf(f, "%lu,%.4f,%.2f\n", (unsigned long)i, v, (v > 0.0) ? 1000.0 / v : 0.0);
        }
        fclose(f);
    }

    // Compact result file (read by the shell's Benchmark view): avg|min|max FPS.
    // The view shows only these three; full stats stay in the CSV + popup.
    {
        char rfn[160];
        snprintf(rfn, sizeof(rfn), "AIO-Graphics-Test_bench_%s.txt", api_label);
        FILE *rf = fopen(rfn, "w");
        if (rf) {
            fprintf(rf, "%.0f|%.0f|%.0f", avg_fps, min_fps, max_fps);
            fclose(rf);
        }
    }

    char *summary = (char *)malloc(512);
    if (summary) {
        snprintf(summary, 512,
                 "%s benchmark\n\n"
                 "Duration : %.1f s\n"
                 "Frames   : %lu\n\n"
                 "Avg FPS  : %.1f\n"
                 "Min FPS  : %.1f\n"
                 "Max FPS  : %.1f\n"
                 "1%% low   : %.1f\n\n"
                 "Saved: " AIO_BENCH_CSV,
                 api_label, total_seconds, (unsigned long)g_n, avg_fps, min_fps, max_fps, low1_fps);
    }

    free(g_ft);
    g_ft = NULL;
    g_n = 0;
    g_active = 0;
    return summary;
}
