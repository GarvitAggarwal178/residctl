// test_replay.c -- build-order item 6 verification: replay_cyclic() drives
// a deterministic multi-pass cyclic sweep under eviction pressure, and the
// trace it produces is checked against a hand-derived expectation.
//
// Usage: test_replay <cgroup_path> <model_path> <trace_out_path>
#define _GNU_SOURCE
#include "region.h"
#include "pager.h"
#include "trace.h"
#include "metrics.h"
#include "replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#define REGION_LEN (16ULL * 1024 * 1024)
#define CHUNK_SIZE (2ULL * 1024 * 1024)   // 8 chunks
#define BUDGET_BYTES (6ULL * 1024 * 1024) // 3 chunks
#define N_PASSES 3

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
        .budget_bytes = BUDGET_BYTES,
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);

    trace_t *trace = trace_open(trace_path);
    g_r.trace = trace;
    metrics_t metrics;
    metrics_init(&metrics);
    g_r.metrics = &metrics;

    printf("region_startup OK: n_chunks=%u budget fits %llu chunks\n",
           g_r.n_chunks, (unsigned long long)(g_r.budget_bytes / g_r.chunks[0].len));

    volatile sig_atomic_t stop = 0;
    pthread_t pager_thread;
    pager_args_t pager_args = { &g_r, &stop };
    pthread_create(&pager_thread, NULL, pager_trampoline, &pager_args);

    replay_result_t res = replay_cyclic(&g_r, N_PASSES);

    stop = 1;
    pthread_join(pager_thread, NULL);
    trace_close(trace);
    g_r.trace = NULL;

    printf("replay: passes=%u touches=%u wall_ns=%llu\n",
           res.n_passes, res.n_touches, (unsigned long long)res.wall_ns);
    printf("counters: absent_handled=%llu dedup_resident=%llu dedup_fetching=%llu evictions=%llu infeasible=%llu\n",
           (unsigned long long)g_r.stat_absent_handled, (unsigned long long)g_r.stat_dedup_resident,
           (unsigned long long)g_r.stat_dedup_fetching, (unsigned long long)g_r.stat_evictions,
           (unsigned long long)g_r.stat_infeasible);

    int fail = 0;

    if (g_r.stat_dedup_resident != 0 || g_r.stat_dedup_fetching != 0) {
        fprintf(stderr, "FAIL: expected zero dedup hits in a single-threaded sequential sweep "
                "(a chunk that's already resident produces no fault at all, so dedup counters "
                "can't fire for it either), got resident=%llu fetching=%llu\n",
                (unsigned long long)g_r.stat_dedup_resident, (unsigned long long)g_r.stat_dedup_fetching);
        fail = 1;
    }
    if (g_r.stat_infeasible != 0) {
        fprintf(stderr, "FAIL: expected zero infeasible (budget always achievable by evicting), got %llu\n",
                (unsigned long long)g_r.stat_infeasible);
        fail = 1;
    }

    // Reference oracle: reproduce the exact victim-selection rule
    // ensure_budget() uses (evict lowest chunk index among resident) over
    // the same cyclic touch sequence, independently of the implementation,
    // to get the true expected fault sequence and final resident set. A
    // closed-form "every touch faults, cyclic chunk_id order" guess turns
    // out to be WRONG here: once the resident set reaches a chunk index,
    // lower-indexed chunks are always evicted first, so the two
    // highest-indexed chunks (6, 7) become permanently resident after pass
    // 1 and never fault again -- verified independently in Python before
    // fixing this test (see CLAUDE.md item 6 note).
    bool ref_resident[8] = {false};
    uint32_t ref_faults[3 * 8];
    uint32_t ref_n_faults = 0;
    const uint32_t budget_chunks = (uint32_t)(BUDGET_BYTES / CHUNK_SIZE);
    for (uint32_t pass = 0; pass < N_PASSES; pass++) {
        for (uint32_t i = 0; i < g_r.n_chunks; i++) {
            if (ref_resident[i]) continue; // already resident: no fault, no trace record
            uint32_t resident_count = 0;
            for (uint32_t j = 0; j < g_r.n_chunks; j++) if (ref_resident[j]) resident_count++;
            while (resident_count + 1 > budget_chunks) {
                for (uint32_t j = 0; j < g_r.n_chunks; j++) {
                    if (ref_resident[j]) { ref_resident[j] = false; resident_count--; break; }
                }
            }
            ref_resident[i] = true;
            ref_faults[ref_n_faults++] = i;
        }
    }
    printf("reference oracle: expected %u faults, sequence: ", ref_n_faults);
    for (uint32_t k = 0; k < ref_n_faults; k++) printf("%u ", ref_faults[k]);
    printf("\n");

    if (g_r.stat_absent_handled != ref_n_faults) {
        fprintf(stderr, "FAIL: absent_handled=%llu != reference oracle's %u\n",
                (unsigned long long)g_r.stat_absent_handled, ref_n_faults);
        fail = 1;
    }
    for (uint32_t idx = 0; idx < g_r.n_chunks; idx++) {
        chunk_state_t want = ref_resident[idx] ? CHUNK_RESIDENT : CHUNK_ABSENT;
        if (g_r.chunks[idx].state != want) {
            fprintf(stderr, "FAIL: chunk %u state=%d, reference oracle expected %s\n",
                    idx, (int)g_r.chunks[idx].state, ref_resident[idx] ? "RESIDENT" : "ABSENT");
            fail = 1;
        }
    }

    // Verify the trace independently against the same oracle: exact
    // chunk_id sequence, strictly increasing seq, all MISSING (fresh
    // evictions read back as MISSING per S3b/I-8), was_prefetched==0.
    int fd = open(trace_path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "FAIL: cannot reopen trace %s\n", trace_path); return 1; }
    trace_record_t rec;
    uint64_t n_records = 0, expect_seq = 1;
    while (read(fd, &rec, sizeof rec) == (ssize_t)sizeof rec) {
        n_records++;
        uint32_t expect_chunk_id = (n_records - 1 < ref_n_faults) ? ref_faults[n_records - 1] : UINT32_MAX;
        if (rec.seq != expect_seq) {
            fprintf(stderr, "FAIL: trace record %llu seq=%llu, expected %llu\n",
                    (unsigned long long)n_records, (unsigned long long)rec.seq, (unsigned long long)expect_seq);
            fail = 1;
        }
        if (rec.chunk_id != expect_chunk_id) {
            fprintf(stderr, "FAIL: trace record %llu chunk_id=%u, expected %u (reference oracle)\n",
                    (unsigned long long)n_records, rec.chunk_id, expect_chunk_id);
            fail = 1;
        }
        if (rec.fault_type != TRACE_FAULT_MISSING || rec.was_prefetched != 0) {
            fprintf(stderr, "FAIL: trace record %llu fault_type=%u was_prefetched=%u, expected 0,0\n",
                    (unsigned long long)n_records, rec.fault_type, rec.was_prefetched);
            fail = 1;
        }
        expect_seq++;
    }
    close(fd);
    printf("trace: %llu records read back (expected %u)\n",
           (unsigned long long)n_records, ref_n_faults);
    if (n_records != ref_n_faults) fail = 1;

    region_teardown(&g_r);
    if (fail) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
