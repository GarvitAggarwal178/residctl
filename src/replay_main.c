// replay_main.c -- standalone trace-replay driver binary (build-order item
// 6). This is the primary workload for the harness (item 10)'s arms C/D/E
// until item 11's llama.cpp integration exists (§12: "the replay driver
// comes before the engine integration so a complete result exists even if
// integration runs long").
//
// Usage: replay_main <cgroup_path> <model_path> <region_len> <chunk_size>
//                     <budget_bytes> <n_passes> [policy] [prefetch] [trace_out_path]
//   policy: "default" (no policy set -- lowest-index fallback, budget.c),
//           "lru", or "layer_order". Defaults to "default".
//   prefetch: "on" or "off" (item 8's prefetch_enabled). Defaults to "off".
//             Distinguishes §11 arm D (layer_order, prefetch off) from
//             arm E (layer_order, prefetch on) -- before this flag existed,
//             prefetch fired unconditionally whenever a policy was set, so
//             D and E were the same run. See CLAUDE.md item 10.
#define _GNU_SOURCE
#include "region.h"
#include "pager.h"
#include "trace.h"
#include "metrics.h"
#include "replay.h"
#include "policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>

static region_t g_r;

typedef struct { region_t *r; volatile sig_atomic_t *stop; } pager_args_t;
static void *pager_trampoline(void *argp) {
    pager_args_t *p = (pager_args_t *)argp;
    pager_run(p->r, p->stop, 50);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 7 || argc > 10) {
        fprintf(stderr,
                "usage: %s <cgroup_path> <model_path> <region_len> <chunk_size> "
                "<budget_bytes> <n_passes> [policy] [prefetch] [trace_out_path]\n", argv[0]);
        return 2;
    }
    const char *cgroup_path = argv[1];
    const char *model_path = argv[2];
    uint64_t region_len = strtoull(argv[3], NULL, 10);
    uint64_t chunk_size = strtoull(argv[4], NULL, 10);
    uint64_t budget_bytes = strtoull(argv[5], NULL, 10);
    uint32_t n_passes = (uint32_t)strtoul(argv[6], NULL, 10);
    const char *policy_name = (argc >= 8) ? argv[7] : "default";
    const char *prefetch_arg = (argc >= 9) ? argv[8] : "off";
    const char *trace_path = (argc == 10) ? argv[9] : NULL;

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

    trace_t *trace = NULL;
    if (trace_path) {
        trace = trace_open(trace_path);
        g_r.trace = trace;
    }
    metrics_t metrics;
    metrics_init(&metrics);
    g_r.metrics = &metrics;

    printf("region_startup OK: n_chunks=%u chunk_size=%llu budget_bytes=%llu (%.1f chunks) policy=%s prefetch=%s\n",
           g_r.n_chunks, (unsigned long long)g_r.chunks[0].len,
           (unsigned long long)g_r.budget_bytes,
           (double)g_r.budget_bytes / (double)g_r.chunks[0].len, policy_name, prefetch_arg);

    volatile sig_atomic_t stop = 0;
    pthread_t pager_thread;
    pager_args_t pager_args = { &g_r, &stop };
    pthread_create(&pager_thread, NULL, pager_trampoline, &pager_args);

    replay_result_t res = replay_cyclic(&g_r, n_passes);

    stop = 1;
    pthread_join(pager_thread, NULL);
    if (trace) { trace_close(trace); g_r.trace = NULL; }

    double seconds = (double)res.wall_ns / 1e9;
    printf("REPLAY SUMMARY\n");
    printf("  passes=%u touches=%u wall_ns=%llu (%.3fs) touches/sec=%.1f\n",
           res.n_passes, res.n_touches, (unsigned long long)res.wall_ns, seconds,
           seconds > 0 ? (double)res.n_touches / seconds : 0.0);
    printf("  resident_bytes=%llu budget_bytes=%llu\n",
           (unsigned long long)g_r.resident_bytes, (unsigned long long)g_r.budget_bytes);
    printf("  fault_missing=%llu fault_minor=%llu dedup_resident=%llu dedup_fetching=%llu\n",
           (unsigned long long)g_r.stat_fault_missing, (unsigned long long)g_r.stat_fault_minor,
           (unsigned long long)g_r.stat_dedup_resident, (unsigned long long)g_r.stat_dedup_fetching);
    printf("  absent_handled=%llu evictions=%llu bytes_punched=%llu infeasible=%llu\n",
           (unsigned long long)g_r.stat_absent_handled, (unsigned long long)g_r.stat_evictions,
           (unsigned long long)g_r.stat_bytes_punched, (unsigned long long)g_r.stat_infeasible);
    printf("  handler_latency: count=%llu min_ns=%llu p50_ns=%llu p99_ns=%llu max_ns=%llu\n",
           (unsigned long long)metrics.handler_latency.count,
           (unsigned long long)metrics.handler_latency.min_ns,
           (unsigned long long)latency_hist_percentile_ns(&metrics.handler_latency, 0.50),
           (unsigned long long)latency_hist_percentile_ns(&metrics.handler_latency, 0.99),
           (unsigned long long)metrics.handler_latency.max_ns);
    printf("  queue_depth_high_water=%u\n", metrics.queue_depth_high_water);

    // Machine-parseable line for the item 10 harness's sensitivity table.
    printf("ARM_CSV,policy=%s,prefetch=%s,budget_bytes=%llu,touches=%u,wall_ns=%llu,"
           "absent_handled=%llu,evictions=%llu,infeasible=%llu,prefetches=%llu\n",
           policy_name, prefetch_arg, (unsigned long long)budget_bytes, res.n_touches,
           (unsigned long long)res.wall_ns, (unsigned long long)g_r.stat_absent_handled,
           (unsigned long long)g_r.stat_evictions, (unsigned long long)g_r.stat_infeasible,
           (unsigned long long)g_r.stat_prefetches);

    if (g_r.resident_bytes > g_r.budget_bytes) {
        fprintf(stderr, "FAIL: resident_bytes exceeded budget_bytes -- I-4/§7 violated\n");
        return 1;
    }

    region_teardown(&g_r);
    policy_destroy(policy);
    printf("PASS\n");
    return 0;
}
