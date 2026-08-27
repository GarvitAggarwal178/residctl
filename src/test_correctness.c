// test_correctness.c -- MECHANISM_SPEC.md §13 T-1, T-2, T-4.
// T-3 (60s concurrent storm) is test_storm.c, run separately and time-boxed.
// T-5 (Belady sanity) is belady_main --selftest (item 9).
//
// Usage: test_correctness <cgroup_path> <model_path>
#define _GNU_SOURCE
#include "region.h"
#include "pager.h"
#include "policy.h"
#include "budget.h"
#include "cgroup_stat.h"

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#define REGION_LEN (16ULL * 1024 * 1024)
#define CHUNK_SIZE (2ULL * 1024 * 1024)     // 8 chunks
#define BUDGET_25PCT (4ULL * 1024 * 1024)   // 25% of region, 2 chunks

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

// T-1: fill model with position-derived pattern (gen_pattern.c already did
// this for pattern_16m.bin), read the ENTIRE region through mapping A under
// a 25%-of-region budget, verify every byte, under each policy in turn.
static int run_t1(const char *cgroup_path, const char *model_path) {
    printf("=== T-1: full-region data integrity under 25%% budget, all policies ===\n");
    int fail = 0;
    // WP1 (A-12): layer_order -> layer_order_learned; layer_order_declared added.
    const char *policy_names[] = { "default", "lru", "layer_order_learned", "layer_order_declared" };

    for (int pi = 0; pi < 4; pi++) {
        region_config_t cfg = {
            .region_len = REGION_LEN, .chunk_size = CHUNK_SIZE,
            .model_path = model_path, .cgroup_path = cgroup_path,
            .budget_bytes = BUDGET_25PCT,
            .reconcile_interval = 1, // A-3: §13 correctness runs use eager reconcile
        };
        run_manifest_t m;
        region_startup(&g_r, &cfg, &m);

        policy_t *policy = NULL;
        if (pi == 1) policy = policy_lru_create();
        else if (pi == 2) policy = policy_layer_order_learned_create(g_r.n_chunks);
        else if (pi == 3) policy = policy_layer_order_declared_create(g_r.n_chunks);
        g_r.policy = policy;
        if (pi == 3) {
            // WP1 (A-12): the declared policy needs its sequence up front.
            // T-1's scan touches chunks 0,1,2,... in order.
            uint32_t *seq = malloc(g_r.n_chunks * sizeof(uint32_t));
            for (uint32_t i = 0; i < g_r.n_chunks; i++) seq[i] = i;
            policy_declare_sequence(&g_r, seq, g_r.n_chunks);
            free(seq);
        }

        volatile sig_atomic_t stop = 0;
        pthread_t pager_thread;
        pager_args_t pa = { &g_r, &stop };
        pthread_create(&pager_thread, NULL, pager_trampoline, &pa);

        uint64_t mismatches = 0, bytes_checked = 0;
        for (uint64_t off = 0; off < REGION_LEN; off += 4096) {
            // WP1 (A-12): fire the workload consumption signal at each chunk
            // boundary so the declared policy's position actually advances
            // (a no-op for the other three policies).
            if (off % CHUNK_SIZE == 0)
                pager_notify_access(&g_r, &g_r.chunks[off / CHUNK_SIZE]);
            uint8_t got = g_r.map_a[off];
            uint8_t want = expected_byte(off);
            bytes_checked += 4096;
            if (got != want) mismatches++;
        }

        stop = 1;
        pthread_join(pager_thread, NULL);

        printf("  policy=%-11s bytes_checked=%llu mismatches=%llu resident_bytes=%llu budget=%llu evictions=%llu -- %s\n",
               policy_names[pi], (unsigned long long)bytes_checked, (unsigned long long)mismatches,
               (unsigned long long)g_r.resident_bytes, (unsigned long long)g_r.budget_bytes,
               (unsigned long long)g_r.stat_evictions, mismatches == 0 ? "OK" : "FAIL");
        if (mismatches != 0) fail = 1;
        if (g_r.resident_bytes > g_r.budget_bytes) {
            printf("  FAIL: resident_bytes exceeded budget under policy=%s\n", policy_names[pi]);
            fail = 1;
        }

        region_teardown(&g_r);
        policy_destroy(policy);
    }
    return fail;
}

