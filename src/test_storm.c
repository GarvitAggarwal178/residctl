// test_storm.c -- MECHANISM_SPEC.md §13 T-3: concurrent fault storm.
// 8 threads randomly touching the region under a tight budget for 60s.
// Pass criteria: no hangs, no corruption, resident_bytes never exceeds
// budget, reconcile() never trips. The last two are enforced by abort() at
// the point of violation (I-4/I-7) -- absence of abort across the full 60s
// run is the evidence they held, the same enforcement pattern used
// throughout this project. Run under `timeout` by the caller so a genuine
// hang fails loud instead of blocking forever.
//
// Usage: test_storm <cgroup_path> <model_path> <duration_seconds>
#define _GNU_SOURCE
#include "region.h"
#include "pager.h"
#include "policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#define REGION_LEN (16ULL * 1024 * 1024)
#define CHUNK_SIZE (2ULL * 1024 * 1024)   // 8 chunks
#define BUDGET_BYTES (4ULL * 1024 * 1024) // 2 chunks -- tight, forces constant eviction
#define N_THREADS 8

static region_t g_r;
static volatile sig_atomic_t g_stop_workers = 0;

static uint8_t expected_byte(uint64_t file_off) {
    return (uint8_t)(file_off ^ (file_off >> 8) ^ (file_off >> 16));
}

typedef struct { region_t *r; volatile sig_atomic_t *stop; } pager_args_t;
static void *pager_trampoline(void *argp) {
    pager_args_t *p = (pager_args_t *)argp;
    pager_run(p->r, p->stop, 50);
    return NULL;
}

typedef struct {
    unsigned seed;
    uint64_t touches;
    uint64_t mismatches;
} worker_result_t;

static void *storm_thread(void *argp) {
    worker_result_t *res = (worker_result_t *)argp;
    while (!g_stop_workers) {
        uint32_t idx = rand_r(&res->seed) % g_r.n_chunks;
        chunk_t *c = &g_r.chunks[idx];
        uint64_t off = rand_r(&res->seed) % c->len;
        uint8_t got = g_r.map_a[c->region_off + off]; // may block on a real fault
        uint8_t want = expected_byte(c->file_off + off);
        res->touches++;
        if (got != want) res->mismatches++;
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <cgroup_path> <model_path> <duration_seconds>\n", argv[0]);
        return 2;
    }
    int duration_s = atoi(argv[3]);

    region_config_t cfg = {
        .region_len = REGION_LEN, .chunk_size = CHUNK_SIZE,
        .model_path = argv[2], .cgroup_path = argv[1],
        .budget_bytes = BUDGET_BYTES,
        .reconcile_interval = 1, // A-3: §13 correctness runs use eager reconcile
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);
    policy_t *policy = policy_layer_order_learned_create(g_r.n_chunks);
    g_r.policy = policy;
    g_r.prefetch_enabled = true; // exercise the prefetch path under contention too

    volatile sig_atomic_t pager_stop = 0;
    pthread_t pager_thread;
    pager_args_t pa = { &g_r, &pager_stop };
    pthread_create(&pager_thread, NULL, pager_trampoline, &pa);

    pthread_t threads[N_THREADS];
    worker_result_t results[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        results[i].seed = (unsigned)(i * 2654435761u + 999);
        results[i].touches = 0;
        results[i].mismatches = 0;
        pthread_create(&threads[i], NULL, storm_thread, &results[i]);
    }

    printf("storm running for %ds with %d threads, budget=%llu (2 chunks of %llu)...\n",
           duration_s, N_THREADS, (unsigned long long)BUDGET_BYTES, (unsigned long long)CHUNK_SIZE);
    struct timespec ts = { .tv_sec = duration_s, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
    g_stop_workers = 1;

    uint64_t total_touches = 0, total_mismatches = 0;
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(threads[i], NULL);
        total_touches += results[i].touches;
        total_mismatches += results[i].mismatches;
    }

    pager_stop = 1;
    pthread_join(pager_thread, NULL);

    printf("storm done: total_touches=%llu mismatches=%llu resident_bytes=%llu budget_bytes=%llu "
           "evictions=%llu absent_handled=%llu dedup_resident=%llu dedup_fetching=%llu "
           "prefetches=%llu infeasible=%llu\n",
           (unsigned long long)total_touches, (unsigned long long)total_mismatches,
           (unsigned long long)g_r.resident_bytes, (unsigned long long)g_r.budget_bytes,
           (unsigned long long)g_r.stat_evictions, (unsigned long long)g_r.stat_absent_handled,
           (unsigned long long)g_r.stat_dedup_resident, (unsigned long long)g_r.stat_dedup_fetching,
           (unsigned long long)g_r.stat_prefetches, (unsigned long long)g_r.stat_infeasible);

    int fail = 0;
    if (total_mismatches != 0) { fprintf(stderr, "FAIL: %llu data mismatches\n", (unsigned long long)total_mismatches); fail = 1; }
    if (g_r.resident_bytes > g_r.budget_bytes) { fprintf(stderr, "FAIL: resident_bytes exceeded budget\n"); fail = 1; }
    if (total_touches == 0) { fprintf(stderr, "FAIL: no touches recorded, storm didn't run\n"); fail = 1; }

    region_teardown(&g_r);
    policy_destroy(policy);
    if (fail) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
