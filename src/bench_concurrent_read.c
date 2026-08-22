// bench_concurrent_read.c -- Phase 1 (Campaign 11) standalone platform I/O
// concurrency microbenchmark. No pager, no uffd, no cgroup: asks the
// platform directly whether concurrent O_DIRECT reads can overlap on this
// filesystem, removing every layer of pager-side dispatch/lock/budget
// machinery that earlier items' measurements were entangled with.
//
// Each of --threads threads reads a DISTINCT, NON-OVERLAPPING, contiguous,
// block-aligned range of the input file via repeated pread() calls of
// --block-size bytes each, until it has read its share of --total-bytes
// (total_bytes is rounded down to a multiple of threads*block_size so every
// thread's share is exact and block-aligned -- required for O_DIRECT).
// --shared-fd: all threads pread() the same fd (opened once, before any
//   thread starts) -- tests whether a single shared fd serializes.
// --fd-per-thread: each thread opens its OWN fd on the same path -- tests
//   whether that removes any serialization --shared-fd showed.
// --direct / --buffered: O_DIRECT vs plain buffered reads.
//
// Reports aggregate throughput (total bytes / wall-clock across the whole
// run) and per-thread throughput (each thread's own bytes / its own
// wall-clock) via a machine-parseable BENCH_RESULT line plus one line per
// thread.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef struct {
    int fd;               // >=0 if sharing a pre-opened fd; -1 => this thread opens its own
    const char *path;
    bool direct;
    uint64_t start_off;
    uint64_t len;         // bytes this thread must read, in total
    uint64_t block_size;
    uint64_t bytes_read;
    uint64_t wall_ns;
    volatile uint64_t sink; // elision guard, per thread
} thread_ctx_t;

static void *worker(void *argp) {
    thread_ctx_t *ctx = argp;
    int fd = ctx->fd;
    int owned_fd = -1;
    if (fd < 0) {
        int flags = O_RDONLY | (ctx->direct ? O_DIRECT : 0);
        fd = open(ctx->path, flags);
        if (fd < 0) { fprintf(stderr, "worker: open(%s) failed: %s\n", ctx->path, strerror(errno)); exit(1); }
        owned_fd = fd;
    }

    uint8_t *buf;
    if (posix_memalign((void **)&buf, 4096, ctx->block_size) != 0) {
        fprintf(stderr, "worker: posix_memalign(%llu) failed\n", (unsigned long long)ctx->block_size);
        exit(1);
    }

    uint64_t off = ctx->start_off;
    uint64_t remaining = ctx->len;
    uint64_t local_sink = 0;
    uint64_t t0 = now_ns();
    while (remaining > 0) {
        uint64_t this_read = remaining < ctx->block_size ? remaining : ctx->block_size;
        ssize_t n = pread(fd, buf, this_read, (off_t)off);
        if (n < 0) {
            fprintf(stderr, "worker: pread(off=%llu, len=%llu) failed: %s\n",
                    (unsigned long long)off, (unsigned long long)this_read, strerror(errno));
            exit(1);
        }
        if (n == 0) {
            fprintf(stderr, "worker: unexpected EOF at off=%llu\n", (unsigned long long)off);
            exit(1);
        }
        // Touch the data (avoid the read being elided; one byte per 4096-byte
        // page, same convention as the project's replay driver).
        for (uint64_t i = 0; i < (uint64_t)n; i += 4096) local_sink += buf[i];
        off += (uint64_t)n;
        remaining -= (uint64_t)n;
        ctx->bytes_read += (uint64_t)n;
    }
    uint64_t t1 = now_ns();
    ctx->wall_ns = t1 - t0;
    ctx->sink = local_sink;

    free(buf);
    if (owned_fd >= 0) close(owned_fd);
    return NULL;
}

