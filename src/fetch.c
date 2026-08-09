// fetch.c -- MECHANISM_SPEC.md §6.1-6.2.
#define _GNU_SOURCE
#include "fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/userfaultfd.h>

static uint64_t align_down(uint64_t v, uint64_t a) { return v - (v % a); }
static uint64_t align_up(uint64_t v, uint64_t a) { return align_down(v + a - 1, a); }

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

    uint64_t read_len = aligned_end - aligned_start;
    uint64_t off = 0;
    while (off < read_len) {
        ssize_t n = pread(r->model_fd, dest + off, read_len - off, (off_t)(aligned_start + off));
        if (n < 0) {
            if (errno == EINTR) continue;
            fetch_fail("fetch_read", "pread(off=%llu, len=%llu) failed: %s",
                       (unsigned long long)(aligned_start + off),
                       (unsigned long long)(read_len - off), strerror(errno));
        }
        if (n == 0)
            fetch_fail("fetch_read", "unexpected EOF at file offset %llu (chunk file_off=%llu len=%llu, model file too short)",
                       (unsigned long long)(aligned_start + off),
                       (unsigned long long)c->file_off, (unsigned long long)c->len);
        off += (uint64_t)n;
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

void fetch_chunk(region_t *r, chunk_t *c) {
    fetch_read(r, c);     // §6.1, I-5's "every byte exists before CONTINUE"
    fetch_resolve(r, c);  // §6.2
}
