// test_trace.c -- build-order item 5 verification: trace recorder + metrics.
//
// Usage: test_trace <cgroup_path> <model_path> <trace_out_path>
#define _GNU_SOURCE
#include "region.h"
#include "pager.h"
#include "trace.h"
#include "metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>

#define REGION_LEN (16ULL * 1024 * 1024)
#define CHUNK_SIZE (2ULL * 1024 * 1024) // 8 chunks

static region_t g_r;

typedef struct { region_t *r; volatile sig_atomic_t *stop; } pager_args_t;
static void *pager_trampoline(void *argp) {
    pager_args_t *p = (pager_args_t *)argp;
    pager_run(p->r, p->stop, 50);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <cgroup_path> <model_path> <trace_out_path>\n", argv[0]);
        return 2;
    }
    const char *trace_path = argv[3];

    region_config_t cfg = {
        .region_len = REGION_LEN,
        .chunk_size = CHUNK_SIZE,
        .model_path = argv[2],
        .cgroup_path = argv[1],
        .budget_bytes = REGION_LEN, // full budget: no eviction, isolates the trace/metrics test
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);

    trace_t *trace = trace_open(trace_path, TRACE_TYPE_FAULT); // pager's own trace: metrics-only, never solver input
    metrics_t metrics;
    metrics_init(&metrics);
    g_r.trace = trace;
    g_r.metrics = &metrics;

    printf("region_startup OK: n_chunks=%u\n", g_r.n_chunks);

    volatile sig_atomic_t stop = 0;
    pthread_t pager_thread;
    pager_args_t pager_args = { &g_r, &stop };
    pthread_create(&pager_thread, NULL, pager_trampoline, &pager_args);

    for (uint32_t i = 0; i < g_r.n_chunks; i++) {
        chunk_t *c = &g_r.chunks[i];
        volatile uint8_t x = g_r.map_a[c->region_off];
        (void)x;
        usleep(30 * 1000);
    }

    stop = 1;
    pthread_join(pager_thread, NULL);
    trace_close(trace);
    g_r.trace = NULL;

    printf("stat_absent_handled=%llu trace records_written(before close, not re-readable)=n/a\n",
           (unsigned long long)g_r.stat_absent_handled);
    printf("metrics: handler_latency count=%llu min_ns=%llu max_ns=%llu p50_ns=%llu p99_ns=%llu queue_depth_high_water=%u\n",
           (unsigned long long)metrics.handler_latency.count,
           (unsigned long long)metrics.handler_latency.min_ns,
           (unsigned long long)metrics.handler_latency.max_ns,
           (unsigned long long)latency_hist_percentile_ns(&metrics.handler_latency, 0.50),
           (unsigned long long)latency_hist_percentile_ns(&metrics.handler_latency, 0.99),
           metrics.queue_depth_high_water);

    // Read the trace file back and check it independently of in-process state.
    int fd = open(trace_path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "FAIL: cannot reopen trace file %s\n", trace_path); return 1; }

    uint8_t hdr_type;
    if (trace_read_header(fd, &hdr_type) != 0) { fprintf(stderr, "FAIL: bad trace header\n"); return 1; }
    if (hdr_type != TRACE_TYPE_FAULT) { fprintf(stderr, "FAIL: expected TRACE_TYPE_FAULT header\n"); return 1; }

    int fail = 0;
    trace_record_t rec;
    uint64_t n_records = 0;
    uint64_t expect_seq = 1;
    uint32_t expect_chunk_id = 0;
    for (;;) {
        ssize_t n = read(fd, &rec, sizeof rec);
        if (n == 0) break;
        if (n != (ssize_t)sizeof rec) {
            fprintf(stderr, "FAIL: short/misaligned record read (%zd bytes)\n", n);
            fail = 1;
            break;
        }
        n_records++;
        if (rec.seq != expect_seq) {
            fprintf(stderr, "FAIL: record %llu has seq=%llu, expected %llu (monotonic per-fault seq)\n",
                    (unsigned long long)n_records, (unsigned long long)rec.seq, (unsigned long long)expect_seq);
            fail = 1;
        }
        if (rec.chunk_id != expect_chunk_id) {
            fprintf(stderr, "FAIL: record %llu has chunk_id=%u, expected %u (sequential touch order)\n",
                    (unsigned long long)n_records, rec.chunk_id, expect_chunk_id);
            fail = 1;
        }
        if (rec.fault_type != TRACE_FAULT_MISSING) {
            // Fresh memfd, sequential single-threaded touches: every fault
            // should be MISSING. A MINOR here would mean something touched
            // this chunk's backing pages before the fault, which shouldn't
            // happen in this single-threaded, budget-unconstrained scenario.
            fprintf(stderr, "FAIL: record %llu has fault_type=%u, expected MISSING(0)\n",
                    (unsigned long long)n_records, rec.fault_type);
            fail = 1;
        }
        if (rec.was_prefetched != 0) {
            fprintf(stderr, "FAIL: record %llu has was_prefetched=%u, expected 0 (item 8 not built)\n",
                    (unsigned long long)n_records, rec.was_prefetched);
            fail = 1;
        }
        expect_seq++;
        expect_chunk_id++;
    }
    close(fd);

    printf("trace file: %llu records read back\n", (unsigned long long)n_records);
    if (n_records != g_r.n_chunks) {
        fprintf(stderr, "FAIL: expected %u trace records (one per chunk), got %llu\n",
                g_r.n_chunks, (unsigned long long)n_records);
        fail = 1;
    }

    region_teardown(&g_r);
    if (fail) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
