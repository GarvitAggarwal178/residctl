// cgroup_stat.c -- see cgroup_stat.h.
#define _GNU_SOURCE
#include "cgroup_stat.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

int cgroup_stat_snapshot_read(const char *cgroup_path, const char *filename, cgroup_stat_snapshot_t *snap) {
    char path[320];
    snprintf(path, sizeof path, "%s/%s", cgroup_path, filename);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    size_t total = 0;
    for (;;) {
        if (total >= sizeof(snap->buf) - 1) break;
        ssize_t n = read(fd, snap->buf + total, sizeof(snap->buf) - 1 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);
    snap->buf[total] = '\0';
    snap->len = total;
    return 0;
}

bool cgroup_stat_snapshot_field(const cgroup_stat_snapshot_t *snap, const char *key, uint64_t *out) {
    char line[256];
    size_t pos = 0;
    while (pos < snap->len) {
        size_t start = pos;
        while (pos < snap->len && snap->buf[pos] != '\n') pos++;
        size_t linelen = pos - start;
        if (linelen >= sizeof line) linelen = sizeof line - 1;
        memcpy(line, snap->buf + start, linelen);
        line[linelen] = '\0';
        pos++; // skip newline

        char k[128];
        unsigned long long v;
        if (sscanf(line, "%127s %llu", k, &v) == 2 && strcmp(k, key) == 0) {
            *out = (uint64_t)v;
            return true;
        }
    }
    return false;
}

uint64_t cgroup_stat_snapshot_sum_prefix(const cgroup_stat_snapshot_t *snap, const char *prefix, int *n_matched) {
    uint64_t sum = 0;
    int matched = 0;
    size_t prefix_len = strlen(prefix);

    char line[256];
    size_t pos = 0;
    while (pos < snap->len) {
        size_t start = pos;
        while (pos < snap->len && snap->buf[pos] != '\n') pos++;
        size_t linelen = pos - start;
        if (linelen >= sizeof line) linelen = sizeof line - 1;
        memcpy(line, snap->buf + start, linelen);
        line[linelen] = '\0';
        pos++;

        char k[128];
        unsigned long long v;
        if (sscanf(line, "%127s %llu", k, &v) == 2 && strncmp(k, prefix, prefix_len) == 0) {
            sum += (uint64_t)v;
            matched++;
        }
    }
    if (n_matched) *n_matched = matched;
    return sum;
}
