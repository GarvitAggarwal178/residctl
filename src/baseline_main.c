// baseline_main.c -- build-order item 10: MECHANISM_SPEC §11 arms A and B.
//
// Arm A: mmap the model read-only, madvise swept (this binary takes the
// mode as a CLI arg; the harness invokes it once per mode and reports the
// best, same "harness sweeps, binary executes one config" pattern as
// everywhere else in this project).
// Arm B: A + MADV_WILLNEED (upfront hint) + MADV_PAGEOUT (per-chunk lagging
// hint, applied to the immediately-preceding chunk after each touch).
//
// Runs the SAME cyclic touch pattern as replay_cyclic() (§6.3/item 6) for a
// fair comparison, and MUST run inside the same cgroup (identical
// memory.max) as arms C/D/E -- here, residency is entirely the KERNEL's
// decision under memory pressure, not this program's; that's the point of
// a baseline arm.
//
// Usage: baseline_main <cgroup_path> <model_path> <region_len> <chunk_size>
//                       <n_passes> <madvise_mode: normal|random|sequential>
//                       <hints: on|off>
#define _GNU_SOURCE
#include "cgroup_stat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// /proc/self/stat field 10 (1-indexed) is majflt. Field 2 (comm) may
// contain spaces/parens, so skip past the last ')' first.
static uint64_t read_majflt(void) {
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    p += 2; // skip ") "
    unsigned long majflt = 0;
    // fields after comm: state(1) ppid(2) pgrp(3) session(4) tty_nr(5)
    // tpgid(6) flags(7) minflt(8) cminflt(9) majflt(10)
    int field = 0;
    char *tok = strtok(p, " ");
    while (tok) {
        field++;
        if (field == 10) { majflt = strtoul(tok, NULL, 10); break; }
        tok = strtok(NULL, " ");
    }
    return (uint64_t)majflt;
}

