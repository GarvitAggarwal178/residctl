// replay_main.c -- standalone trace-replay driver binary (build-order item
// 6, corrected by item 10's Defects 1-3). This is the primary workload for
// the harness (item 10)'s arms C/D/E until item 11's llama.cpp integration
// exists.
//
// Usage: replay_main <cgroup_path> <model_path> <region_len> <chunk_size>
//                     <budget_bytes> <n_passes> [policy] [prefetch]
//                     [fault_trace_path] [reference_trace_path]
//                     [--eager-reconcile]
//   policy: "default" (no policy set), "lru", or "layer_order". Default "default".
//   prefetch: "on" or "off" (item 8). Default "off".
//   fault_trace_path: optional. If given, the PAGER's own fault trace
//     (TRACE_TYPE_FAULT) is written here -- misses only, metrics/dedup/
//     prefetch accounting use. NEVER solver input (Defect 1).
//   reference_trace_path: optional. If given, the WORKLOAD's ground-truth
//     reference trace (TRACE_TYPE_REFERENCE) is written here -- every
//     access in order, hit or miss. This is the only valid input to
//     belady_main (item 9).
//   --eager-reconcile: recognized anywhere in argv, sets reconcile_interval=1
//     (A-3). Default is the amortized interval (16).
//   --fetch-trace <path>: recognized anywhere in argv (consumes the next
//     arg as its value). Item 10b Task A: emits one fetch_trace_record_t
//     per real fetch AND per successful prefetch to <path> (raw binary
//     array, no header), written once at process exit -- see fetch_trace.h.
//   --sync-handler: item 10c. Restores the original fully-synchronous
//     handler (items 2-10b) exactly, for A/B comparison against the new
//     default async dispatch-only handler (A-5).
//   --fetch-workers N: item 10c. Shared fetch-pool worker count for the
//     async handler (demand + prefetch). Default 4. Ignored with
//     --sync-handler (which uses --prefetch-depth workers for its own
//     prefetch-only pool, exactly as item 10b did).
//   --prefetch-admission {always,guarded}: item 10c Task B. Default
//     guarded: a prefetch that would need to evict a chunk needed sooner
//     than its own target is dropped, not forced. "always" restores item
//     10b's original unconditional-eviction behavior, for comparison.
//   --driver-threads N: item 10d Task A. Default 1 (replay_cyclic(),
//     unchanged). N>1 uses replay_cyclic_mt(): N threads collectively
//     execute the SAME reference sequence, partitioned within each chunk's
//     page range with a barrier at chunk boundaries -- models parallel
//     compute over one layer, then advancing together.
//   --prefetch-retention {none,pinned}: item 10d Task C. Default pinned: a
//     prefetch target stays pinned from arrival until consumed
//     (pager_notify_access()) or it falls off the bounded prefetch_depth
//     FIFO. "none" restores item 10c's immediate-unpin-after-fetch
//     behavior, for comparison.
#define _GNU_SOURCE
#include "region.h"
#include "pager.h"
#include "trace.h"
#include "metrics.h"
#include "replay.h"
#include "policy.h"
#include "fetch_trace.h"
#include "prefetch_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

static region_t g_r;

typedef struct { region_t *r; volatile sig_atomic_t *stop; } pager_args_t;
static void *pager_trampoline(void *argp) {
    pager_args_t *p = (pager_args_t *)argp;
    pager_run(p->r, p->stop, 50);
    return NULL;
}

// Defect 3: cross-arm primary metric. Same helper as baseline_main.c
// (duplicated rather than shared -- these are small, standalone binaries
// throughout this project).
static uint64_t read_io_read_bytes(void) {
    FILE *f = fopen("/proc/self/io", "r");
    if (!f) return UINT64_MAX; // sentinel: not available on this kernel/config
    char line[256];
    uint64_t val = UINT64_MAX;
    while (fgets(line, sizeof line, f)) {
        unsigned long long v;
        if (sscanf(line, "read_bytes: %llu", &v) == 1) { val = v; break; }
    }
    fclose(f);
    return val;
}

