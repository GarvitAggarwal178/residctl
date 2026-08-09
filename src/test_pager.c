// test_pager.c -- build-order items 2+3 verification: handler loop, state
// machine dispatch (I-8), and the fetch path (§6, I-5), tested together
// since handle_absent() cannot run without fetch_chunk().
//
// Not a substitute for the §13 correctness harness (T-1..T-5, item/task
// #30) -- this is a smaller, item-scoped check: does the loop resolve
// faults with correct data, with the state machine transitioning exactly
// once per chunk.
//
// KNOWN GAP, reported rather than papered over: phase 1 tries to force the
// RESIDENT/FETCHING dedup branches (I-8) by barrier-releasing 8 threads onto
// the same cold chunk simultaneously. Across 5 observed runs this produced
// exactly ONE uffd MISSING message every time (fault_missing=1 after phase
// 1), not several -- the single fetch+UFFDIO_CONTINUE for the whole 2 MiB
// chunk apparently completes before any other thread's access reaches the
// kernel far enough to queue a second message, on this machine. The dedup
// branches are implemented per §5's pseudocode and are believed correct by
// inspection, but are NOT yet confirmed exercised by a passing test. §13's
// T-3 (8 threads, tight budget, 60s, repeated eviction/refetch cycling) is
// a much better-positioned test to actually hit this window and is where
// this should get confirmed, not here.
//
// Usage: test_pager <cgroup_path> <model_path>
#define _GNU_SOURCE
#include "region.h"
#include "pager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>

#define REGION_LEN (16ULL * 1024 * 1024)
#define CHUNK_SIZE (2ULL * 1024 * 1024)
#define N_THREADS 8
#define TOUCHES_PER_THREAD 200

static region_t g_r;

static uint8_t expected_byte(uint64_t file_off) {
    return (uint8_t)(file_off ^ (file_off >> 8) ^ (file_off >> 16));
}

typedef struct {
    int id;
    unsigned seed;
} thread_arg_t;

typedef struct {
    region_t *r;
    volatile sig_atomic_t *stop;
} pager_args_t;

static void *pager_trampoline(void *argp) {
    pager_args_t *p = (pager_args_t *)argp;
    pager_run(p->r, p->stop, 50);
    return NULL;
}

static pthread_barrier_t g_start_barrier;

