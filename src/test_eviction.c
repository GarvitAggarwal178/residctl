// test_eviction.c -- build-order item 4 verification: reconcile(),
// evict_chunk(), ensure_budget() (§7). Deterministic by construction: the
// temporary victim selector (lowest layer_id among RESIDENT+unpinned) plus
// strictly sequential touches makes the resident set exactly predictable,
// so this checks EXACT membership, not just "didn't crash."
//
// Usage: test_eviction <cgroup_path> <model_path>
#define _GNU_SOURCE
#include "region.h"
#include "pager.h"
#include "budget.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#define REGION_LEN (16ULL * 1024 * 1024)
#define CHUNK_SIZE (2ULL * 1024 * 1024)   // 8 chunks
#define BUDGET_BYTES (6ULL * 1024 * 1024) // fits exactly 3 chunks resident

static region_t g_r;

static uint8_t expected_byte(uint64_t file_off) {
    return (uint8_t)(file_off ^ (file_off >> 8) ^ (file_off >> 16));
}

typedef struct { region_t *r; volatile sig_atomic_t *stop; } pager_args_t;
static void *pager_trampoline(void *argp) {
    pager_args_t *p = (pager_args_t *)argp;
    pager_run(p->r, p->stop, 50);
    return NULL;
}

static void touch(uint32_t idx) {
    chunk_t *c = &g_r.chunks[idx];
    volatile uint8_t x = g_r.map_a[c->region_off];
    (void)x;
    usleep(50 * 1000); // let the (single-threaded) pager fully drain this fault
}

static int verify_chunk_data(uint32_t idx) {
    chunk_t *c = &g_r.chunks[idx];
    for (uint64_t off = 0; off < c->len; off += 4096) {
        uint8_t got = g_r.map_a[c->region_off + off];
        uint8_t want = expected_byte(c->file_off + off);
        if (got != want) {
            fprintf(stderr, "MISMATCH chunk=%u off=%llu got=%u want=%u\n",
                    idx, (unsigned long long)off, got, want);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <cgroup_path> <model_path>\n", argv[0]);
        return 2;
    }

    region_config_t cfg = {
        .region_len = REGION_LEN,
        .chunk_size = CHUNK_SIZE,
        .model_path = argv[2],
        .cgroup_path = argv[1],
        .budget_bytes = BUDGET_BYTES,
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);
    printf("region_startup OK: n_chunks=%u budget_bytes=%llu (fits %llu chunks)\n",
           g_r.n_chunks, (unsigned long long)g_r.budget_bytes,
           (unsigned long long)(g_r.budget_bytes / g_r.chunks[0].len));

    volatile sig_atomic_t stop = 0;
    pthread_t pager_thread;
    pager_args_t pager_args = { &g_r, &stop };
    pthread_create(&pager_thread, NULL, pager_trampoline, &pager_args);

    int fail = 0;

    // Sequential touch of all 8 chunks. With a 3-chunk budget and the
    // temporary lowest-layer_id victim selector, the resident set after
    // touching chunk i (i>=2) should be exactly {i-2, i-1, i}.
    for (uint32_t i = 0; i < g_r.n_chunks; i++) {
        touch(i);
        uint32_t resident_count = 0;
        for (uint32_t j = 0; j < g_r.n_chunks; j++)
            if (g_r.chunks[j].state == CHUNK_RESIDENT) resident_count++;
        uint32_t expect_count = (i + 1 < 3) ? (i + 1) : 3;
        printf("after touch(%u): resident_count=%u (expect %u) resident_bytes=%llu evictions=%llu\n",
               i, resident_count, expect_count, (unsigned long long)g_r.resident_bytes,
               (unsigned long long)g_r.stat_evictions);
        if (resident_count != expect_count) {
            fprintf(stderr, "FAIL: after touch(%u) resident_count=%u != expected %u\n",
                    i, resident_count, expect_count);
            fail = 1;
        }
        if (g_r.resident_bytes > g_r.budget_bytes) {
            fprintf(stderr, "FAIL: resident_bytes=%llu exceeds budget_bytes=%llu (I-4/§7 violated)\n",
                    (unsigned long long)g_r.resident_bytes, (unsigned long long)g_r.budget_bytes);
            fail = 1;
        }
    }

    // Expect exactly chunks {5,6,7} resident, {0..4} evicted, 5 evictions total.
    uint32_t expect_resident[] = {5, 6, 7};
    for (uint32_t k = 0; k < 3; k++) {
        uint32_t idx = expect_resident[k];
        if (g_r.chunks[idx].state != CHUNK_RESIDENT) {
            fprintf(stderr, "FAIL: expected chunk %u RESIDENT, state=%d\n", idx, (int)g_r.chunks[idx].state);
            fail = 1;
        }
        if (!verify_chunk_data(idx)) fail = 1;
    }
    for (uint32_t idx = 0; idx < 5; idx++) {
        if (g_r.chunks[idx].state != CHUNK_ABSENT) {
            fprintf(stderr, "FAIL: expected chunk %u ABSENT (evicted), state=%d\n", idx, (int)g_r.chunks[idx].state);
            fail = 1;
        }
    }
    if (g_r.stat_evictions != 5) {
        fprintf(stderr, "FAIL: expected exactly 5 evictions, got %llu\n", (unsigned long long)g_r.stat_evictions);
        fail = 1;
    }
    if (g_r.stat_bytes_punched != 5 * CHUNK_SIZE) {
        fprintf(stderr, "FAIL: expected bytes_punched=%llu, got %llu\n",
                (unsigned long long)(5 * CHUNK_SIZE), (unsigned long long)g_r.stat_bytes_punched);
        fail = 1;
    }
    printf("after full pass: evictions=%llu bytes_punched=%llu infeasible=%llu\n",
           (unsigned long long)g_r.stat_evictions, (unsigned long long)g_r.stat_bytes_punched,
           (unsigned long long)g_r.stat_infeasible);

    // Punch-refetch round trip (preview of §13 T-2): re-touch an evicted
    // chunk (0) and verify it comes back with correct data, and that this
    // in turn evicted the new lowest-layer_id resident chunk (5).
    touch(0);
    if (g_r.chunks[0].state != CHUNK_RESIDENT) {
        fprintf(stderr, "FAIL: chunk 0 not RESIDENT after refetch\n");
        fail = 1;
    }
    if (!verify_chunk_data(0)) fail = 1;
    if (g_r.chunks[5].state != CHUNK_ABSENT) {
        fprintf(stderr, "FAIL: expected chunk 5 evicted to make room for refetched chunk 0, state=%d\n",
                (int)g_r.chunks[5].state);
        fail = 1;
    }
    if (g_r.stat_evictions != 6) {
        fprintf(stderr, "FAIL: expected 6 evictions after refetch, got %llu\n", (unsigned long long)g_r.stat_evictions);
        fail = 1;
    }
    printf("punch-refetch round trip on chunk 0: state=%d evictions=%llu\n",
           (int)g_r.chunks[0].state, (unsigned long long)g_r.stat_evictions);

    stop = 1;
    pthread_join(pager_thread, NULL);
    region_teardown(&g_r);

    if (fail) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
