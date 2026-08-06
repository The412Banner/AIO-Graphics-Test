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
// on Wine, where FILE_FLAG_NO_BUFFERING is ignored.
//
// Method matches CrossPlatformDiskTest (CPDT, com.Saplin.CPDT) so the numbers are
// comparable to that on-device benchmark:
//   - Random read/write visit a SHUFFLED set of UNIQUE 4 KiB block offsets (each
//     block touched at most once, so a repeat can't be served from RAM), and run
//     for a fixed 7-second window rather than a fixed op count.
//   - The random WRITE is flushed PER OP (FlushFileBuffers). On the Android device
//     CPDT runs with write-buffering off, which makes it fsync() every write; under
//     Wine FlushFileBuffers is the analog. Without it, Wine ignores WRITE_THROUGH,
//     lets the scattered writes coalesce in the page cache, and the figure balloons
//     to RAM speed (measured 1.7 GB/s vs CPDT's 40 MB/s). The per-op flush gives the
//     committed-write speed CPDT reports. It runs last so it doesn't dirty the file
//     before the reads.
//
// Copyright (c) 2026 The412Banner. Licensed under Apache-2.0 (see LICENSE).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk.h"

#define DISK_BLOCK    (4u * 1024u * 1024u)  // sequential block: 4 MiB (CPDT bigBlock)
#define DISK_RAND_SZ  4096u                  // random read/write size: 4 KiB (CPDT smallBlock)
#define DISK_RAND_SECS 7.0                    // CPDT runs each random test for a fixed 7 s window
#define DISK_RAND_CAP (1u << 20)              // cap on shuffled unique positions (CPDT maxBlocksInTest)
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

// Build a Fisher-Yates-shuffled array of unique 4 KiB block offsets covering the
// file (capped at DISK_RAND_CAP blocks, like CPDT). Visiting each block at most
// once is what keeps random reads honest - a repeated offset could be served from
// the page cache. Caller frees with free(). Sets *out_n; returns NULL on OOM.
static uint64_t *make_shuffled_offsets(uint64_t slots, uint64_t *out_n) {
    uint64_t n = slots < DISK_RAND_CAP ? slots : DISK_RAND_CAP;
    if (n == 0) { *out_n = 0; return NULL; }
    uint64_t *a = (uint64_t *)malloc((size_t)n * sizeof(uint64_t));
    if (!a) { *out_n = 0; return NULL; }
    for (uint64_t i = 0; i < n; i++) a[i] = i * (uint64_t)DISK_RAND_SZ;
    uint32_t s = 0x2545f491u;
    for (uint64_t i = n - 1; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        uint64_t j = (uint64_t)(s % (uint32_t)(i + 1));
        uint64_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
    *out_n = n;
    return a;
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

char *aio_disk_run_ex(int size_mb, int defeat_cache, aio_disk_progress_fn progress, void *user,
                      AioDiskResult *out) {
    if (out) { memset(out, 0, sizeof(*out)); }
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
        if (out) snprintf(out->err, sizeof(out->err), "Could not allocate the I/O buffer.");
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
    uint64_t rand_ops = 0, randw_ops = 0;  // 4 KiB ops completed in the 7 s window
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
            uint64_t plan_n = 0;
            uint64_t *plan = make_shuffled_offsets(total / DISK_RAND_SZ, &plan_n);
            if (!plan) {
                err = "Out of memory building the random-access plan.";
            } else {
                double t0 = now_sec(freq);
                for (uint64_t i = 0; i < plan_n && !err; i++) {
                    LARGE_INTEGER li;
                    li.QuadPart = (LONGLONG)plan[i];
                    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) { err = "Seek failed."; break; }
                    DWORD got = 0;
                    if (!ReadFile(h, buf, DISK_RAND_SZ, &got, NULL) || got != DISK_RAND_SZ) {
                        err = "Random read failed."; break;
                    }
                    rand_ops++;
                    // CPDT-style fixed window: stop after 7 s (check every 256 ops).
                    if ((i & 255) == 0 && now_sec(freq) - t0 >= DISK_RAND_SECS) break;
                }
                double t1 = now_sec(freq);
                free(plan);
                if (!err) {
                    t_rand = t1 - t0;
                    if (t_rand > 0.0 && rand_ops > 0) {
                        rand_iops = (double)rand_ops / t_rand;
                        rand_mbps = (double)rand_ops * DISK_RAND_SZ / 1e6 / t_rand;
                    }
                }
            }
            CloseHandle(h);
        }
    }

    // ---- Random 4 KB write (LAST; WRITE_THROUGH + per-op flush; 7 s window) ----
    // Each scattered 4 KiB write is committed to the device before the next
    // (WRITE_THROUGH handle + a FlushFileBuffers per op). That's the Wine analog of
    // CPDT's per-op fsync on Android (write-buffering off) and gives the same
    // committed-write speed (~tens of MB/s). Without the per-op flush, Wine ignores
    // WRITE_THROUGH and the writes coalesce in cache, reporting RAM speed. Writes hit
    // a shuffled set of unique offsets for up to 7 s. Runs after the reads so it
    // doesn't dirty the file beforehand.
    if (!err) {
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            uint64_t plan_n = 0;
            uint64_t *plan = make_shuffled_offsets(total / DISK_RAND_SZ, &plan_n);
            if (!plan) {
                err = "Out of memory building the random-access plan.";
            } else {
                double t0 = now_sec(freq);
                for (uint64_t i = 0; i < plan_n && !err; i++) {
                    LARGE_INTEGER li;
                    li.QuadPart = (LONGLONG)plan[i];
                    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) { err = "Seek failed."; break; }
                    DWORD wrote = 0;
                    if (!WriteFile(h, buf, DISK_RAND_SZ, &wrote, NULL) || wrote != DISK_RAND_SZ) {
                        err = "Random write failed."; break;
                    }
                    FlushFileBuffers(h);  // commit THIS write before timing the next
                    randw_ops++;
                    if ((randw_ops & 31) == 0 && now_sec(freq) - t0 >= DISK_RAND_SECS) break;
                }
                double t1 = now_sec(freq);
                free(plan);
                if (!err) {
                    t_randw = t1 - t0;
                    if (t_randw > 0.0 && randw_ops > 0) {
                        randw_iops = (double)randw_ops / t_randw;
                        randw_mbps = (double)randw_ops * DISK_RAND_SZ / 1e6 / t_randw;
                    }
                }
            }
            CloseHandle(h);
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
        if (out) snprintf(out->err, sizeof(out->err), "%s", err);
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
             "Random 4K read   : %7.1f MB/s   (%.0f IOPS, %llu reads / %.1fs)\r\n"
             "Random 4K write  : %7.1f MB/s   (%.0f IOPS, %llu writes / %.1fs)\r\n\r\n"
             "Storage class    : ~ %s\r\n"
             "                   (or better - in-container estimate%s)\r\n\r\n"
             "Throughput is decimal MB/s (1,000,000 bytes). Random tests run a fixed\r\n"
             "%.0f s over shuffled unique 4 KiB offsets (CPDT-matched).\r\n%s",
             path, file_mib, write_mbps, t_write, read_mbps, t_read,
             rand_mbps, rand_iops, (unsigned long long)rand_ops, t_rand,
             randw_mbps, randw_iops, (unsigned long long)randw_ops, t_randw,
             cls, defeat_cache ? "" : "; run Real-Flash Read for a read-based class",
             DISK_RAND_SECS, note);
    if (out) {
        out->ok = 1;
        out->seq_read_mbps = read_mbps;
        out->seq_write_mbps = write_mbps;
        out->rand_read_mbps = rand_mbps;
        out->rand_write_mbps = randw_mbps;
        out->rand_read_iops = rand_iops;
        out->rand_write_iops = randw_iops;
        snprintf(out->storage_class, sizeof(out->storage_class), "%s", cls);
    }
    return report;
}

