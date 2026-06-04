// AIO Graphics Test - disk (drive) read/write speed test.
//
// Writes a temp file sequentially (write-through, then FlushFileBuffers so the
// data really lands on the device), reads it back (sequential + random 4 KB),
// then does a burst of random 4 KB writes. Reports MB/s (decimal, 1e6 bytes) and
// IOPS, the units drive benchmarks conventionally use.
//
// The catch on Wine/Winlator: FILE_FLAG_NO_BUFFERING is usually ignored, so a
// read right after the write is served from the Linux page cache (RAM) and looks
// absurdly fast. In real-flash mode we defeat that by writing a second file the
// size of the device's RAM after the write - that evicts the test file's pages
// from the cache (LRU), so the read passes hit the storage cold.
//
// Real-flash mode busts the cache TWICE - once before each read - because both
// reads warm the file and would poison the other:
//   write -> buster #1 -> sequential read (cold) -> buster #2 -> random read
//   (cold) -> random write
// The sequential read pulls the whole file into cache, so without buster #2 the
// random read just hits RAM (~500k IOPS / 2 GB/s - the original bug). Note the
// random read alone can't precede the sequential one either: even with a RANDOM
// hint, the kernel's async readahead loads neighbour pages the random reads never
// reuse (so the random average stays honestly cold) yet leaves the file fully
// resident for a following sequential pass. Two flushes is the only reliable fix
// on Wine, where FILE_FLAG_NO_BUFFERING is ignored. The random WRITE runs last,
// flushed per-op (committed-write speed) so it's "real" in both modes and doesn't
// dirty the file before the reads.
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
#define DISK_RAND_SZ  4096u                  // random read/write size: 4 KiB
#define DISK_RAND_CNT 4096                    // number of random reads (~16 MiB)
#define DISK_RANDW_CNT 1024                   // random 4 KiB writes (per-op flushed, so fewer)
#define DISK_REPORT_CAP 2560                  // report buffer (room for a long path + notes)

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

