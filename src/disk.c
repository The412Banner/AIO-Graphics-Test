// AIO Graphics Test - disk (drive) read/write speed test.
//
// Writes a temp file sequentially (write-through, then FlushFileBuffers so the
// data really lands on the device), reads it back sequentially with unbuffered
// I/O to dodge the OS page cache where the platform allows it, then does a burst
// of random 4 KB reads for an IOPS figure. Reports MB/s (decimal, 1e6 bytes) and
// IOPS, the units drive benchmarks conventionally use.
//
// Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk.h"

#define DISK_BLOCK    (4u * 1024u * 1024u)  // sequential block: 4 MiB
#define DISK_RAND_SZ  4096u                  // random read size: 4 KiB
#define DISK_RAND_CNT 4096                    // number of random reads (~16 MiB)
#define DISK_REPORT_CAP 1536                  // report buffer (room for a long temp path)

static double now_sec(LARGE_INTEGER freq) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

// Page-aligned buffer (needed for FILE_FLAG_NO_BUFFERING). VirtualAlloc returns
// page-aligned memory, which satisfies any volume sector alignment in practice.
static void *aligned_alloc_pages(size_t n) {
    return VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static void aligned_free_pages(void *p) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

char *aio_disk_run(int size_mb, aio_disk_progress_fn progress, void *user) {
    char *report = (char *)malloc(DISK_REPORT_CAP);
    if (!report) return NULL;
    report[0] = '\0';

    if (size_mb < 4) size_mb = 4;
    // Round the file size up to a whole number of 4 MiB blocks so unbuffered
    // reads always land on full, sector-aligned blocks.
    uint64_t total = (uint64_t)size_mb * 1024u * 1024u;
    if (total % DISK_BLOCK) total += DISK_BLOCK - (total % DISK_BLOCK);
    uint64_t nblocks = total / DISK_BLOCK;
    unsigned file_mib = (unsigned)(total / (1024u * 1024u));

    // Temp file path: %TEMP%\AIO-Graphics-Test_disk.tmp, falling back to the
    // current directory if GetTempPath fails.
    char dir[MAX_PATH], path[MAX_PATH];
    DWORD dn = GetTempPathA(sizeof(dir), dir);
    if (dn == 0 || dn >= sizeof(dir)) strcpy(dir, ".\\");
    snprintf(path, sizeof(path), "%sAIO-Graphics-Test_disk.tmp", dir);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    void *buf = aligned_alloc_pages(DISK_BLOCK);
    if (!buf) {
        snprintf(report, DISK_REPORT_CAP, "Disk Read / Write Speed\r\n\r\nCould not allocate the I/O buffer.");
        return report;
    }
    // Fill with non-trivial data so compression / sparse-file optimisations can't
    // cheat the write figure (a simple LCG over the whole block).
    {
        uint32_t *w = (uint32_t *)buf;
        uint32_t s = 0x1234567u;
        for (size_t i = 0; i < DISK_BLOCK / sizeof(uint32_t); i++) {
            s = s * 1664525u + 1013904223u;
            w[i] = s;
        }
    }

    double write_mbps = 0.0, read_mbps = 0.0, rand_mbps = 0.0, rand_iops = 0.0;
    double t_write = 0.0, t_read = 0.0, t_rand = 0.0;
    const char *err = NULL;

    // ---- Sequential write (write-through) ----------------------------------
    {
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            err = "Could not create the temp file (storage full or read-only?).";
        } else {
            double t0 = now_sec(freq);
            for (uint64_t b = 0; b < nblocks && !err; b++) {
                DWORD wrote = 0;
                if (!WriteFile(h, buf, DISK_BLOCK, &wrote, NULL) || wrote != DISK_BLOCK)
                    err = "Write failed partway (storage full?).";
            }
            FlushFileBuffers(h);  // make sure it actually hit the device
            double t1 = now_sec(freq);
            CloseHandle(h);
            if (!err) {
                t_write = t1 - t0;
                if (t_write > 0.0) write_mbps = (double)total / 1e6 / t_write;
            }
        }
    }

    // Live update after the write phase.
    if (progress && !err) {
        snprintf(report, DISK_REPORT_CAP,
                 "Disk Read / Write Speed\r\n"
                 "=======================\r\n\r\n"
                 "Test file : %s\r\n"
                 "File size : %u MiB   (block 4 MiB)\r\n\r\n"
                 "Sequential write : %7.1f MB/s   (%.2f s)\r\n"
                 "Sequential read  : reading...\r\n",
                 path, file_mib, write_mbps, t_write);
        progress(user, report);
    }

    // ---- Sequential read (unbuffered if the platform allows it) -------------
    if (!err) {
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        int unbuffered = 1;
        if (h == INVALID_HANDLE_VALUE) {  // Wine/Winlator may reject NO_BUFFERING
            h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            unbuffered = 0;
        }
        if (h == INVALID_HANDLE_VALUE) {
            err = "Could not reopen the temp file for reading.";
        } else {
            double t0 = now_sec(freq);
            for (uint64_t b = 0; b < nblocks && !err; b++) {
                DWORD got = 0;
                if (!ReadFile(h, buf, DISK_BLOCK, &got, NULL) || got != DISK_BLOCK)
                    err = "Read failed partway.";
            }
            double t1 = now_sec(freq);
            CloseHandle(h);
            if (!err) {
                t_read = t1 - t0;
                if (t_read > 0.0) read_mbps = (double)total / 1e6 / t_read;
            }
            (void)unbuffered;
        }
    }

    // ---- Random 4 KB read ---------------------------------------------------
    if (!err) {
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, NULL);
        if (h == INVALID_HANDLE_VALUE)
            h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_FLAG_RANDOM_ACCESS, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            uint64_t slots = total / DISK_RAND_SZ;  // number of 4 KiB-aligned offsets
            uint32_t rng = 0x9e3779b9u;
            double t0 = now_sec(freq);
            for (int i = 0; i < DISK_RAND_CNT && !err; i++) {
                rng = rng * 1664525u + 1013904223u;
                uint64_t off = (uint64_t)(rng % (uint32_t)slots) * DISK_RAND_SZ;
                LARGE_INTEGER li;
                li.QuadPart = (LONGLONG)off;
                if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) { err = "Seek failed."; break; }
                DWORD got = 0;
                if (!ReadFile(h, buf, DISK_RAND_SZ, &got, NULL) || got != DISK_RAND_SZ)
                    err = "Random read failed.";
            }
            double t1 = now_sec(freq);
            CloseHandle(h);
            if (!err) {
                t_rand = t1 - t0;
                if (t_rand > 0.0) {
                    rand_iops = (double)DISK_RAND_CNT / t_rand;
                    rand_mbps = (double)DISK_RAND_CNT * DISK_RAND_SZ / 1e6 / t_rand;
                }
            }
        }
    }

    DeleteFileA(path);  // clean up the temp file
    aligned_free_pages(buf);

    if (err) {
        snprintf(report, DISK_REPORT_CAP,
                 "Disk Read / Write Speed\r\n"
                 "=======================\r\n\r\n"
                 "Test file : %s\r\n\r\n"
                 "ERROR: %s\r\n",
                 path, err);
        return report;
    }

    snprintf(report, DISK_REPORT_CAP,
             "Disk Read / Write Speed\r\n"
             "=======================\r\n\r\n"
             "Test file : %s\r\n"
             "File size : %u MiB   (block 4 MiB)\r\n\r\n"
             "Sequential write : %7.1f MB/s   (%.2f s)\r\n"
             "Sequential read  : %7.1f MB/s   (%.2f s)\r\n"
             "Random 4K read   : %7.1f MB/s   (%.0f IOPS, %d reads)\r\n\r\n"
             "Throughput is decimal MB/s (1,000,000 bytes). The read pass uses\r\n"
             "unbuffered I/O to bypass the OS cache where the platform allows it;\r\n"
             "on some Wine/Winlator setups the cache can still inflate it.",
             path, file_mib, write_mbps, t_write, read_mbps, t_read, rand_mbps, rand_iops,
             DISK_RAND_CNT);
    return report;
}