// Text-only wrapper (unchanged public contract; menu.c / CLI use this).
char *aio_disk_run(int size_mb, int defeat_cache, aio_disk_progress_fn progress, void *user) {
    return aio_disk_run_ex(size_mb, defeat_cache, progress, user, NULL);
}

// Delete any leftover temp files from a previous run and report how much was freed.
// A normal run already deletes the test file + cache-buster, but if the app was
// closed (or a backend hung and the watchdog killed the process) mid-run, the
// (possibly multi-GB) files can be left behind. This is the manual safety net.
char *aio_disk_cleanup(void) {
    char *report = (char *)malloc(320);
    if (!report) return NULL;

    char dir[MAX_PATH], path[MAX_PATH], buster[MAX_PATH];
    DWORD dn = GetTempPathA(sizeof(dir), dir);
    if (dn == 0 || dn >= sizeof(dir)) strcpy(dir, ".\\");
    snprintf(path, sizeof(path), "%sAIO-Graphics-Test_disk.tmp", dir);
    snprintf(buster, sizeof(buster), "%sAIO-Graphics-Test_buster.tmp", dir);

    const char *files[2] = { path, buster };
    uint64_t freed = 0;
    int deleted = 0;
    for (int i = 0; i < 2; i++) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(files[i], GetFileExInfoStandard, &fad)) {
            uint64_t sz = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            if (DeleteFileA(files[i])) { freed += sz; deleted++; }
        }
    }

    if (deleted == 0)
        snprintf(report, 320,
                 "Disk Read / Write Speed\r\n"
                 "=======================\r\n\r\n"
                 "No leftover temp files found.\r\n\r\n"
                 "A normal run already deletes its files automatically; this only\r\n"
                 "matters if a run was interrupted (app closed mid-test).");
    else
        snprintf(report, 320,
                 "Disk Read / Write Speed\r\n"
                 "=======================\r\n\r\n"
                 "Cleaned up %d leftover temp file%s - freed %.1f MiB.\r\n\r\n"
                 "Folder: %s",
                 deleted, deleted == 1 ? "" : "s",
                 (double)freed / (1024.0 * 1024.0), dir);
    return report;
}
