// test_t6.c -- MECHANISM_SPEC.md §13 T-6 (item 10c, new): the dedup
// branches in handle_fault() must actually fire under the async handler.
//
// Item 10b Task C proved dedup_fetching was structurally unreachable under
// the old synchronous handler (the handler drained uffd messages strictly
// sequentially, one fully processed -- including its blocking pread --
// before the next was read, so no execution context could ever observe a
// chunk mid-FETCHING from handle_fault()). Item 10c's async dispatch-only
// handler (A-5) is the fix. This test is the direct, explicit check that
// the fix actually landed: with dispatch decoupled from fetch, many threads
// faulting into a chunk whose fetch is in flight is now the COMMON case,
// not a rare race window.
//
// Same load shape as T-3 (8 threads, tight 2-chunk budget, 60s,
// layer_order + prefetch) -- deliberately, since T-3 is the load this
// architecture change was justified against -- but this test's PASS
// criterion is specifically stat_dedup_fetching > 0, which T-3 itself
// (unmodified, per instructions) does not gate on. Explicit instruction:
// if this is still zero, the decoupling did not actually happen and Task A
// is not done -- do not proceed to Task B.
//
// Usage: test_t6 <cgroup_path> <model_path> <duration_seconds>
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
        // sync_handler left false (default): this test exists specifically
        // to confirm the ASYNC path (item 10c's default).
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);
    policy_t *policy = policy_layer_order_create(g_r.n_chunks);
    g_r.policy = policy;
    g_r.prefetch_enabled = true; // exercise the prefetch path under contention too

    printf("T-6: async_handler=%d fetch_workers=%u\n", g_r.async_handler, g_r.fetch_workers);
    if (!g_r.async_handler) {
        fprintf(stderr, "FAIL: expected async_handler=true by default; region_startup's default changed?\n");
        return 1;
    }

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

    printf("T-6 done: total_touches=%llu mismatches=%llu resident_bytes=%llu budget_bytes=%llu "
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
    if (g_r.stat_dedup_fetching == 0) {
        fprintf(stderr,
                "FAIL (T-6 gate): stat_dedup_fetching == 0. With an async handler, many threads "
                "faulting into a chunk whose fetch is in flight is supposed to be the COMMON case. "
                "If it is still zero, the dispatch/fetch decoupling did not actually happen -- do "
                "not proceed to Task B.\n");
        fail = 1;
    }

    uint64_t final_dedup_fetching = g_r.stat_dedup_fetching; // region_teardown() zeroes g_r; capture first
    region_teardown(&g_r);
    policy_destroy(policy);
    if (fail) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("PASS (stat_dedup_fetching=%llu > 0)\n", (unsigned long long)final_dedup_fetching);
    return 0;
}
