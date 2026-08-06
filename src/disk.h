// AIO Graphics Test - disk (drive) read/write speed test.
//
// Measures sequential write, sequential read, random 4 KB read, and random 4 KB
// write throughput against a temporary file, then reports MB/s + IOPS. Useful on
// Winlator/Wine to see how fast the mapped Android storage is for game I/O.
#ifndef AIO_DISK_H
#define AIO_DISK_H

// Progress callback: invoked with a cumulative, human-readable report snapshot
// as each phase completes, so a UI can update live. The text is owned by
// aio_disk_run and is only valid for the duration of the call - copy it if you
// need to keep it. May be NULL.
typedef void (*aio_disk_progress_fn)(void *user, const char *text);

// Run the disk benchmark using a temp file of size_mb mebibytes (rounded up to a
// multiple of the 4 MiB block size).
//
// defeat_cache: 0 = quick mode (read served from the OS page cache - fast but
// the read figure is inflated). 1 = real-flash mode: after writing the test
// file, a second "cache-buster" file ~= the device's RAM is written to evict the
// test file from the page cache, so the read pass hits the storage cold. Slower
// and writes several extra GB (deleted afterwards), but the read reflects real
// flash speed.
//
// Returns a heap-allocated final report that the caller must free().
// progress/user may be NULL.
char *aio_disk_run(int size_mb, int defeat_cache, aio_disk_progress_fn progress, void *user);

// Structured result for the ImGui shell's Disk Speed tiles. All figures are the
// SAME ones aio_disk_run computes (decimal MB/s + IOPS); nothing is fabricated.
typedef struct {
    int ok;                    // 1 if the run completed (else err[] is set)
    double seq_read_mbps, seq_write_mbps;
    double rand_read_mbps, rand_write_mbps;
    double rand_read_iops, rand_write_iops;
    char storage_class[48];    // e.g. "UFS 3.1"
    char err[160];             // failure reason when ok==0
} AioDiskResult;

// Same as aio_disk_run but also fills *out (may be NULL) with the structured
// figures. Still returns the heap text report (caller frees).
char *aio_disk_run_ex(int size_mb, int defeat_cache, aio_disk_progress_fn progress, void *user,
                      AioDiskResult *out);

// Delete any leftover temp files (test file + cache-buster) from an interrupted
// run and report how much space was freed. Normal runs clean up automatically;
// this is the manual safety net. Returns a heap report the caller must free().
char *aio_disk_cleanup(void);

#endif  // AIO_DISK_H