// Write `bytes` of `buf` (DISK_BLOCK each) to `path`, write-through + flushed so
// it really hits the device. Used for both the (timed) test file and the
// (untimed) cache-buster. Returns 1 on success, 0 on failure.
static int write_file_blocks(const char *path, uint64_t bytes, const void *buf) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 1;
    for (uint64_t off = 0; off < bytes; off += DISK_BLOCK) {
        DWORD wrote = 0;
        if (!WriteFile(h, buf, DISK_BLOCK, &wrote, NULL) || wrote != DISK_BLOCK) { ok = 0; break; }
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

// Map a sequential MB/s figure to a consumer storage class. Thresholds are set a
// little below the raw-spec headline numbers because we measure in-container (Wine
// + the container FS add overhead), so this is a floor - the real chip is often a
// tier higher, hence the "or better" hedge in the report.
static const char *disk_class_for(double seq_mbps) {
    if (seq_mbps < 120) return "SD card / slow eMMC";
    if (seq_mbps < 280) return "eMMC 5.x";
    if (seq_mbps < 600) return "UFS 2.0-2.1";
    if (seq_mbps < 1300) return "UFS 2.2 / UFS 3.0";
    if (seq_mbps < 2600) return "UFS 3.1";
    if (seq_mbps < 4200) return "UFS 4.0";
    return "UFS 4.0+ / NVMe SSD";
}

char *aio_disk_run(int size_mb, int defeat_cache, aio_disk_progress_fn progress, void *user) {
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

    // Temp file paths: %TEMP%\AIO-Graphics-Test_disk{,_buster}.tmp.
    char dir[MAX_PATH], path[MAX_PATH], buster[MAX_PATH];
    DWORD dn = GetTempPathA(sizeof(dir), dir);
    if (dn == 0 || dn >= sizeof(dir)) strcpy(dir, ".\\");
    snprintf(path, sizeof(path), "%sAIO-Graphics-Test_disk.tmp", dir);
    snprintf(buster, sizeof(buster), "%sAIO-Graphics-Test_buster.tmp", dir);

    // Physical RAM + free space - used to size the cache-buster in real-flash mode.
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    uint64_t ram = GlobalMemoryStatusEx(&ms) ? (uint64_t)ms.ullTotalPhys : 0;
    ULARGE_INTEGER freeb;
    freeb.QuadPart = 0;
    GetDiskFreeSpaceExA(dir, &freeb, NULL, NULL);

    // Cache-buster size: ~1.1x RAM so the test file is fully evicted, rounded to a
    // block. Capped so test + buster + a 512 MiB margin still fit in free space.
    uint64_t buster_bytes = 0;
    int buster_short = 0;  // had to shrink the buster (read may be partly cached)
    if (defeat_cache && ram > 0) {
        buster_bytes = ram + ram / 10;
        buster_bytes -= buster_bytes % DISK_BLOCK;
        uint64_t margin = 512ull * 1024 * 1024;
        if (freeb.QuadPart > 0 && total + buster_bytes + margin > freeb.QuadPart) {
            uint64_t avail = (freeb.QuadPart > total + margin) ? freeb.QuadPart - total - margin : 0;
            avail -= avail % DISK_BLOCK;
            if (avail < buster_bytes) { buster_bytes = avail; buster_short = 1; }
        }
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    void *buf = aligned_alloc_pages(DISK_BLOCK);
    if (!buf) {
        snprintf(report, DISK_REPORT_CAP,
                 "Disk Read / Write Speed\r\n\r\nCould not allocate the I/O buffer.");
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
    double randw_mbps = 0.0, randw_iops = 0.0;
    double t_write = 0.0, t_read = 0.0, t_rand = 0.0, t_randw = 0.0;
    const char *err = NULL;

    // ---- Sequential write (write-through, timed) ---------------------------
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
                 "Sequential read  : %s\r\n",
                 path, file_mib, write_mbps, t_write,
                 (defeat_cache && buster_bytes > 0) ? "flushing cache, then cold reads..."
                                                    : "reading...");
        progress(user, report);
    }

    // ---- Cache-buster: evict the test file from the page cache --------------
    // Real-flash mode only. Writing a file ~= RAM pushes the just-written test
    // file out of the cache (LRU on clean pages), so the read below hits flash.
    if (!err && defeat_cache && buster_bytes > 0) {
        if (!write_file_blocks(buster, buster_bytes, buf)) {
            // Non-fatal: if the buster can't be written, fall back to a (cached)
            // read rather than failing the whole run.
            buster_short = 1;
            buster_bytes = 0;
        }
    }

    // ---- Sequential read (cold off buster #1 in real-flash mode) -----------
    if (!err) {
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (h == INVALID_HANDLE_VALUE)  // Wine/Winlator may reject NO_BUFFERING
            h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, NULL);
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
        }
    }

    // ---- Cache-buster #2: re-evict before the random read ------------------
    // The sequential read above re-warmed the whole file (its readahead leaves
    // neighbour pages resident even past what it returned), so without flushing
    // again the random read would be served from RAM. Rewrite the buster file -
    // RAM-worth of fresh write-through pushes the test file back out via LRU.
    if (!err && defeat_cache && buster_bytes > 0) {
        if (!write_file_blocks(buster, buster_bytes, buf))
            buster_short = 1;  // second flush failed: random read may read cached
    }

    // ---- Random 4 KB read (cold off buster #2 in real-flash mode) ----------
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

    // ---- Random 4 KB write (LAST; per-op flush = committed-write speed) -----
    // Each scattered 4 KiB write is committed to the device before the next
    // (write-through + a FlushFileBuffers per op). That makes this queue-depth-1
    // *durable* random write - the worst case that matters for save-game and
    // shader-cache commits, and the figure external benchmarks report. A single
    // trailing flush (bulk) over-reports ~10x on Wine: the scattered writes stay
    // in RAM and coalesce into one sequential flush. Runs after the reads so it
    // doesn't dirty the file beforehand.
    if (!err) {
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            uint64_t slots = total / DISK_RAND_SZ;
            uint32_t rng = 0x85ebca6bu;
            double t0 = now_sec(freq);
            for (int i = 0; i < DISK_RANDW_CNT && !err; i++) {
                rng = rng * 1664525u + 1013904223u;
                uint64_t off = (uint64_t)(rng % (uint32_t)slots) * DISK_RAND_SZ;
                LARGE_INTEGER li;
                li.QuadPart = (LONGLONG)off;
                if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) { err = "Seek failed."; break; }
                DWORD wrote = 0;
                if (!WriteFile(h, buf, DISK_RAND_SZ, &wrote, NULL) || wrote != DISK_RAND_SZ)
                    err = "Random write failed.";
                FlushFileBuffers(h);  // commit THIS write before timing the next
            }
            double t1 = now_sec(freq);
            CloseHandle(h);
            if (!err) {
                t_randw = t1 - t0;
                if (t_randw > 0.0) {
                    randw_iops = (double)DISK_RANDW_CNT / t_randw;
                    randw_mbps = (double)DISK_RANDW_CNT * DISK_RAND_SZ / 1e6 / t_randw;
                }
            }
        }
    }

    DeleteFileA(path);    // clean up the temp files
    DeleteFileA(buster);
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

    // Build the per-mode footnote about read accuracy.
    char note[768];
    double ram_gb = (double)ram / (1024.0 * 1024.0 * 1024.0);
    if (defeat_cache) {
        if (buster_bytes > 0 && !buster_short)
            snprintf(note, sizeof(note),
                     "Mode: real flash. Both reads ran cold - a %llu MiB cache-buster file\r\n"
                     "(device RAM ~= %.1f GB) is written before EACH read to evict the test file\r\n"
                     "from the OS page cache, so seq + random read reflect storage, not RAM.\r\n"
                     "Random write is per-op flushed (committed-write speed).",
                     (unsigned long long)(buster_bytes / (1024 * 1024)), ram_gb);
        else
            snprintf(note, sizeof(note),
                     "Mode: real flash (PARTIAL). Not enough free space to write a full\r\n"
                     "RAM-sized cache-buster (device RAM ~= %.1f GB), so the read may still be\r\n"
                     "partly cached and reads higher than real flash. Free up space to fix this.",
                     ram_gb);
    } else {
        snprintf(note, sizeof(note),
                 "Mode: quick. The read passes are served from the OS page cache (the file\r\n"
                 "was just written), so seq + random READ are RAM-fast, not real flash. Use\r\n"
                 "the \"Real-Flash Read\" button for true read figures. (Both WRITES use\r\n"
                 "write-through + flush, so they are always real.)");
    }

    // Storage-class estimate: from sequential write (always real) and, in
    // real-flash mode, the cold read (use whichever is higher - tiers are usually
    // quoted by peak sequential). The cached quick-mode read and random are NOT
    // used (they'd mis-rate the class).
    double seq_signal = write_mbps;
    if (defeat_cache && read_mbps > seq_signal) seq_signal = read_mbps;
    const char *cls = disk_class_for(seq_signal);

    snprintf(report, DISK_REPORT_CAP,
             "Disk Read / Write Speed\r\n"
             "=======================\r\n\r\n"
             "Test file : %s\r\n"
             "File size : %u MiB   (block 4 MiB)\r\n\r\n"
             "Sequential write : %7.1f MB/s   (%.2f s)\r\n"
             "Sequential read  : %7.1f MB/s   (%.2f s)\r\n"
             "Random 4K read   : %7.1f MB/s   (%.0f IOPS, %d reads)\r\n"
             "Random 4K write  : %7.1f MB/s   (%.0f IOPS, %d writes)\r\n\r\n"
             "Storage class    : ~ %s\r\n"
             "                   (or better - in-container estimate%s)\r\n\r\n"
             "Throughput is decimal MB/s (1,000,000 bytes).\r\n%s",
             path, file_mib, write_mbps, t_write, read_mbps, t_read, rand_mbps, rand_iops,
             DISK_RAND_CNT, randw_mbps, randw_iops, DISK_RANDW_CNT,
             cls, defeat_cache ? "" : "; run Real-Flash Read for a read-based class",
             note);
    return report;
}
