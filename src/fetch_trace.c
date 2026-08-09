// fetch_trace.c -- see fetch_trace.h.
#define _GNU_SOURCE
#include "fetch_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

fetch_trace_t *fetch_trace_open(const char *path, uint64_t capacity) {
    fetch_trace_t *ft = calloc(1, sizeof *ft);
    if (!ft) { fprintf(stderr, "fetch_trace_open: calloc failed\n"); abort(); }
    ft->records = calloc(capacity, sizeof(fetch_trace_record_t));
    if (!ft->records) { fprintf(stderr, "fetch_trace_open: calloc(%llu records) failed\n",
                                (unsigned long long)capacity); abort(); }
    ft->capacity = capacity;
    ft->count = 0;
    ft->next_seq = 1;
    pthread_mutex_init(&ft->lock, NULL);
    snprintf(ft->path, sizeof ft->path, "%s", path);
    return ft;
}

fetch_trace_record_t *fetch_trace_reserve(fetch_trace_t *ft) {
    pthread_mutex_lock(&ft->lock);
    if (ft->count >= ft->capacity) {
        fprintf(stderr, "FETCH_TRACE FAILED: capacity %llu exceeded -- diagnostic run undersized "
                        "its --fetch-trace buffer, not truncating silently\n",
                (unsigned long long)ft->capacity);
        abort();
    }
    fetch_trace_record_t *rec = &ft->records[ft->count++];
    memset(rec, 0, sizeof *rec);
    rec->seq = ft->next_seq++;
    pthread_mutex_unlock(&ft->lock);
    return rec;
}

void fetch_trace_flush(fetch_trace_t *ft) {
    int fd = open(ft->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "fetch_trace_flush: cannot open %s: %s\n", ft->path, strerror(errno));
        return;
    }
    size_t total = ft->count * sizeof(fetch_trace_record_t);
    const char *buf = (const char *)ft->records;
    size_t off = 0;
    while (off < total) {
        ssize_t n = write(fd, buf + off, total - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "fetch_trace_flush: write failed: %s\n", strerror(errno));
            break;
        }
        off += (size_t)n;
    }
    close(fd);
    fprintf(stderr, "fetch_trace: wrote %llu records to %s\n", (unsigned long long)ft->count, ft->path);
}

void fetch_trace_close(fetch_trace_t *ft) {
    if (!ft) return;
    pthread_mutex_destroy(&ft->lock);
    free(ft->records);
    free(ft);
}
