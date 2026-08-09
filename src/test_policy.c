// test_policy.c -- build-order item 7 verification: pure unit tests of
// policy.c's select_victim/predict_next/on_fault against hand-derived
// expectations. No memfd/uffd/cgroup needed -- these functions only read
// chunk_t.state/pin/last_fault_seq and policy-private state, so this tests
// them directly rather than through a live fault path (deterministic and
// fast; the live-path integration is exercised for the default/no-policy
// case by items 4 and 6's tests already).
#define _GNU_SOURCE
#include "region.h"
#include "policy.h"

#include <stdio.h>
#include <string.h>

#define N 8
static chunk_t g_chunks[N];
static region_t g_r;

static void reset_chunks(void) {
    memset(g_chunks, 0, sizeof g_chunks);
    for (int i = 0; i < N; i++) {
        g_chunks[i].state = CHUNK_ABSENT;
        g_chunks[i].pin = 0;
        g_chunks[i].last_fault_seq = 0;
    }
    memset(&g_r, 0, sizeof g_r);
    g_r.chunks = g_chunks;
    g_r.n_chunks = N;
}

int main(void) {
    int fail = 0;

    // ---- lru --------------------------------------------------------
    {
        reset_chunks();
        policy_t *lru = policy_lru_create();
        g_r.policy = lru;

        // No residents at all -> CHUNK_NONE.
        if (lru->select_victim(&g_r) != CHUNK_NONE) {
            fprintf(stderr, "FAIL(lru): expected CHUNK_NONE with no residents\n"); fail = 1;
        }

        // Residents 1,3,5 with last_fault_seq 100,10,50 -> oldest (min seq) is 3.
        g_chunks[1].state = CHUNK_RESIDENT; g_chunks[1].last_fault_seq = 100;
        g_chunks[3].state = CHUNK_RESIDENT; g_chunks[3].last_fault_seq = 10;
        g_chunks[5].state = CHUNK_RESIDENT; g_chunks[5].last_fault_seq = 50;
        uint32_t v = lru->select_victim(&g_r);
        if (v != 3) { fprintf(stderr, "FAIL(lru): expected victim 3 (min last_fault_seq), got %u\n", v); fail = 1; }

        // Pin the oldest -> next oldest (5) must be chosen instead.
        g_chunks[3].pin = 1;
        v = lru->select_victim(&g_r);
        if (v != 5) { fprintf(stderr, "FAIL(lru): expected victim 5 (3 is pinned), got %u\n", v); fail = 1; }

        if (lru->predict_next(&g_r, &g_chunks[1]) != -1) {
            fprintf(stderr, "FAIL(lru): predict_next must always be -1 (control policy)\n"); fail = 1;
        }

        policy_destroy(lru);
        printf("lru: %s\n", fail ? "FAIL" : "ok");
    }

    // ---- layer_order --------------------------------------------------
    {
        int local_fail = 0;
        reset_chunks();
        policy_t *lo = policy_layer_order_create(N);
        g_r.policy = lo;

        // Train the chain 0->1->2->3 by calling on_fault in that order,
        // mirroring what pager.c does on each real ABSENT->FETCHING
        // transition (once per genuine fetch, in fetch order).
        lo->on_fault(&g_r, &g_chunks[0]);
        lo->on_fault(&g_r, &g_chunks[1]);
        lo->on_fault(&g_r, &g_chunks[2]);
        lo->on_fault(&g_r, &g_chunks[3]);

        if (lo->predict_next(&g_r, &g_chunks[0]) != 1) { fprintf(stderr, "FAIL(layer_order): predict_next(0) != 1\n"); local_fail = 1; }
        if (lo->predict_next(&g_r, &g_chunks[1]) != 2) { fprintf(stderr, "FAIL(layer_order): predict_next(1) != 2\n"); local_fail = 1; }
        if (lo->predict_next(&g_r, &g_chunks[2]) != 3) { fprintf(stderr, "FAIL(layer_order): predict_next(2) != 3\n"); local_fail = 1; }
        if (lo->predict_next(&g_r, &g_chunks[3]) != -1) { fprintf(stderr, "FAIL(layer_order): predict_next(3) should be -1 (never observed)\n"); local_fail = 1; }

        // Move "now" to a KNOWN position mid-chain by observing one more
        // fetch of chunk 1 (as if chunk 1 was fetched again, following
        // whatever came before). last_fetched is now 1, whose chain is
        // 1->2(dist 1)->3(dist 2, then unknown).
        lo->on_fault(&g_r, &g_chunks[1]);

        // Residents {2, 3, 5}: 2 is due next (dist 1), 3 is due after that
        // (dist 2), 5 was never observed at all (infinite distance). Belady
        // says evict the one due FURTHEST in the future -> must be 5, not
        // the lowest index (2) and not the nearest-known (3).
        g_chunks[2].state = CHUNK_RESIDENT;
        g_chunks[3].state = CHUNK_RESIDENT;
        g_chunks[5].state = CHUNK_RESIDENT;
        uint32_t v = lo->select_victim(&g_r);
        if (v != 5) {
            fprintf(stderr, "FAIL(layer_order): expected victim 5 (never observed, infinite distance), got %u\n", v);
            local_fail = 1;
        }

        // Now with only {2, 3} resident (both known distances: 2 is dist 1,
        // 3 is dist 2) -- must evict 3 (further away), keeping 2 (due
        // sooner). A lowest-index or LRU-style rule would get this wrong.
        g_chunks[5].state = CHUNK_ABSENT;
        v = lo->select_victim(&g_r);
        if (v != 3) {
            fprintf(stderr, "FAIL(layer_order): expected victim 3 (furthest known next-use, dist 2 > dist 1), got %u\n", v);
            local_fail = 1;
        }

        // No residents -> CHUNK_NONE.
        g_chunks[2].state = CHUNK_ABSENT;
        g_chunks[3].state = CHUNK_ABSENT;
        if (lo->select_victim(&g_r) != CHUNK_NONE) {
            fprintf(stderr, "FAIL(layer_order): expected CHUNK_NONE with no residents\n"); local_fail = 1;
        }

        policy_destroy(lo);
        printf("layer_order: %s\n", local_fail ? "FAIL" : "ok");
        fail |= local_fail;
    }

    if (fail) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
