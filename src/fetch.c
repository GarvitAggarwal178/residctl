// fetch.c -- MECHANISM_SPEC.md §6.1-6.2.
#define _GNU_SOURCE
#include "fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/userfaultfd.h>

static uint64_t align_down(uint64_t v, uint64_t a) { return v - (v % a); }
static uint64_t align_up(uint64_t v, uint64_t a) { return align_down(v + a - 1, a); }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void fetch_fail(const char *step, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FETCH FAILED at step [%s]: ", step);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

// §6.1: read the chunk into map_b, looping over short reads.
static void fetch_read(region_t *r, chunk_t *c) {
    // General form even though the current (item 1) chunk table always
    // produces file_off == region_off, both already 4096-aligned -- so
    // slack is 0 today. This keeps the code correct if a future,
    // tensor-metadata-driven chunk table (build-order item 11) produces
    // chunks whose file offsets aren't naturally aligned, per §6.1.
    uint64_t aligned_start = align_down(c->file_off, RESIDCTL_ALIGN);
    uint64_t aligned_end = align_up(c->file_off + c->len, RESIDCTL_ALIGN);
    uint64_t slack = c->file_off - aligned_start;
    uint8_t *dest = r->map_b + c->region_off - slack;

    if (((uintptr_t)dest) % RESIDCTL_ALIGN != 0)
        fetch_fail("fetch_read", "dest %p not %llu-aligned (chunk region_off=%llu file_off=%llu)",
                   (void *)dest, (unsigned long long)RESIDCTL_ALIGN,
                   (unsigned long long)c->region_off, (unsigned long long)c->file_off);

    // WP2 (§6.1): a real GGUF is not a 4096 multiple, so the LAST chunk's
    // 4096-aligned end runs past EOF. Split the read at the last 4096
    // boundary <= EOF: O_DIRECT for the aligned bulk, one buffered pread for
    // the sub-4096 tail (O_DIRECT rejects unaligned partial reads), then
    // zero-fill [file_size, aligned_end) -- padding, never a tensor byte
    // (the region maps the file 1:1). Guarded by model_file_size != 0 so the
    // replay path (file_size == region_len, both 4096-aligned) is unchanged.
    uint64_t direct_end = aligned_end;   // bytes to read via r->model_fd (aligned)
    uint64_t tail_start = aligned_end, tail_end = aligned_end; // buffered, [tail_start, tail_end)
    if (r->model_file_size != 0 && aligned_end > r->model_file_size) {
        uint64_t fs = r->model_file_size;
        uint64_t last_aligned = align_down(fs, RESIDCTL_ALIGN);
        direct_end = (last_aligned > aligned_start) ? last_aligned : aligned_start;
        tail_start = direct_end;
        tail_end   = fs;
        memset(dest + (fs - aligned_start), 0, aligned_end - fs); // pad
    }

    uint64_t off = 0;
    while (aligned_start + off < direct_end) {
        uint64_t want = direct_end - (aligned_start + off);
        ssize_t n = pread(r->model_fd, dest + off, want, (off_t)(aligned_start + off));
        if (n < 0) {
            if (errno == EINTR) continue;
            fetch_fail("fetch_read", "pread(off=%llu, len=%llu) failed: %s",
                       (unsigned long long)(aligned_start + off),
                       (unsigned long long)want, strerror(errno));
        }
        if (n == 0)
            fetch_fail("fetch_read", "unexpected EOF at file offset %llu (chunk file_off=%llu len=%llu, model file too short)",
                       (unsigned long long)(aligned_start + off),
                       (unsigned long long)c->file_off, (unsigned long long)c->len);
        off += (uint64_t)n;
    }
    // buffered tail (only when the chunk crosses EOF and there is a sub-4096 remainder)
    while (tail_start < tail_end) {
        uint64_t dst_off = tail_start - aligned_start;
        ssize_t n = pread(r->model_fd_buf >= 0 ? r->model_fd_buf : r->model_fd,
                          dest + dst_off, tail_end - tail_start, (off_t)tail_start);
        if (n < 0) {
            if (errno == EINTR) continue;
            fetch_fail("fetch_read", "buffered tail pread(off=%llu) failed: %s",
                       (unsigned long long)tail_start, strerror(errno));
        }
        if (n == 0)
            fetch_fail("fetch_read", "unexpected EOF in buffered tail at %llu (file_size=%llu)",
                       (unsigned long long)tail_start, (unsigned long long)r->model_file_size);
        tail_start += (uint64_t)n;
    }
}

// §6.2: resolve via UFFDIO_CONTINUE, asserting full coverage (I-5).
static void fetch_resolve(region_t *r, chunk_t *c) {
    struct uffdio_continue cont;
    memset(&cont, 0, sizeof cont);
    cont.range.start = (unsigned long)(r->map_a + c->region_off);
    cont.range.len = c->len;
    cont.mode = 0; // wake waiters

    if (ioctl(r->uffd, UFFDIO_CONTINUE, &cont) != 0)
        fetch_fail("fetch_resolve", "UFFDIO_CONTINUE failed: %s", strerror(errno));
    if (cont.mapped != (int64_t)c->len)
        fetch_fail("fetch_resolve", "mapped=%lld, expected chunk->len=%llu (I-5 violation)",
                   (long long)cont.mapped, (unsigned long long)c->len);
}

void fetch_chunk(region_t *r, chunk_t *c, fetch_timing_t *out_timing) {
    if (out_timing) out_timing->read_start_ns = now_ns();
    fetch_read(r, c);     // §6.1, I-5's "every byte exists before CONTINUE"
    if (out_timing) out_timing->read_end_ns = now_ns();
    fetch_resolve(r, c);  // §6.2
    if (out_timing) out_timing->continue_end_ns = now_ns();
}