int main(int argc, char **argv) {
    uint32_t threads = 1;
    uint64_t block_size = 4ULL * 1024 * 1024;
    uint64_t total_bytes = 1ULL * 1024 * 1024 * 1024;
    bool shared_fd = true;
    bool direct = true;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) threads = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--block-size") == 0 && i + 1 < argc) block_size = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--total-bytes") == 0 && i + 1 < argc) total_bytes = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--shared-fd") == 0) shared_fd = true;
        else if (strcmp(argv[i], "--fd-per-thread") == 0) shared_fd = false;
        else if (strcmp(argv[i], "--direct") == 0) direct = true;
        else if (strcmp(argv[i], "--buffered") == 0) direct = false;
        else path = argv[i];
    }
    if (!path || threads == 0 || block_size == 0) {
        fprintf(stderr, "usage: %s --threads N --block-size B [--shared-fd|--fd-per-thread] "
                        "[--direct|--buffered] --total-bytes T <path>\n", argv[0]);
        return 2;
    }

    uint64_t per_thread = (total_bytes / threads / block_size) * block_size;
    if (per_thread == 0) {
        fprintf(stderr, "total_bytes (%llu) too small for threads=%u * block_size=%llu\n",
                (unsigned long long)total_bytes, threads, (unsigned long long)block_size);
        return 2;
    }
    uint64_t actual_total = per_thread * threads;

    int shared_fd_num = -1;
    if (shared_fd) {
        int flags = O_RDONLY | (direct ? O_DIRECT : 0);
        shared_fd_num = open(path, flags);
        if (shared_fd_num < 0) { fprintf(stderr, "open(%s) failed: %s\n", path, strerror(errno)); return 1; }
    }

    pthread_t *tids = calloc(threads, sizeof(pthread_t));
    thread_ctx_t *ctxs = calloc(threads, sizeof(thread_ctx_t));
    if (!tids || !ctxs) { fprintf(stderr, "calloc failed\n"); return 1; }
    for (uint32_t t = 0; t < threads; t++) {
        ctxs[t].fd = shared_fd ? shared_fd_num : -1;
        ctxs[t].path = path;
        ctxs[t].direct = direct;
        ctxs[t].start_off = (uint64_t)t * per_thread;
        ctxs[t].len = per_thread;
        ctxs[t].block_size = block_size;
    }

    uint64_t t_start = now_ns();
    for (uint32_t t = 0; t < threads; t++) {
        if (pthread_create(&tids[t], NULL, worker, &ctxs[t]) != 0) {
            fprintf(stderr, "pthread_create failed\n"); return 1;
        }
    }
    for (uint32_t t = 0; t < threads; t++) pthread_join(tids[t], NULL);
    uint64_t t_end = now_ns();

    if (shared_fd_num >= 0) close(shared_fd_num);

    double wall_s = (double)(t_end - t_start) / 1e9;
    uint64_t total_read = 0, sink_total = 0;
    for (uint32_t t = 0; t < threads; t++) { total_read += ctxs[t].bytes_read; sink_total += ctxs[t].sink; }
    double agg_mibs = wall_s > 0 ? (double)total_read / 1048576.0 / wall_s : 0.0;

    printf("BENCH_RESULT,threads=%u,block_size=%llu,shared_fd=%d,direct=%d,total_bytes=%llu,"
           "wall_ns=%llu,wall_s=%.6f,agg_MiBps=%.2f,sink=%llu\n",
           threads, (unsigned long long)block_size, shared_fd ? 1 : 0, direct ? 1 : 0,
           (unsigned long long)actual_total, (unsigned long long)(t_end - t_start), wall_s, agg_mibs,
           (unsigned long long)sink_total);
    for (uint32_t t = 0; t < threads; t++) {
        double per_s = (double)ctxs[t].wall_ns / 1e9;
        double per_mibs = per_s > 0 ? (double)ctxs[t].bytes_read / 1048576.0 / per_s : 0.0;
        printf("  thread=%u bytes=%llu wall_ns=%llu MiBps=%.2f\n", t,
               (unsigned long long)ctxs[t].bytes_read, (unsigned long long)ctxs[t].wall_ns, per_mibs);
    }

    free(tids);
    free(ctxs);
    return 0;
}
