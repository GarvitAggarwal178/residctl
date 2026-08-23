// policy_trace.c -- see policy_trace.h.
#define _GNU_SOURCE
#include "policy_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

policy_trace_t *policy_trace_open(const char *path, uint64_t capacity) {
    policy_trace_t *pt = calloc(1, sizeof *pt);
    if (!pt) { fprintf(stderr, "policy_trace_open: calloc failed\n"); abort(); }
    pt->records = calloc(capacity, sizeof(policy_trace_record_t));
    if (!pt->records) {
        fprintf(stderr, "policy_trace_open: calloc(%llu records) failed\n",
                (unsigned long long)capacity);
        abort();
    }
    pt->capacity = capacity;
    pt->count = 0;
    pt->next_seq = 1;
    snprintf(pt->path, sizeof pt->path, "%s", path);
    return pt;
}

// Caller already holds r->budget_lock (every select_victim() call site
// does) -- no separate lock needed here, unlike fetch_trace which is
// reserved from multiple concurrent worker threads with no shared lock at
// that point.
policy_trace_record_t *policy_trace_reserve(policy_trace_t *pt) {
    if (pt->count >= pt->capacity) {
        fprintf(stderr, "POLICY_TRACE FAILED: capacity %llu exceeded -- diagnostic run undersized "
                        "its --policy-trace buffer, not truncating silently\n",
                (unsigned long long)pt->capacity);
        abort();
    }
    policy_trace_record_t *rec = &pt->records[pt->count++];
    memset(rec, 0, sizeof *rec);
    rec->seq = pt->next_seq++;
    return rec;
}

void policy_trace_flush(policy_trace_t *pt) {
    int fd = open(pt->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "policy_trace_flush: cannot open %s: %s\n", pt->path, strerror(errno));
        return;
    }
    size_t total = pt->count * sizeof(policy_trace_record_t);
    const char *buf = (const char *)pt->records;
    size_t off = 0;
    while (off < total) {
        ssize_t n = write(fd, buf + off, total - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "policy_trace_flush: write failed: %s\n", strerror(errno));
            break;
        }
        off += (size_t)n;
    }
    close(fd);
    fprintf(stderr, "policy_trace: wrote %llu records to %s\n", (unsigned long long)pt->count, pt->path);
}

void policy_trace_close(policy_trace_t *pt) {
    if (!pt) return;
    free(pt->records);
    free(pt);
}
