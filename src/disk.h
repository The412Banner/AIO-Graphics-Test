// AIO Graphics Test - disk (drive) read/write speed test.
//
// Measures sequential write, sequential read, and random 4 KB read throughput
// against a temporary file, then reports MB/s + IOPS. Useful on Winlator/Wine
// to see how fast the mapped Android storage actually is for game I/O.
#ifndef AIO_DISK_H
#define AIO_DISK_H

// Progress callback: invoked with a cumulative, human-readable report snapshot
// as each phase (write / sequential read / random read) completes, so a UI can
// update live. The text is owned by aio_disk_run and is only valid for the
// duration of the call - copy it if you need to keep it. May be NULL.
typedef void (*aio_disk_progress_fn)(void *user, const char *text);

// Run the disk benchmark using a temp file of size_mb mebibytes (rounded up to a
// multiple of the 4 MiB block size). Returns a heap-allocated final report that
// the caller must free(). progress/user may be NULL.
char *aio_disk_run(int size_mb, aio_disk_progress_fn progress, void *user);

#endif  // AIO_DISK_H
