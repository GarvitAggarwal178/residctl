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

    // ---- layer_order_learned ----------------------------------------
    {
        int local_fail = 0;
        reset_chunks();
        policy_t *lo = policy_layer_order_learned_create(N);
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
        printf("layer_order_learned: %s\n", local_fail ? "FAIL" : "ok");
        fail |= local_fail;
    }

    // ---- layer_order_declared (WP1 / A-12) --------------------------
    {
        int local_fail = 0;
        reset_chunks();
        policy_t *lo = policy_layer_order_declared_create(N);
        g_r.policy = lo;
        // FINAL SESSION Phase 2 flipped the operational default to protect-off;
        // this block validates the heuristic's correctness WHEN ENABLED, so
        // pin it on explicitly (self-documenting, default-independent).
        policy_set_protect_current(1);
        // LIVELOCK FIX Defect 1: this block asserts post-consumption semantics
        // (d starts at 1, seq[pos] furthest). Pin the mode explicitly.
        policy_set_signal_mode(0);

        // Declare a non-identity permutation so the test can't pass by
        // accident on chunk_id==position.
        const uint32_t decl[N] = { 0, 2, 4, 6, 1, 3, 5, 7 };
        policy_declare_sequence(&g_r, decl, N);

        // Before any on_access(): predict_next is keyed on the chunk's slot
        // in the declared pass -- successor of 0 is 2, of 6 is 1, of 7 is 0
        // (cyclic wrap).
        if (lo->predict_next(&g_r, &g_chunks[0]) != 2) { fprintf(stderr, "FAIL(declared): predict_next(0) != 2\n"); local_fail = 1; }
        if (lo->predict_next(&g_r, &g_chunks[6]) != 1) { fprintf(stderr, "FAIL(declared): predict_next(6) != 1\n"); local_fail = 1; }
        if (lo->predict_next(&g_r, &g_chunks[7]) != 0) { fprintf(stderr, "FAIL(declared): predict_next(7) != 0 (cyclic wrap)\n"); local_fail = 1; }

        // Consume the first two references (chunks 0 then 2). Position is
        // now at declared index 1 (chunk 2). Distances from here: chunk 4 is
        // 1 away, 6 is 2, 1 is 3, 3 is 4, 5 is 5, 7 is 6. WP0 fix: chunk 2
        // (current, seq[pos]) and chunk 0 (seq[pos-1]) return 0 -- protected.
        lo->on_access(&g_r, &g_chunks[0]);
        lo->on_access(&g_r, &g_chunks[2]);
        if (lo->next_use_distance(&g_r, &g_chunks[4]) != 1) { fprintf(stderr, "FAIL(declared): dist(4) != 1\n"); local_fail = 1; }
        if (lo->next_use_distance(&g_r, &g_chunks[1]) != 3) { fprintf(stderr, "FAIL(declared): dist(1) != 3\n"); local_fail = 1; }
        if (lo->next_use_distance(&g_r, &g_chunks[2]) != 0) { fprintf(stderr, "FAIL(declared): dist(2) != 0 (current chunk, protected)\n"); local_fail = 1; }
        if (lo->next_use_distance(&g_r, &g_chunks[0]) != 0) { fprintf(stderr, "FAIL(declared): dist(0) != 0 (seq[pos-1], protected)\n"); local_fail = 1; }
        if (lo->trace_cursor(&g_r) != 2) { fprintf(stderr, "FAIL(declared): trace_cursor != 2\n"); local_fail = 1; }

        // select_victim among residents {6, 1, 7}: distances 2, 3, 6 -> the
        // furthest (7) must be chosen, not lowest index (1) or nearest (6).
        g_chunks[6].state = CHUNK_RESIDENT;
        g_chunks[1].state = CHUNK_RESIDENT;
        g_chunks[7].state = CHUNK_RESIDENT;
        uint32_t v = lo->select_victim(&g_r);
        if (v != 7) { fprintf(stderr, "FAIL(declared): expected victim 7 (furthest declared next-use), got %u\n", v); local_fail = 1; }

        // Pin 7 -> next furthest (1, dist 3) wins.
        g_chunks[7].pin = 1;
        v = lo->select_victim(&g_r);
        if (v != 1) { fprintf(stderr, "FAIL(declared): expected victim 1 (7 pinned), got %u\n", v); local_fail = 1; }

        // No residents -> CHUNK_NONE.
        g_chunks[6].state = CHUNK_ABSENT; g_chunks[1].state = CHUNK_ABSENT;
        g_chunks[7].state = CHUNK_ABSENT; g_chunks[7].pin = 0;
        if (lo->select_victim(&g_r) != CHUNK_NONE) { fprintf(stderr, "FAIL(declared): expected CHUNK_NONE with no residents\n"); local_fail = 1; }

        // ---- Belady cross-check (§1.1): the declared policy's next-use
        // distance must agree, at every position and for every chunk, with
        // an independent naive forward scan over the unrolled cyclic
        // reference string. Written separately here -- belady.c's solver is
        // NOT refactored -- exactly as the spec requires.
        {
            const uint32_t W = 6;                 // distinct chunks
            const uint32_t cyc[6] = { 3, 1, 4, 0, 5, 2 };
            reset_chunks();
            policy_t *d2 = policy_layer_order_declared_create(W);
            g_r.policy = d2;
            policy_set_protect_current(1);   // this cross-check assumes protect-on semantics
            policy_set_signal_mode(0);        // post-consumption (d starts at 1)
            policy_declare_sequence(&g_r, cyc, W);

            int xchk_fail = 0;
            for (uint32_t step = 0; step < W; step++) {
                d2->on_access(&g_r, &g_chunks[cyc[step]]); // position now at step
                for (uint32_t x = 0; x < W; x++) {
                    int64_t got = d2->next_use_distance(&g_r, &g_chunks[x]);
                    // naive reference: WP0 fix -> the actively-consumed chunk
                    // (cyc[step]) and the one before it (cyc[step-1]) are
                    // protected (0); otherwise the smallest k>=1 with
                    // cyc[(step+k) % W] == x.
                    int64_t want;
                    if (x == cyc[step] || (step > 0 && x == cyc[step - 1])) {
                        want = 0;
                    } else {
                        want = -1;
                        for (uint32_t k = 1; k <= W; k++)
                            if (cyc[(step + k) % W] == x) { want = (int64_t)k; break; }
                    }
                    if (got != want) {
                        fprintf(stderr, "FAIL(declared xcheck): step=%u chunk=%u got=%lld want=%lld\n",
                                step, x, (long long)got, (long long)want);
                        xchk_fail = 1;
                    }
                }
            }
            printf("declared next-use vs naive Belady scan (post, protect-on): %s\n", xchk_fail ? "DISAGREE" : "agree (all positions, all chunks)");
            if (xchk_fail) local_fail = 1;
            policy_destroy(d2);

            // LIVELOCK FIX Defect 1: the SAME cross-check in PRE-consumption
            // mode with protect OFF -- the mode the real model uses after
            // Defect 2. Naive reference: smallest k >= 0 with cyc[(step+k)%W]
            // == x (so seq[pos] itself -> 0). The existing post-mode check
            // above was insensitive to the d0 change: protect-on short-circuits
            // seq[pos] and seq[pos-1] to 0, and those are the only inputs where
            // starting at 0 vs 1 differs.
            {
                reset_chunks();
                policy_t *d3 = policy_layer_order_declared_create(W);
                g_r.policy = d3;
                policy_set_protect_current(0);
                policy_set_signal_mode(1);   // pre-consumption (d starts at 0)
                policy_declare_sequence(&g_r, cyc, W);
                int xf = 0;
                for (uint32_t step = 0; step < W; step++) {
                    d3->on_access(&g_r, &g_chunks[cyc[step]]);
                    for (uint32_t x = 0; x < W; x++) {
                        int64_t got = d3->next_use_distance(&g_r, &g_chunks[x]);
                        int64_t want = -1;
                        for (uint32_t k = 0; k < W; k++)
                            if (cyc[(step + k) % W] == x) { want = (int64_t)k; break; }
                        if (got != want) {
                            fprintf(stderr, "FAIL(declared xcheck pre): step=%u chunk=%u got=%lld want=%lld\n",
                                    step, x, (long long)got, (long long)want);
                            xf = 1;
                        }
                    }
                }
                printf("declared next-use vs naive Belady scan (pre, protect-off): %s\n", xf ? "DISAGREE" : "agree (all positions, all chunks)");
                if (xf) local_fail = 1;
                policy_destroy(d3);
                policy_set_protect_current(1);
                policy_set_signal_mode(0);
            }
        }

        policy_destroy(lo);

        // ---- protect-current OFF, POST-consumption mode ----
        // Heuristic off + d starts at 1: the actively-consumed chunk seq[pos]
        // is seq_len away (furthest -> top victim), seq[pos-1] is seq_len-1.
        // This is the synthetic --consumption-signal all-threads path and is
        // byte-for-byte the pre-Defect-1 behaviour.
        {
            reset_chunks();
            policy_set_protect_current(0);
            policy_set_signal_mode(0);   // post-consumption
            policy_t *lo3 = policy_layer_order_declared_create(N);
            g_r.policy = lo3;
            const uint32_t decl3[N] = { 0, 2, 4, 6, 1, 3, 5, 7 };
            policy_declare_sequence(&g_r, decl3, N);
            lo3->on_access(&g_r, &g_chunks[0]);
            lo3->on_access(&g_r, &g_chunks[2]);   // position at declared index 1 (chunk 2)
            if (lo3->next_use_distance(&g_r, &g_chunks[2]) != (int64_t)N) {
                fprintf(stderr, "FAIL(declared post protect-off): dist(2) != %d (should be furthest)\n", N); local_fail = 1;
            }
            if (lo3->next_use_distance(&g_r, &g_chunks[0]) != (int64_t)(N - 1)) {
                fprintf(stderr, "FAIL(declared post protect-off): dist(0) != %d\n", N - 1); local_fail = 1;
            }
            if (lo3->next_use_distance(&g_r, &g_chunks[4]) != 1) {
                fprintf(stderr, "FAIL(declared post protect-off): dist(4) != 1\n"); local_fail = 1;
            }
            policy_destroy(lo3);
            policy_set_protect_current(1);
            printf("layer_order_declared post protect-off: %s\n", local_fail ? "FAIL" : "ok");
        }

        // ---- LIVELOCK FIX Defect 1: protect-current OFF, PRE-consumption ----
        // Heuristic off + d starts at 0: seq[pos] is imminent (dist 0),
        // seq[pos+1] is 1, seq[pos-1] is seq_len-1, a chunk absent from the
        // declared sequence is INT64_MAX. This is the real-model path after
        // Defect 2 moves the notify onto the pre-compute callback pass.
        {
            reset_chunks();
            policy_set_protect_current(0);
            policy_set_signal_mode(1);   // pre-consumption
            policy_t *lo4 = policy_layer_order_declared_create(N);
            g_r.policy = lo4;
            // 7 distinct chunks declared; chunk 5 is deliberately absent.
            const uint32_t decl4[7] = { 0, 2, 4, 6, 1, 3, 7 };
            policy_declare_sequence(&g_r, decl4, 7);
            lo4->on_access(&g_r, &g_chunks[0]);
            lo4->on_access(&g_r, &g_chunks[2]);   // pos = declared index 1 (chunk 2)
            if (lo4->next_use_distance(&g_r, &g_chunks[2]) != 0) {
                fprintf(stderr, "FAIL(declared pre protect-off): dist(seq[pos]=2) != 0\n"); local_fail = 1;
            }
            if (lo4->next_use_distance(&g_r, &g_chunks[4]) != 1) {
                fprintf(stderr, "FAIL(declared pre protect-off): dist(seq[pos+1]=4) != 1\n"); local_fail = 1;
            }
            if (lo4->next_use_distance(&g_r, &g_chunks[0]) != 6) {
                fprintf(stderr, "FAIL(declared pre protect-off): dist(seq[pos-1]=0) != seq_len-1 (6)\n"); local_fail = 1;
            }
            if (lo4->next_use_distance(&g_r, &g_chunks[5]) != (int64_t)INT64_MAX) {
                fprintf(stderr, "FAIL(declared pre protect-off): dist(5, absent from seq) != INT64_MAX\n"); local_fail = 1;
            }
            // select_victim: residents {4,0}. dist(4)=1, dist(0)=6 -> evict 0.
            g_chunks[4].state = CHUNK_RESIDENT;
            g_chunks[0].state = CHUNK_RESIDENT;
            if (lo4->select_victim(&g_r) != 0) {
                fprintf(stderr, "FAIL(declared pre protect-off): select_victim != 0 (furthest)\n"); local_fail = 1;
            }
            policy_destroy(lo4);
            printf("layer_order_declared pre protect-off: %s\n", local_fail ? "FAIL" : "ok");
        }

        // ---- LIVELOCK FIX Defect 1: protect-current ON must still work in
        // BOTH modes -- seq[pos] and seq[pos-1] forced to 0 regardless.
        {
            reset_chunks();
            const uint32_t decl5[7] = { 0, 2, 4, 6, 1, 3, 7 };
            for (int mode = 0; mode <= 1; mode++) {
                reset_chunks();
                policy_set_protect_current(1);
                policy_set_signal_mode(mode);
                policy_t *lo5 = policy_layer_order_declared_create(7);
                g_r.policy = lo5;
                policy_declare_sequence(&g_r, decl5, 7);
                lo5->on_access(&g_r, &g_chunks[0]);
                lo5->on_access(&g_r, &g_chunks[2]);
                if (lo5->next_use_distance(&g_r, &g_chunks[2]) != 0 ||
                    lo5->next_use_distance(&g_r, &g_chunks[0]) != 0) {
                    fprintf(stderr, "FAIL(declared protect-on mode=%d): seq[pos]/seq[pos-1] not both 0\n", mode);
                    local_fail = 1;
                }
                if (lo5->next_use_distance(&g_r, &g_chunks[5]) != (int64_t)INT64_MAX) {
                    fprintf(stderr, "FAIL(declared protect-on mode=%d): dist(absent) != INT64_MAX\n", mode);
                    local_fail = 1;
                }
                policy_destroy(lo5);
            }
            printf("layer_order_declared protect-on both modes: %s\n", local_fail ? "FAIL" : "ok");
        }

        policy_set_signal_mode(0);   // restore default for any later test
        printf("layer_order_declared: %s\n", local_fail ? "FAIL" : "ok");
        fail |= local_fail;
    }

    if (fail) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
