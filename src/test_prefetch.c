// test_prefetch.c -- build-order item 8 verification: prefetch (§6.3).
//
// Two checks:
//  1. lru's predict_next is always -1 -> prefetch must NEVER fire under lru
//     (deterministic, exact assertion).
//  2. layer_order + prefetch, same 8-chunk/3-slot/5-pass scenario item 7's
//     smoke test used (which measured 32 real faults for layer_order WITHOUT
//     prefetch wired in -- see CLAUDE.md). With prefetch now wired into the
//     same policy, real faults must measurably decrease and stat_prefetches
//     must be nonzero, or prefetch isn't doing anything.
//  Both also serve as the regression check for the self-eviction deadlock
//  found while building this (maybe_prefetch's ensure_budget() call could
//  have the policy pick the currently-locked just-resident chunk as its own
//  victim) -- if that fix is wrong, this test hangs instead of failing
//  cleanly, which is why run_item8_tests.sh wraps it in `timeout`.
//
// Usage: test_prefetch <cgroup_path> <model_path>
#define _GNU_SOURCE
#include "region.h"
#include "pager.h"
#include "policy.h"
#include "replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>

#define REGION_LEN (16ULL * 1024 * 1024)
#define CHUNK_SIZE (2ULL * 1024 * 1024)   // 8 chunks
#define BUDGET_BYTES (6ULL * 1024 * 1024) // 3 chunks
#define N_PASSES 5
#define LAYER_ORDER_NO_PREFETCH_BASELINE 32 // from item 7's smoke test, same scenario

static region_t g_r;

typedef struct { region_t *r; volatile sig_atomic_t *stop; } pager_args_t;
static void *pager_trampoline(void *argp) {
    pager_args_t *p = (pager_args_t *)argp;
    pager_run(p->r, p->stop, 50);
    return NULL;
}

static int run_scenario(const char *cgroup_path, const char *model_path, policy_t *policy,
                         uint64_t *out_absent_handled, uint64_t *out_prefetches) {
    region_config_t cfg = {
        .region_len = REGION_LEN,
        .chunk_size = CHUNK_SIZE,
        .model_path = model_path,
        .cgroup_path = cgroup_path,
        .budget_bytes = BUDGET_BYTES,
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);
    g_r.policy = policy;
    // Enabled explicitly in both scenarios (not just the layer_order one):
    // this makes the lru case a stronger test too -- it proves
    // prefetch_enabled=true alone doesn't force prefetching without a
    // policy that actually predicts something (predict_next==-1 for lru).
    g_r.prefetch_enabled = true;

    volatile sig_atomic_t stop = 0;
    pthread_t pager_thread;
    pager_args_t pager_args = { &g_r, &stop };
    pthread_create(&pager_thread, NULL, pager_trampoline, &pager_args);

    replay_result_t res = replay_cyclic(&g_r, N_PASSES);
    (void)res;

    stop = 1;
    pthread_join(pager_thread, NULL);

    int fail = 0;
    if (g_r.resident_bytes > g_r.budget_bytes) {
        fprintf(stderr, "FAIL: resident_bytes=%llu exceeded budget_bytes=%llu\n",
                (unsigned long long)g_r.resident_bytes, (unsigned long long)g_r.budget_bytes);
        fail = 1;
    }
    *out_absent_handled = g_r.stat_absent_handled;
    *out_prefetches = g_r.stat_prefetches;

    region_teardown(&g_r);
    return fail;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <cgroup_path> <model_path>\n", argv[0]);
        return 2;
    }
    int fail = 0;

    // --- lru: prefetch must never fire ---
    {
        policy_t *lru = policy_lru_create();
        uint64_t absent = 0, prefetches = 0;
        fail |= run_scenario(argv[1], argv[2], lru, &absent, &prefetches);
        printf("lru: absent_handled=%llu prefetches=%llu\n",
               (unsigned long long)absent, (unsigned long long)prefetches);
        if (prefetches != 0) {
            fprintf(stderr, "FAIL: lru's predict_next is always -1, prefetches must be 0, got %llu\n",
                    (unsigned long long)prefetches);
            fail = 1;
        }
        policy_destroy(lru);
    }

    // --- layer_order + prefetch: must measurably beat the no-prefetch baseline ---
    {
        policy_t *lo = policy_layer_order_create(REGION_LEN / CHUNK_SIZE);
        uint64_t absent = 0, prefetches = 0;
        fail |= run_scenario(argv[1], argv[2], lo, &absent, &prefetches);
        printf("layer_order+prefetch: absent_handled=%llu prefetches=%llu (no-prefetch baseline was %d)\n",
               (unsigned long long)absent, (unsigned long long)prefetches, LAYER_ORDER_NO_PREFETCH_BASELINE);
        if (prefetches == 0) {
            fprintf(stderr, "FAIL: expected at least one successful prefetch across %d passes\n", N_PASSES);
            fail = 1;
        }
        if (absent >= LAYER_ORDER_NO_PREFETCH_BASELINE) {
            fprintf(stderr, "FAIL: expected real faults (%llu) to be measurably below the "
                    "no-prefetch baseline (%d) -- prefetch should be converting some future "
                    "faults into no-ops\n", (unsigned long long)absent, LAYER_ORDER_NO_PREFETCH_BASELINE);
            fail = 1;
        }
        policy_destroy(lo);
    }

    if (fail) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