static uint64_t read_io_read_bytes(void) {
    FILE *f = fopen("/proc/self/io", "r");
    if (!f) return UINT64_MAX; // sentinel: not available on this kernel/config
    char line[256];
    uint64_t val = UINT64_MAX;
    while (fgets(line, sizeof line, f)) {
        unsigned long long v;
        if (sscanf(line, "read_bytes: %llu", &v) == 1) { val = v; break; }
    }
    fclose(f);
    return val;
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr, "usage: %s <cgroup_path> <model_path> <region_len> <chunk_size> "
                        "<n_passes> <madvise_mode:normal|random|sequential> <hints:on|off>\n", argv[0]);
        return 2;
    }
    const char *cgroup_path = argv[1];
    const char *model_path = argv[2];
    uint64_t region_len = strtoull(argv[3], NULL, 10);
    uint64_t chunk_size = strtoull(argv[4], NULL, 10);
    uint32_t n_passes = (uint32_t)strtoul(argv[5], NULL, 10);
    const char *madvise_mode = argv[6];
    int hints_on = (strcmp(argv[7], "on") == 0);
    if (!hints_on && strcmp(argv[7], "off") != 0) {
        fprintf(stderr, "hints must be 'on' or 'off'\n"); return 2;
    }

    int fd = open(model_path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", model_path, strerror(errno)); return 1; }
    struct stat st;
    fstat(fd, &st);
    if ((uint64_t)st.st_size < region_len) {
        fprintf(stderr, "model file %s is smaller than region_len\n", model_path); return 1;
    }

    uint8_t *map = mmap(NULL, region_len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { fprintf(stderr, "mmap: %s\n", strerror(errno)); return 1; }

    int advice;
    if (strcmp(madvise_mode, "normal") == 0) advice = MADV_NORMAL;
    else if (strcmp(madvise_mode, "random") == 0) advice = MADV_RANDOM;
    else if (strcmp(madvise_mode, "sequential") == 0) advice = MADV_SEQUENTIAL;
    else { fprintf(stderr, "unknown madvise_mode '%s'\n", madvise_mode); return 2; }
    if (madvise(map, region_len, advice) != 0)
        fprintf(stderr, "WARNING: madvise(%s) failed: %s\n", madvise_mode, strerror(errno));

    if (hints_on) {
        if (madvise(map, region_len, MADV_WILLNEED) != 0)
            fprintf(stderr, "WARNING: madvise(MADV_WILLNEED) failed: %s\n", strerror(errno));
    }

    uint32_t n_chunks = (uint32_t)(region_len / chunk_size);

    cgroup_stat_snapshot_t snap_before;
    int have_before = (cgroup_stat_snapshot_read(cgroup_path, "memory.stat", &snap_before) == 0);
    uint64_t pgscan_before = 0, pgsteal_before = 0, refault_before = 0;
    if (have_before) {
        cgroup_stat_snapshot_field(&snap_before, "pgscan", &pgscan_before);
        cgroup_stat_snapshot_field(&snap_before, "pgsteal", &pgsteal_before);
        cgroup_stat_snapshot_field(&snap_before, "workingset_refault_file", &refault_before);
    }
    uint64_t majflt_before = read_majflt();
    uint64_t io_before = read_io_read_bytes();

    uint64_t t_start = now_ns();
    uint32_t touches = 0;
    for (uint32_t pass = 0; pass < n_passes; pass++) {
        for (uint32_t i = 0; i < n_chunks; i++) {
            volatile uint8_t x = map[(uint64_t)i * chunk_size];
            (void)x;
            touches++;
            if (hints_on && i >= 1) {
                // Lagging PAGEOUT hint: the chunk immediately behind the
                // current one is a reasonable "probably cold now" guess
                // for a cyclic scan, without baking budget-awareness into
                // this binary -- actual residency enforcement is the
                // kernel's, via the cgroup's memory.max, same as the rest
                // of arm A/B's design.
                madvise(map + (uint64_t)(i - 1) * chunk_size, chunk_size, MADV_PAGEOUT);
            }
        }
    }
    uint64_t t_end = now_ns();

    cgroup_stat_snapshot_t snap_after;
    int have_after = (cgroup_stat_snapshot_read(cgroup_path, "memory.stat", &snap_after) == 0);
    uint64_t pgscan_after = 0, pgsteal_after = 0, refault_after = 0;
    if (have_after) {
        cgroup_stat_snapshot_field(&snap_after, "pgscan", &pgscan_after);
        cgroup_stat_snapshot_field(&snap_after, "pgsteal", &pgsteal_after);
        cgroup_stat_snapshot_field(&snap_after, "workingset_refault_file", &refault_after);
    }
    uint64_t majflt_after = read_majflt();
    uint64_t io_after = read_io_read_bytes();

    double seconds = (double)(t_end - t_start) / 1e9;
    printf("BASELINE SUMMARY (arm %s)\n", hints_on ? "B" : "A");
    printf("  madvise_mode=%s hints=%s passes=%u touches=%u wall_ns=%llu (%.3fs) touches/sec=%.1f\n",
           madvise_mode, hints_on ? "on" : "off", n_passes, touches,
           (unsigned long long)(t_end - t_start), seconds, seconds > 0 ? (double)touches / seconds : 0.0);
    printf("  majflt_delta=%llu\n", (unsigned long long)(majflt_after - majflt_before));
    if (io_before != UINT64_MAX && io_after != UINT64_MAX)
        printf("  io_read_bytes_delta=%llu\n", (unsigned long long)(io_after - io_before));
    else
        printf("  io_read_bytes_delta=NOT_AVAILABLE (/proc/self/io not readable on this system)\n");
    if (have_before && have_after)
        printf("  pgscan_delta=%llu pgsteal_delta=%llu workingset_refault_file_delta=%llu\n",
               (unsigned long long)(pgscan_after - pgscan_before),
               (unsigned long long)(pgsteal_after - pgsteal_before),
               (unsigned long long)(refault_after - refault_before));
    else
        printf("  memory.stat fields NOT_AVAILABLE (cgroup read failed)\n");

    printf("ARM_CSV,policy=mmap_%s_%s,prefetch=n/a,budget_bytes=n/a,touches=%u,wall_ns=%llu,"
           "absent_handled=n/a,evictions=n/a,infeasible=n/a,prefetches=n/a,"
           "majflt_delta=%llu,pgscan_delta=%llu,pgsteal_delta=%llu\n",
           madvise_mode, hints_on ? "on" : "off", touches, (unsigned long long)(t_end - t_start),
           (unsigned long long)(majflt_after - majflt_before),
           have_before && have_after ? (unsigned long long)(pgscan_after - pgscan_before) : 0,
           have_before && have_after ? (unsigned long long)(pgsteal_after - pgsteal_before) : 0);

    munmap(map, region_len);
    close(fd);
    printf("PASS\n");
    return 0;
}
