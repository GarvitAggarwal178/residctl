// trace.c -- MECHANISM_SPEC.md §9. See trace.h for the reference-vs-fault
// trace distinction (spec amendment A-1).
#define _GNU_SOURCE
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

static void write_all(int fd, const void *buf, size_t len, const char *what) {
    const char *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "trace: write(%s) failed: %s\n", what, strerror(errno));
            abort();
        }
        p += n;
        remaining -= (size_t)n;
    }
}

trace_t *trace_open(const char *path, uint8_t trace_type) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "trace_open: cannot open %s: %s\n", path, strerror(errno));
        abort();
    }

    trace_header_t hdr;
    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.magic, TRACE_MAGIC, 4);
    hdr.trace_type = trace_type;
    hdr.version = TRACE_FORMAT_VERSION;
    write_all(fd, &hdr, sizeof hdr, "header");

    trace_t *t = calloc(1, sizeof *t);
    if (!t) { fprintf(stderr, "trace_open: calloc failed\n"); abort(); }
    t->fd = fd;
    t->trace_type = trace_type;
    t->records_written = 0;
    return t;
}

void trace_record(trace_t *t, uint64_t seq, uint32_t chunk_id, uint8_t fault_type, uint8_t was_prefetched) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    trace_record_t rec;
    memset(&rec, 0, sizeof rec);
    rec.seq = seq;
    rec.timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    rec.chunk_id = chunk_id;
    rec.fault_type = fault_type;
    rec.was_prefetched = was_prefetched;

    write_all(t->fd, &rec, sizeof rec, "record");
    t->records_written++;
}

void trace_close(trace_t *t) {
    if (!t) return;
    close(t->fd);
    free(t);
}

int trace_read_header(int fd, uint8_t *out_type) {
    trace_header_t hdr;
    ssize_t n = read(fd, &hdr, sizeof hdr);
    if (n != (ssize_t)sizeof hdr) return -1;
    if (memcmp(hdr.magic, TRACE_MAGIC, 4) != 0) return -1;
    if (hdr.version != TRACE_FORMAT_VERSION) return -1;
    *out_type = hdr.trace_type;
    return 0;
}
