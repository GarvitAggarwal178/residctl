// test_t7.c -- MECHANISM_SPEC.md §13 T-7 (item 10c, new): no fault is ever
// lost under the async handler. Every chunk enqueued to the fetch pool must
// eventually reach CHUNK_RESIDENT and wake whatever thread(s) faulted on
// it -- if a bug in the dispatch/enqueue/dequeue path drops a job, the
// faulting thread that triggered it blocks in the kernel forever (stuck in
// a page fault with nothing that will ever issue UFFDIO_CONTINUE for that
// address), which a plain "did the process exit nonzero" check cannot
// detect: the process just hangs.
//
// Runs T-3's storm (same load shape, same duration) then, immediately after
// telling the storm threads to stop, joins them under an internal 120s
// watchdog. Under normal operation this join is near-instant (bounded by
// whatever fetch was in flight when a thread's last access happened, at
// most tens of ms) -- if a thread is stuck on a genuinely lost fault, the
// join blocks and the watchdog fires: dumps /proc/PID/task/*/wchan and
// /status for every thread (same technique used to diagnose item 10b's
// real deadlocks, see CLAUDE.md) and hard-exits nonzero rather than hanging
// the test runner forever.
//
// Usage: test_t7 <cgroup_path> <model_path> <duration_seconds>
#define _GNU_SOURCE
#include "region.h"
#include "pager.h"
#include "policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#define REGION_LEN (16ULL * 1024 * 1024)
#define CHUNK_SIZE (2ULL * 1024 * 1024)   // 8 chunks
#define BUDGET_BYTES (4ULL * 1024 * 1024) // 2 chunks -- tight, forces constant eviction
#define N_THREADS 8
#define WATCHDOG_SECONDS 120

static region_t g_r;
static volatile sig_atomic_t g_stop_workers = 0;
static volatile sig_atomic_t g_join_done = 0;

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
        uint8_t got = g_r.map_a[c->region_off + off]; // may block on a real fault -- exactly what T-7 watches
        uint8_t want = expected_byte(c->file_off + off);
        res->touches++;
        if (got != want) res->mismatches++;
    }
    return NULL;
}

// Same diagnostic technique used live during item 10b's deadlock hunts:
// read /proc/<pid>/task/<tid>/wchan (what kernel function, if any, the
// thread is blocked in) and the State: line of /status for every thread.
static void dump_all_thread_states(void) {
    pid_t pid = getpid();
    char task_dir[64];
    snprintf(task_dir, sizeof task_dir, "/proc/%d/task", (int)pid);
    DIR *d = opendir(task_dir);
    if (!d) {
        fprintf(stderr, "T-7 watchdog: cannot open %s: %s\n", task_dir, strerror(errno));
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char wchan_path[512], status_path[512];
        snprintf(wchan_path, sizeof wchan_path, "%s/%s/wchan", task_dir, ent->d_name);
        snprintf(status_path, sizeof status_path, "%s/%s/status", task_dir, ent->d_name);

        char wchan[256] = "?";
        FILE *wf = fopen(wchan_path, "r");
        if (wf) { if (!fgets(wchan, sizeof wchan, wf)) wchan[0] = '\0'; fclose(wf); }

        char state_line[256] = "State: ?";
        FILE *sf = fopen(status_path, "r");
        if (sf) {
            char line[256];
            while (fgets(line, sizeof line, sf)) {
                if (strncmp(line, "State:", 6) == 0) { snprintf(state_line, sizeof state_line, "%s", line); break; }
            }
            fclose(sf);
        }
        // trim trailing newline
        size_t sl = strlen(state_line);
        if (sl > 0 && state_line[sl - 1] == '\n') state_line[sl - 1] = '\0';

        fprintf(stderr, "  tid=%s wchan=%s %s\n", ent->d_name, wchan, state_line);
    }
    closedir(d);
}

static void *watchdog_main(void *argp) {
    (void)argp;
    struct timespec ts = { .tv_sec = WATCHDOG_SECONDS, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
    if (!g_join_done) {
        fprintf(stderr,
                "T-7 FAIL: joining storm threads did not complete within the %ds watchdog -- "
                "a fault was lost (enqueued but never resolved to RESIDENT). Dumping thread "
                "state before hard exit:\n", WATCHDOG_SECONDS);
        dump_all_thread_states();
        fflush(stderr);
        _exit(1); // hard fail; do not let a hung join limp the test runner along
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
        .reconcile_interval = 1, // A-3
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);
    policy_t *policy = policy_layer_order_learned_create(g_r.n_chunks);
    g_r.policy = policy;
    g_r.prefetch_enabled = true;

    printf("T-7: async_handler=%d fetch_workers=%u watchdog=%ds\n",
           g_r.async_handler, g_r.fetch_workers, WATCHDOG_SECONDS);

    pthread_t watchdog;
    pthread_create(&watchdog, NULL, watchdog_main, NULL);
    // Detach: this test's own process exit (normal or the watchdog's _exit)
    // is what actually ends the watchdog's relevance; nothing needs to join it.
    pthread_detach(watchdog);

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
        pthread_join(threads[i], NULL); // this is what the watchdog is timing
        total_touches += results[i].touches;
        total_mismatches += results[i].mismatches;
    }

    pager_stop = 1;
    pthread_join(pager_thread, NULL);
    g_join_done = 1; // disarm the watchdog -- everything joined within budget

    printf("T-7 done: total_touches=%llu mismatches=%llu resident_bytes=%llu budget_bytes=%llu "
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
    printf("PASS (all %d storm threads joined within the %ds watchdog -- no lost fault)\n",
           N_THREADS, WATCHDOG_SECONDS);
    return 0;
}
