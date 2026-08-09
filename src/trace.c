// trace.c -- MECHANISM_SPEC.md §9.
#define _GNU_SOURCE
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

trace_t *trace_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "trace_open: cannot open %s: %s\n", path, strerror(errno));
        abort();
    }
    trace_t *t = calloc(1, sizeof *t);
    if (!t) { fprintf(stderr, "trace_open: calloc failed\n"); abort(); }
    t->fd = fd;
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

    const char *buf = (const char *)&rec;
    size_t remaining = sizeof rec;
    while (remaining > 0) {
        ssize_t n = write(t->fd, buf, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "trace_record: write failed: %s\n", strerror(errno));
            abort();
        }
        buf += n;
        remaining -= (size_t)n;
    }
    t->records_written++;
}

void trace_close(trace_t *t) {
    if (!t) return;
    close(t->fd);
    free(t);
}
