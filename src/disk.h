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

#endif  // AIO_DISK_H