// T-2: fetch a chunk, evict it, refetch, verify identical. 1000 iterations.
// Two chunks, budget for exactly one -> every touch of the "other" chunk
// forces an eviction, giving a genuine punch-refetch cycle every step.
static int run_t2(const char *cgroup_path, const char *model_path) {
    printf("=== T-2: punch-refetch cycle x1000 ===\n");
    region_config_t cfg = {
        .region_len = 4ULL * 1024 * 1024, .chunk_size = 2ULL * 1024 * 1024, // 2 chunks
        .model_path = model_path, .cgroup_path = cgroup_path,
        .budget_bytes = 2ULL * 1024 * 1024, // 1 chunk
        .reconcile_interval = 1, // A-3
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);

    volatile sig_atomic_t stop = 0;
    pthread_t pager_thread;
    pager_args_t pa = { &g_r, &stop };
    pthread_create(&pager_thread, NULL, pager_trampoline, &pa);

    int fail = 0;
    uint64_t mismatches = 0;
    for (int iter = 0; iter < 1000; iter++) {
        chunk_t *c = &g_r.chunks[iter % 2];
        volatile uint8_t x = g_r.map_a[c->region_off]; // triggers fetch if ABSENT
        (void)x;
        for (uint64_t off = 0; off < c->len; off += 65536) { // sample, not every byte, x1000 iters
            uint8_t got = g_r.map_a[c->region_off + off];
            uint8_t want = expected_byte(c->file_off + off);
            if (got != want) mismatches++;
        }
    }
    stop = 1;
    pthread_join(pager_thread, NULL);

    printf("  1000 iterations, mismatches=%llu, evictions=%llu -- %s\n",
           (unsigned long long)mismatches, (unsigned long long)g_r.stat_evictions,
           mismatches == 0 ? "OK" : "FAIL");
    if (mismatches != 0) fail = 1;
    if (g_r.stat_evictions < 999) {
        printf("  FAIL: expected ~999 evictions (ping-pong every iteration), got %llu\n",
               (unsigned long long)g_r.stat_evictions);
        fail = 1;
    }

    region_teardown(&g_r);
    return fail;
}

// T-4: after an arbitrary sequence of fetches/evictions, resident_bytes
// must match memory.stat[shmem] minus known overhead EXACTLY -- a tighter
// check than reconcile()'s own one-chunk operational threshold (I-7).
static int run_t4_one(const char *cgroup_path, const char *model_path, int declared) {
    printf("=== T-4: exact accounting after mixed fetch/evict sequence (policy=%s) ===\n",
           declared ? "layer_order_declared" : "layer_order_learned");
    region_config_t cfg = {
        .region_len = REGION_LEN, .chunk_size = CHUNK_SIZE,
        .model_path = model_path, .cgroup_path = cgroup_path,
        .budget_bytes = 3ULL * CHUNK_SIZE,
        .reconcile_interval = 1, // A-3
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);
    policy_t *policy = declared ? policy_layer_order_declared_create(g_r.n_chunks)
                                : policy_layer_order_learned_create(g_r.n_chunks);
    g_r.policy = policy;
    g_r.prefetch_enabled = true;
    // Arbitrary, non-monotonic touch sequence. For the declared policy this
    // IS the declared sequence (repeated cyclically) -- a deliberately
    // irregular one, so the lookup path is exercised, not just a clean scan.
    uint32_t seq[] = { 0, 3, 1, 7, 2, 6, 0, 5, 4, 3, 7, 1, 6, 0, 2 };
    if (declared) {
        policy_declare_sequence(&g_r, seq, sizeof(seq) / sizeof(seq[0]));
    }

    volatile sig_atomic_t stop = 0;
    pthread_t pager_thread;
    pager_args_t pa = { &g_r, &stop };
    pthread_create(&pager_thread, NULL, pager_trampoline, &pa);

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        chunk_t *c = &g_r.chunks[seq[i]];
        if (declared) pager_notify_access(&g_r, c); // WP1 (A-12): advance declared position
        volatile uint8_t x = g_r.map_a[c->region_off];
        (void)x;
    }
    usleep(50 * 1000); // let any in-flight prefetch settle

    stop = 1;
    pthread_join(pager_thread, NULL);

    cgroup_stat_snapshot_t snap;
    int fail = 0;
    if (cgroup_stat_snapshot_read(cgroup_path, "memory.stat", &snap) != 0) {
        printf("  FAIL: cannot read memory.stat\n");
        fail = 1;
    } else {
        uint64_t shmem;
        if (!cgroup_stat_snapshot_field(&snap, "shmem", &shmem)) {
            printf("  FAIL: 'shmem' field not found\n");
            fail = 1;
        } else {
            uint64_t expected = g_r.resident_bytes + g_r.known_overhead_bytes;
            printf("  memory.stat[shmem]=%llu, resident_bytes+known_overhead=%llu -- %s\n",
                   (unsigned long long)shmem, (unsigned long long)expected,
                   shmem == expected ? "EXACT MATCH" : "MISMATCH");
            if (shmem != expected) fail = 1;
        }
    }

    region_teardown(&g_r);
    policy_destroy(policy);
    return fail;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <cgroup_path> <model_path>\n", argv[0]);
        return 2;
    }
    int fail = 0;
    fail |= run_t1(argv[1], argv[2]);
    fail |= run_t2(argv[1], argv[2]);
    fail |= run_t4_one(argv[1], argv[2], 0); // layer_order_learned
    fail |= run_t4_one(argv[1], argv[2], 1); // layer_order_declared (WP1 / A-12)

    if (fail) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