int main(int argc, char **argv) {
    // Pull --eager-reconcile and --fetch-trace <path> out of argv wherever
    // they appear, before positional parsing.
    int eager_reconcile = 0;
    const char *fetch_trace_path = NULL;
    uint32_t prefetch_depth = 0; // 0 => region_startup's default (1)
    int sync_handler = 0;        // item 10c
    uint32_t fetch_workers = 0;  // item 10c: 0 => region_startup's default (4)
    int prefetch_admission_always = 0; // item 10c Task B: 0 => guarded (default)
    uint32_t driver_threads = 1; // item 10d Task A: default 1 (replay_cyclic(), unchanged)
    int prefetch_retention_none = 0; // item 10d Task C: 0 => pinned (default)
    int nargs = 0;
    char *args[16];
    for (int i = 1; i < argc && nargs < 16; i++) {
        if (strcmp(argv[i], "--eager-reconcile") == 0) { eager_reconcile = 1; continue; }
        if (strcmp(argv[i], "--fetch-trace") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--fetch-trace requires a path\n"); return 2; }
            fetch_trace_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--prefetch-depth") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--prefetch-depth requires a number\n"); return 2; }
            prefetch_depth = (uint32_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--sync-handler") == 0) { sync_handler = 1; continue; }
        if (strcmp(argv[i], "--fetch-workers") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--fetch-workers requires a number\n"); return 2; }
            fetch_workers = (uint32_t)strtoul(argv[++i], NULL, 10);
            continue;
        }
        if (strcmp(argv[i], "--prefetch-admission") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--prefetch-admission requires always|guarded\n"); return 2; }
            const char *mode = argv[++i];
            if (strcmp(mode, "always") == 0) prefetch_admission_always = 1;
            else if (strcmp(mode, "guarded") == 0) prefetch_admission_always = 0;
            else { fprintf(stderr, "--prefetch-admission must be 'always' or 'guarded', got '%s'\n", mode); return 2; }
            continue;
        }
        if (strcmp(argv[i], "--driver-threads") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--driver-threads requires a number\n"); return 2; }
            driver_threads = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (driver_threads == 0) { fprintf(stderr, "--driver-threads must be >= 1\n"); return 2; }
            continue;
        }
        if (strcmp(argv[i], "--prefetch-retention") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--prefetch-retention requires none|pinned\n"); return 2; }
            const char *mode = argv[++i];
            if (strcmp(mode, "none") == 0) prefetch_retention_none = 1;
            else if (strcmp(mode, "pinned") == 0) prefetch_retention_none = 0;
            else { fprintf(stderr, "--prefetch-retention must be 'none' or 'pinned', got '%s'\n", mode); return 2; }
            continue;
        }
        args[nargs++] = argv[i];
    }

    if (nargs < 6 || nargs > 10) {
        fprintf(stderr,
                "usage: %s <cgroup_path> <model_path> <region_len> <chunk_size> "
                "<budget_bytes> <n_passes> [policy] [prefetch] [fault_trace_path] "
                "[reference_trace_path] [--eager-reconcile]\n", argv[0]);
        return 2;
    }
    const char *cgroup_path = args[0];
    const char *model_path = args[1];
    uint64_t region_len = strtoull(args[2], NULL, 10);
    uint64_t chunk_size = strtoull(args[3], NULL, 10);
    uint64_t budget_bytes = strtoull(args[4], NULL, 10);
    uint32_t n_passes = (uint32_t)strtoul(args[5], NULL, 10);
    const char *policy_name = (nargs >= 7) ? args[6] : "default";
    const char *prefetch_arg = (nargs >= 8) ? args[7] : "off";
    const char *fault_trace_path = (nargs >= 9 && args[8][0] != '\0') ? args[8] : NULL;
    const char *reference_trace_path = (nargs >= 10 && args[9][0] != '\0') ? args[9] : NULL;

    bool prefetch_on;
    if (strcmp(prefetch_arg, "on") == 0) prefetch_on = true;
    else if (strcmp(prefetch_arg, "off") == 0) prefetch_on = false;
    else { fprintf(stderr, "prefetch must be 'on' or 'off', got '%s'\n", prefetch_arg); return 2; }

    region_config_t cfg = {
        .region_len = region_len,
        .chunk_size = chunk_size,
        .model_path = model_path,
        .cgroup_path = cgroup_path,
        .budget_bytes = budget_bytes,
        .reconcile_interval = eager_reconcile ? 1 : 0, // 0 => region_startup's default (16)
        .prefetch_depth = prefetch_depth,               // 0 => region_startup's default (1)
        .sync_handler = sync_handler,                    // item 10c
        .fetch_workers = fetch_workers,                  // item 10c: 0 => region_startup's default (4)
        .prefetch_admission_always = prefetch_admission_always, // item 10c Task B
        .prefetch_retention_none = prefetch_retention_none, // item 10d Task C
    };
    run_manifest_t m;
    region_startup(&g_r, &cfg, &m);

    policy_t *policy = NULL;
    if (strcmp(policy_name, "lru") == 0) {
        policy = policy_lru_create();
    } else if (strcmp(policy_name, "layer_order") == 0) {
        policy = policy_layer_order_create(g_r.n_chunks);
    } else if (strcmp(policy_name, "default") != 0) {
        fprintf(stderr, "unknown policy '%s' (expected default|lru|layer_order)\n", policy_name);
        return 2;
    }
    g_r.policy = policy;
    g_r.prefetch_enabled = prefetch_on;

    // item 10c: under the async handler (the default), pager_run() owns the
    // shared fetch pool's entire lifecycle itself (demand fetches need it
    // unconditionally, not just when prefetch is on) -- this driver must
    // NOT also start one, or there would be two. Only under --sync-handler
    // does this driver still own it, exactly as item 10b did: a
    // prefetch-only pool, only when prefetch_on && depth>1.
    prefetch_pool_t *pool = NULL;
    if (sync_handler && prefetch_on && g_r.prefetch_depth > 1) {
        pool = prefetch_pool_start(&g_r, g_r.prefetch_depth); // Task B: one worker per unit of depth
    }

    trace_t *fault_trace = NULL;
    if (fault_trace_path) {
        fault_trace = trace_open(fault_trace_path, TRACE_TYPE_FAULT);
        g_r.trace = fault_trace;
    }
    trace_t *ref_trace = NULL;
    if (reference_trace_path) {
        ref_trace = trace_open(reference_trace_path, TRACE_TYPE_REFERENCE);
    }
    metrics_t metrics;
    metrics_init(&metrics);
    g_r.metrics = &metrics;

    fetch_trace_t *fetch_trace = NULL;
    if (fetch_trace_path) {
        fetch_trace = fetch_trace_open(fetch_trace_path, 200000); // generous; aborts if exceeded, not silently truncated
        g_r.diag_fetch_trace = fetch_trace;
    }

    printf("region_startup OK: n_chunks=%u chunk_size=%llu budget_bytes=%llu (%.1f chunks) "
           "policy=%s prefetch=%s reconcile_interval=%u prefetch_depth=%u "
           "handler=%s fetch_workers=%u driver_threads=%u prefetch_retention=%s\n",
           g_r.n_chunks, (unsigned long long)g_r.chunks[0].len,
           (unsigned long long)g_r.budget_bytes,
           (double)g_r.budget_bytes / (double)g_r.chunks[0].len, policy_name, prefetch_arg,
           g_r.reconcile_interval, g_r.prefetch_depth,
           g_r.async_handler ? "async" : "sync", g_r.fetch_workers, driver_threads,
           g_r.prefetch_retention_pinned ? "pinned" : "none");

    uint64_t io_before = read_io_read_bytes();

    volatile sig_atomic_t stop = 0;
    pthread_t pager_thread;
    pager_args_t pager_args = { &g_r, &stop };
    pthread_create(&pager_thread, NULL, pager_trampoline, &pager_args);

    // item 10d Task A: driver_threads<=1 uses the ORIGINAL, unmodified
    // replay_cyclic() -- not replay_cyclic_mt() special-cased to N=1 -- so
    // the default preserves current behaviour byte-for-byte, per spec.
    replay_result_t res = (driver_threads <= 1)
        ? replay_cyclic(&g_r, n_passes, ref_trace)
        : replay_cyclic_mt(&g_r, n_passes, ref_trace, driver_threads);

    stop = 1;
    pthread_join(pager_thread, NULL);
    // Stop the pool AFTER the handler thread (no more real faults means no
    // more prefetch_pool_top_up() calls can enqueue new work) but BEFORE
    // flushing the fetch trace (workers may still be mid-fetch and can
    // still call fetch_trace_reserve() until they've all drained/joined).
    // prefetch_pool_stop() lets outstanding work finish rather than
    // abandoning an in-flight fetch_chunk() mid-call, which has no
    // partial-failure mode to unwind from.
    if (pool) prefetch_pool_stop(pool);
    if (fault_trace) { trace_close(fault_trace); g_r.trace = NULL; }
    if (ref_trace) trace_close(ref_trace);
    if (fetch_trace) {
        fetch_trace_flush(fetch_trace); // once, here, after the handler thread has stopped -- never from inside it
        fetch_trace_close(fetch_trace);
        g_r.diag_fetch_trace = NULL;
    }

    uint64_t io_after = read_io_read_bytes();
    uint64_t io_delta = (io_before != UINT64_MAX && io_after != UINT64_MAX) ? (io_after - io_before) : UINT64_MAX;

    double seconds = (double)res.wall_ns / 1e9;
    printf("REPLAY SUMMARY\n");
    printf("  passes=%u touches=%u bytes_touched=%llu wall_ns=%llu (%.3fs) touches/sec=%.1f\n",
           res.n_passes, res.n_touches, (unsigned long long)res.bytes_touched,
           (unsigned long long)res.wall_ns, seconds, seconds > 0 ? (double)res.n_touches / seconds : 0.0);
    printf("  elision_guard_sink=%llu (nonzero, run-dependent -- evidence the full-chunk read loop executed)\n",
           (unsigned long long)replay_sink_value());
    printf("  resident_bytes=%llu budget_bytes=%llu\n",
           (unsigned long long)g_r.resident_bytes, (unsigned long long)g_r.budget_bytes);
    printf("  fault_missing=%llu fault_minor=%llu dedup_resident=%llu dedup_fetching=%llu\n",
           (unsigned long long)g_r.stat_fault_missing, (unsigned long long)g_r.stat_fault_minor,
           (unsigned long long)g_r.stat_dedup_resident, (unsigned long long)g_r.stat_dedup_fetching);
    printf("  absent_handled=%llu evictions=%llu bytes_punched=%llu infeasible=%llu\n",
           (unsigned long long)g_r.stat_absent_handled, (unsigned long long)g_r.stat_evictions,
           (unsigned long long)g_r.stat_bytes_punched, (unsigned long long)g_r.stat_infeasible);
    printf("  prefetch_admission=%s prefetch_infeasible=%llu prefetch_declined=%llu\n",
           g_r.prefetch_admission_always ? "always" : "guarded",
           (unsigned long long)g_r.stat_prefetch_infeasible, (unsigned long long)g_r.stat_prefetch_declined);
    printf("  prefetch_retention=%s stat_pin_broken=%llu\n",
           g_r.prefetch_retention_pinned ? "pinned" : "none", (unsigned long long)g_r.stat_pin_broken);
    printf("  handler_latency: count=%llu min_ns=%llu p50_ns=%llu p99_ns=%llu max_ns=%llu\n",
           (unsigned long long)metrics.handler_latency.count,
           (unsigned long long)metrics.handler_latency.min_ns,
           (unsigned long long)latency_hist_percentile_ns(&metrics.handler_latency, 0.50),
           (unsigned long long)latency_hist_percentile_ns(&metrics.handler_latency, 0.99),
           (unsigned long long)metrics.handler_latency.max_ns);
    printf("  queue_depth_high_water=%u\n", metrics.queue_depth_high_water);

    // Defect 3: read_bytes is the PRIMARY cross-arm metric (§9). Report the
    // pager's own byte accounting AND the kernel's /proc/self/io delta
    // independently, and flag a discrepancy rather than picking one.
    printf("  pager_bytes_fetched=%llu (own accounting: sum of c->len over every successful fetch)\n",
           (unsigned long long)g_r.stat_bytes_fetched);
    if (io_delta == UINT64_MAX) {
        printf("  io_read_bytes_delta=NOT_AVAILABLE (/proc/self/io not readable on this system)\n");
    } else {
        printf("  io_read_bytes_delta=%llu\n", (unsigned long long)io_delta);
        uint64_t diff = (io_delta > g_r.stat_bytes_fetched) ? (io_delta - g_r.stat_bytes_fetched)
                                                              : (g_r.stat_bytes_fetched - io_delta);
        uint64_t threshold = g_r.chunks[0].len; // one chunk, same tolerance as I-7's reconcile()
        if (diff > threshold) {
            printf("  DISCREPANCY: pager_bytes_fetched (%llu) vs io_read_bytes_delta (%llu) differ by "
                   "%llu, exceeding one-chunk threshold (%llu) -- reporting both, not picking one\n",
                   (unsigned long long)g_r.stat_bytes_fetched, (unsigned long long)io_delta,
                   (unsigned long long)diff, (unsigned long long)threshold);
        } else {
            printf("  byte accounting cross-check: pager (%llu) vs io_read_bytes_delta (%llu) agree "
                   "within one chunk\n", (unsigned long long)g_r.stat_bytes_fetched, (unsigned long long)io_delta);
        }
    }

    // Machine-parseable line for the item 10 harness's sensitivity table.
    printf("ARM_CSV,policy=%s,prefetch=%s,budget_bytes=%llu,touches=%u,bytes_touched=%llu,wall_ns=%llu,"
           "absent_handled=%llu,evictions=%llu,infeasible=%llu,prefetches=%llu,"
           "pager_bytes_fetched=%llu,io_read_bytes_delta=%llu,dedup_resident=%llu,dedup_fetching=%llu,"
           "handler=%s,fetch_workers=%u,prefetch_admission=%s,prefetch_declined=%llu,"
           "driver_threads=%u,prefetch_retention=%s,pin_broken=%llu\n",
           policy_name, prefetch_arg, (unsigned long long)budget_bytes, res.n_touches,
           (unsigned long long)res.bytes_touched, (unsigned long long)res.wall_ns,
           (unsigned long long)g_r.stat_absent_handled, (unsigned long long)g_r.stat_evictions,
           (unsigned long long)g_r.stat_infeasible, (unsigned long long)g_r.stat_prefetches,
           (unsigned long long)g_r.stat_bytes_fetched, io_delta == UINT64_MAX ? 0 : (unsigned long long)io_delta,
           (unsigned long long)g_r.stat_dedup_resident, (unsigned long long)g_r.stat_dedup_fetching,
           g_r.async_handler ? "async" : "sync", g_r.fetch_workers,
           g_r.prefetch_admission_always ? "always" : "guarded", (unsigned long long)g_r.stat_prefetch_declined,
           driver_threads, g_r.prefetch_retention_pinned ? "pinned" : "none",
           (unsigned long long)g_r.stat_pin_broken);

    if (g_r.resident_bytes > g_r.budget_bytes) {
        fprintf(stderr, "FAIL: resident_bytes exceeded budget_bytes -- I-4/§7 violated\n");
        return 1;
    }

    region_teardown(&g_r);
    policy_destroy(policy);
    printf("PASS\n");
    return 0;
}