// All threads hammer the SAME still-ABSENT chunk (chunk 0), released
// simultaneously by a barrier, to actually exercise the FETCHING/RESIDENT
// dedup branches (I-8) instead of touching already-resolved pages, which
// never generate a uffd message at all once the PTE is installed.
static void *contention_thread(void *argp) {
    thread_arg_t *arg = (thread_arg_t *)argp;
    chunk_t *c = &g_r.chunks[0];
    pthread_barrier_wait(&g_start_barrier);
    volatile uint8_t sum = 0;
    for (int i = 0; i < TOUCHES_PER_THREAD; i++) {
        uint64_t off = (rand_r(&arg->seed) % c->len);
        sum += g_r.map_a[c->region_off + off];
    }
    (void)sum;
    return NULL;
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
        .budget_bytes = REGION_LEN, // full budget: no eviction needed (item 4 not built)
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);
    printf("region_startup OK: n_chunks=%u\n", g_r.n_chunks);

    volatile sig_atomic_t stop = 0;
    pthread_t pager_thread;
    pager_args_t pager_args = { &g_r, &stop };
    if (pthread_create(&pager_thread, NULL, pager_trampoline, &pager_args) != 0) {
        fprintf(stderr, "pthread_create(pager) failed\n");
        return 1;
    }

    // Phase 1: N_THREADS threads released simultaneously onto chunk 0 while
    // it is still CHUNK_ABSENT. This is the only way to actually exercise
    // the FETCHING/RESIDENT dedup branches (I-8): once a page's PTE is
    // installed, later reads never generate a uffd message at all, so
    // touching already-resolved chunks (as a naive "concurrent storm" would)
    // tests nothing new.
    pthread_barrier_init(&g_start_barrier, NULL, N_THREADS);
    pthread_t threads[N_THREADS];
    thread_arg_t targs[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        targs[i].id = i;
        targs[i].seed = (unsigned)(i * 2654435761u + 12345);
        pthread_create(&threads[i], NULL, contention_thread, &targs[i]);
    }
    for (int i = 0; i < N_THREADS; i++) pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&g_start_barrier);

    printf("phase 1 (contention: %d threads racing chunk 0 from cold) done\n", N_THREADS);
    printf("counters after phase 1: fault_missing=%llu fault_minor=%llu dedup_resident=%llu dedup_fetching=%llu absent_handled=%llu\n",
           (unsigned long long)g_r.stat_fault_missing, (unsigned long long)g_r.stat_fault_minor,
           (unsigned long long)g_r.stat_dedup_resident, (unsigned long long)g_r.stat_dedup_fetching,
           (unsigned long long)g_r.stat_absent_handled);
    if (g_r.stat_dedup_resident == 0 && g_r.stat_dedup_fetching == 0) {
        printf("NOTE: dedup counters are both 0 -- the race window wasn't hit this "
               "run (timing-dependent, not a failure by itself; see resident_bytes "
               "and mismatch checks below for the actual pass criteria).\n");
    }

    // Phase 2: sequentially touch the remaining chunks (1..n-1), which are
    // still cold, then verify every chunk's contents against the known
    // pattern.
    for (uint32_t i = 1; i < g_r.n_chunks; i++) {
        chunk_t *c = &g_r.chunks[i];
        volatile uint8_t x = g_r.map_a[c->region_off];
        (void)x;
    }
    usleep(200 * 1000); // let the pager drain any in-flight fetches

    int mismatches = 0;
    for (uint32_t i = 0; i < g_r.n_chunks; i++) {
        chunk_t *c = &g_r.chunks[i];
        for (uint64_t off = 0; off < c->len; off += 4096) {
            uint8_t got = g_r.map_a[c->region_off + off];
            uint8_t want = expected_byte(c->file_off + off);
            if (got != want) {
                fprintf(stderr, "MISMATCH chunk=%u off=%llu got=%u want=%u\n",
                        i, (unsigned long long)off, got, want);
                mismatches++;
            }
        }
    }
    printf("phase 2 (resolve remaining %u chunks + verify all %u): mismatches=%d resident_bytes=%llu\n",
           g_r.n_chunks - 1, g_r.n_chunks, mismatches, (unsigned long long)g_r.resident_bytes);
    printf("final counters: fault_missing=%llu fault_minor=%llu dedup_resident=%llu dedup_fetching=%llu absent_handled=%llu\n",
           (unsigned long long)g_r.stat_fault_missing, (unsigned long long)g_r.stat_fault_minor,
           (unsigned long long)g_r.stat_dedup_resident, (unsigned long long)g_r.stat_dedup_fetching,
           (unsigned long long)g_r.stat_absent_handled);

    stop = 1;
    pthread_join(pager_thread, NULL);

    if (mismatches != 0) {
        fprintf(stderr, "FAIL: %d data mismatches\n", mismatches);
        return 1;
    }
    if (g_r.stat_absent_handled != g_r.n_chunks) {
        fprintf(stderr, "FAIL: expected exactly %u ABSENT->RESIDENT transitions, got %llu\n",
                g_r.n_chunks, (unsigned long long)g_r.stat_absent_handled);
        return 1;
    }
    if (g_r.resident_bytes != g_r.region_len) {
        fprintf(stderr, "FAIL: resident_bytes=%llu != region_len=%llu\n",
                (unsigned long long)g_r.resident_bytes, (unsigned long long)g_r.region_len);
        return 1;
    }

    region_teardown(&g_r);
    printf("PASS\n");
    return 0;
}
