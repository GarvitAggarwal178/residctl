// residctl_llama.c -- WP2. See residctl_llama.h.
#define _GNU_SOURCE
#include "residctl_llama.h"

#include "region.h"
#include "pager.h"
#include "policy.h"
#include "metrics.h"
#include "trace.h"
#include "fetch_trace.h"   // FINAL SESSION Phase 3
#include "policy_trace.h"  // FINAL SESSION Phase 3

#include "gguf.h"   // third_party/llama.cpp/ggml/include

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define RC_ALIGN 4096ULL
static uint64_t align_down(uint64_t v) { return v & ~(RC_ALIGN - 1); }
static uint64_t align_up(uint64_t v)   { return (v + RC_ALIGN - 1) & ~(RC_ALIGN - 1); }

// ---- config ---------------------------------------------------------------
static char     g_model_path[1024];
static char     g_cgroup[512];
static uint64_t g_budget_bytes;
static char     g_policy_name[64] = "layer_order_declared";
static int      g_prefetch_on;
static uint32_t g_prefetch_depth = 2;
static int      g_retention_none;          // 0 => pinned (default)
static uint32_t g_fetch_workers = 4;
static int      g_eager_reconcile;
static char     g_reftrace_path[1024];
static char     g_fetchtrace_path[1024];   // FINAL SESSION Phase 3
static char     g_policytrace_path[1024];  // FINAL SESSION Phase 3
static uint32_t g_fetching_timeout_ms = 0; // CLEANUP session: 0 => region_startup default (30000)
static int      g_protect_current = 0;      // LIVELOCK FIX A-14: default OFF. The eval
                                           // callback now fires notify on the PRE-compute
                                           // pass (Defect 2) and matches the "embd" node
                                           // (Defect 4), so the declared cursor tracks the
                                           // real read frontier and seq[pos] is distance 0
                                           // by construction -- the heuristic is redundant,
                                           // exactly as on replay_main.c's --consumption-
                                           // signal all-threads path. The cleanup session's
                                           // "protect OFF => arm D +67-78%" was a Defect-2/
                                           // Defect-4 artifact (cursor lagged a full layer,
                                           // token_embd never signalled); with the fixes,
                                           // protect on vs off is within +-1.8% at every
                                           // ratio (results/livelock/phase3c_arm_d_protect_
                                           // on.csv). On the pre-consumption path the
                                           // heuristic's residual effect -- pinning
                                           // seq[pos-1], the previous layer -- is a
                                           // mis-rank. protect_current=on in the config
                                           // still forces it on.

static void load_config(void) {
    const char *cfg = getenv("RESIDCTL_CONFIG");
    if (!cfg) { fprintf(stderr, "residctl_llama: RESIDCTL_CONFIG unset\n"); _exit(3); }
    FILE *f = fopen(cfg, "r");
    if (!f) { fprintf(stderr, "residctl_llama: cannot open config %s\n", cfg); _exit(3); }
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (line[0] == '#' || line[0] == 0) continue;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; const char *k = line, *v = eq + 1;
        if      (!strcmp(k, "model"))            snprintf(g_model_path, sizeof g_model_path, "%s", v);
        else if (!strcmp(k, "cgroup"))           snprintf(g_cgroup, sizeof g_cgroup, "%s", v);
        else if (!strcmp(k, "budget_bytes"))     g_budget_bytes = strtoull(v, NULL, 10);
        else if (!strcmp(k, "policy"))           snprintf(g_policy_name, sizeof g_policy_name, "%s", v);
        else if (!strcmp(k, "prefetch"))         g_prefetch_on = !strcmp(v, "on");
        else if (!strcmp(k, "prefetch_depth"))   g_prefetch_depth = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "retention"))        g_retention_none = !strcmp(v, "none");
        else if (!strcmp(k, "fetch_workers"))    g_fetch_workers = (uint32_t)strtoul(v, NULL, 10);
        else if (!strcmp(k, "eager_reconcile"))  g_eager_reconcile = !strcmp(v, "1");
        else if (!strcmp(k, "reftrace"))         snprintf(g_reftrace_path, sizeof g_reftrace_path, "%s", v);
        else if (!strcmp(k, "fetchtrace"))       snprintf(g_fetchtrace_path, sizeof g_fetchtrace_path, "%s", v);
        else if (!strcmp(k, "policytrace"))      snprintf(g_policytrace_path, sizeof g_policytrace_path, "%s", v);
        else if (!strcmp(k, "protect_current"))  g_protect_current = strcmp(v, "off") != 0;
        else if (!strcmp(k, "fetching_timeout_ms")) g_fetching_timeout_ms = (uint32_t)strtoul(v, NULL, 10);
    }
    fclose(f);
    if (!g_model_path[0] || !g_cgroup[0] || !g_budget_bytes) {
        fprintf(stderr, "residctl_llama: config needs model=, cgroup=, budget_bytes=\n"); _exit(3);
    }
}

// ---- tensor inventory ----------------------------------------------------
// group codes: >=0 transformer layer N; the negatives below for non-layer.
#define GRP_HEADER  (-1000)
#define GRP_EMBD    (-1)     // token_embd.weight
#define GRP_OUTPUT  (-2)     // output.weight (final projection)
#define GRP_OUTNORM (-3)     // output_norm.weight
#define GRP_OTHER   (-4)     // rope_freqs etc.

// Human-readable label for a chunk group code (layer N or GRP_*). Buffer is
// static -- one caller at a time (diagnostic / abort paths only).
static const char *chunk_group_label(int64_t gc) {
    static char lb[32];
    if (gc >= 0)                 { snprintf(lb, sizeof lb, "L%lld", (long long)gc); return lb; }
    if (gc == GRP_HEADER)  return "header";
    if (gc == GRP_EMBD)    return "token_embd";
    if (gc == GRP_OUTPUT)  return "output";
    if (gc == GRP_OUTNORM) return "output_norm";
    return "other";
}

typedef struct {
    char     name[128];
    uint64_t file_off;
    uint64_t size;
    int64_t  group;
} tensor_ent_t;

static tensor_ent_t *g_tensors;
static int64_t        g_n_tensors;
static uint64_t       g_file_size, g_region_len, g_data_offset;
static int64_t        g_n_layers;

static residctl_chunk_spec_t *g_specs;
static uint32_t               g_n_specs;
static int64_t               *g_chunk_group;    // per chunk: layer N or GRP_*

// per-layer chunk lists (a real GGUF can split one layer across >1 file run)
#define MAX_CHUNKS_PER_LAYER 6
static uint32_t (*g_layer_chunks)[MAX_CHUNKS_PER_LAYER];
static uint8_t  *g_layer_nchunks;
static uint32_t  g_chunk_embd = 0xFFFFFFFFu, g_chunk_output = 0xFFFFFFFFu, g_chunk_outnorm = 0xFFFFFFFFu;

static uint32_t *g_declared_seq;
static uint32_t  g_declared_len;

static int64_t parse_layer(const char *name) {
    const char *p = strstr(name, "blk.");
    if (!p) return -1;
    p += 4;
    if (*p < '0' || *p > '9') return -1;
    int64_t n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    return (*p == '.') ? n : -1;
}

static int64_t classify(const char *nm) {
    int64_t L = parse_layer(nm);
    if (L >= 0) return L;
    if (strstr(nm, "token_embd"))  return GRP_EMBD;
    if (strstr(nm, "output_norm")) return GRP_OUTNORM;
    if (!strncmp(nm, "output.", 7) || !strcmp(nm, "output.weight")) return GRP_OUTPUT;
    return GRP_OTHER;
}

static int cmp_tensor_off(const void *a, const void *b) {
    uint64_t x = ((const tensor_ent_t *)a)->file_off, y = ((const tensor_ent_t *)b)->file_off;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void build_inventory_and_table(void) {
    struct gguf_init_params p = { .no_alloc = true, .ctx = NULL };
    struct gguf_context *ctx = gguf_init_from_file(g_model_path, p);
    if (!ctx) { fprintf(stderr, "residctl_llama: gguf_init_from_file(%s) failed\n", g_model_path); _exit(3); }

    g_data_offset = gguf_get_data_offset(ctx);
    g_n_tensors = gguf_get_n_tensors(ctx);
    g_tensors = calloc(g_n_tensors, sizeof *g_tensors);

    int64_t max_layer = -1;
    for (int64_t i = 0; i < g_n_tensors; i++) {
        const char *nm = gguf_get_tensor_name(ctx, i);
        snprintf(g_tensors[i].name, sizeof g_tensors[i].name, "%s", nm);
        g_tensors[i].file_off = g_data_offset + gguf_get_tensor_offset(ctx, i);
        g_tensors[i].size     = gguf_get_tensor_size(ctx, i);
        g_tensors[i].group    = classify(nm);
        if (g_tensors[i].group > max_layer) max_layer = g_tensors[i].group;
    }
    g_n_layers = max_layer + 1;

    struct stat st; stat(g_model_path, &st);
    g_file_size = (uint64_t)st.st_size;
    g_region_len = align_up(g_file_size);

    tensor_ent_t *srt = malloc(g_n_tensors * sizeof *srt);
    memcpy(srt, g_tensors, g_n_tensors * sizeof *srt);
    qsort(srt, g_n_tensors, sizeof *srt, cmp_tensor_off);

    // boundaries: 0 (header), then align_down() of the first tensor of each
    // maximal contiguous same-group run, then region_len.
    uint64_t *bnd = malloc((g_n_tensors + 2) * sizeof(uint64_t));
    int64_t  *grp = malloc((g_n_tensors + 2) * sizeof(int64_t));
    uint32_t nb = 0;
    bnd[nb] = 0; grp[nb] = GRP_HEADER; nb++;
    {
        uint64_t b = align_down(srt[0].file_off);
        if (b == 0) grp[0] = srt[0].group;       // no separate header chunk
        else { bnd[nb] = b; grp[nb] = srt[0].group; nb++; }
    }
    int64_t cur = grp[nb - 1];
    for (int64_t i = 1; i < g_n_tensors; i++) {
        if (srt[i].group == cur) continue;
        cur = srt[i].group;
        uint64_t b = align_down(srt[i].file_off);
        if (b <= bnd[nb - 1]) continue;          // same 4096 page -> merge
        bnd[nb] = b; grp[nb] = cur; nb++;
    }
    bnd[nb] = g_region_len;

    g_n_specs = nb;
    g_specs = calloc(g_n_specs, sizeof *g_specs);
    g_chunk_group = calloc(g_n_specs, sizeof *g_chunk_group);
    for (uint32_t c = 0; c < g_n_specs; c++) {
        g_specs[c].file_off = bnd[c];
        g_specs[c].region_off = bnd[c];
        g_specs[c].len = bnd[c + 1] - bnd[c];
        g_specs[c].layer_id = (grp[c] >= 0) ? (uint32_t)grp[c] : 0xFFFFFFFFu;
        g_chunk_group[c] = grp[c];
        if      (grp[c] == GRP_EMBD)    g_chunk_embd = c;
        else if (grp[c] == GRP_OUTPUT)  g_chunk_output = c;
        else if (grp[c] == GRP_OUTNORM) g_chunk_outnorm = c;
    }
    // if output.weight is tied to token_embd (TENSOR_DUPLICATED), there is no
    // separate output chunk -- the embd chunk serves both.
    if (g_chunk_output == 0xFFFFFFFFu) g_chunk_output = g_chunk_embd;
    if (g_chunk_outnorm == 0xFFFFFFFFu) g_chunk_outnorm = g_chunk_embd;

    g_layer_chunks = calloc(g_n_layers, sizeof *g_layer_chunks);
    g_layer_nchunks = calloc(g_n_layers, 1);
    for (uint32_t c = 0; c < g_n_specs; c++) {
        int64_t L = g_chunk_group[c];
        if (L >= 0 && L < g_n_layers && g_layer_nchunks[L] < MAX_CHUNKS_PER_LAYER)
            g_layer_chunks[L][g_layer_nchunks[L]++] = c;
    }

    // WP2: declared access sequence = the real per-token consumption order:
    //   token_embd, layer 0 chunk(s), layer 1 chunk(s), ..., output_norm, output.
    // The GGUF header chunk and any GRP_OTHER chunks are deliberately absent
    // (touched once at load, never re-needed) -> the policy treats them as
    // "never used again" and evicts them first.
    g_declared_seq = malloc((g_n_specs + 4) * sizeof(uint32_t));
    g_declared_len = 0;
    if (g_chunk_embd != 0xFFFFFFFFu) g_declared_seq[g_declared_len++] = g_chunk_embd;
    for (int64_t L = 0; L < g_n_layers; L++)
        for (uint8_t k = 0; k < g_layer_nchunks[L]; k++)
            g_declared_seq[g_declared_len++] = g_layer_chunks[L][k];
    if (g_chunk_outnorm != 0xFFFFFFFFu && g_chunk_outnorm != g_chunk_embd)
        g_declared_seq[g_declared_len++] = g_chunk_outnorm;
    if (g_chunk_output != 0xFFFFFFFFu && g_chunk_output != g_chunk_embd && g_chunk_output != g_chunk_outnorm)
        g_declared_seq[g_declared_len++] = g_chunk_output;

    free(srt); free(bnd); free(grp);
    gguf_free(ctx);

    int split_layers = 0;
    for (int64_t L = 0; L < g_n_layers; L++) if (g_layer_nchunks[L] > 1) split_layers++;
    fprintf(stderr, "residctl_llama: file=%llu region=%llu data_off=%llu tensors=%lld layers=%lld "
            "chunks=%u declared_len=%u split_layers=%d embd_chunk=%u out_chunk=%u\n",
            (unsigned long long)g_file_size, (unsigned long long)g_region_len,
            (unsigned long long)g_data_offset, (long long)g_n_tensors, (long long)g_n_layers,
            g_n_specs, g_declared_len, split_layers, g_chunk_embd, g_chunk_output);
}

// ---- region + pager -----------------------------------------------------
static region_t     g_r;
static run_manifest_t g_manifest;
static policy_t    *g_policy;
static metrics_t    g_metrics;
static trace_t     *g_reftrace;
static pthread_t    g_pager_thread;
static volatile sig_atomic_t g_stop;
static int          g_started;
static uint64_t     g_notify_seq;
// LIVELOCK FIX Phase 0b: per-chunk consumption-signal count + a one-shot audit.
// A node-name mismatch in wp2_gen.cpp:eval_cb() (as found in Phase 0 for
// token_embd) leaves a chunk the workload really consumes with zero signals,
// so the declared policy ranks it far-future and thrashes it -- silently.
// After two full declared passes we assert every declared chunk got a signal.
static uint32_t    *g_chunk_notify_count;
static int          g_notify_audit_done;
static fetch_trace_t  *g_fetchtrace;   // FINAL SESSION Phase 3
static policy_trace_t *g_policytrace;  // FINAL SESSION Phase 3

// FINAL SESSION Phase 3: on SIGUSR1, dump the residency state the arm-E
// collapse characterisation needs -- how many chunks are pinned, how many
// RESIDENT+unpinned, stat_pin_broken/infeasible, and the pager's own view.
// Async-signal-safety: only read plain scalars and write() a preformatted-ish
// line; snprintf is not strictly async-signal-safe but is used the same way
// item 10b's watchdog dump did, and this is a diagnostic path.
static void residctl_llama_sigusr1(int sig) {
    (void)sig;
    uint32_t pinned = 0, resident_unpinned = 0, resident = 0, fetching = 0;
    int64_t  fetching_idx = -1; uint64_t fetching_len = 0;
    for (uint32_t i = 0; i < g_r.n_chunks; i++) {
        chunk_t *c = &g_r.chunks[i];
        if (c->state == CHUNK_RESIDENT) {
            resident++;
            if (c->pin != 0) pinned++; else resident_unpinned++;
        }
        if (c->state == CHUNK_FETCHING) {
            fetching++;
            if (fetching_idx < 0) { fetching_idx = (int64_t)i; fetching_len = c->len; }
        }
    }
    char xbuf[320];
    int xn = snprintf(xbuf, sizeof xbuf,
        "RESIDCTL_SIGDUMP2 reserved_bytes=%llu pinned_prefetch_len=%u fetching_idx=%lld fetching_len=%llu "
        "stat_prefetch_declined=%llu stat_prefetch_infeasible=%llu stat_dedup_fetching=%llu "
        "stat_dedup_resident=%llu stat_bytes_fetched=%llu\n",
        (unsigned long long)g_r.reserved_bytes, g_r.pinned_prefetch_len,
        (long long)fetching_idx, (unsigned long long)fetching_len,
        (unsigned long long)g_r.stat_prefetch_declined, (unsigned long long)g_r.stat_prefetch_infeasible,
        (unsigned long long)g_r.stat_dedup_fetching, (unsigned long long)g_r.stat_dedup_resident,
        (unsigned long long)g_r.stat_bytes_fetched);
    if (xn > 0) { ssize_t w = write(2, xbuf, (size_t)xn); (void)w; }
    char buf[512];
    int n = snprintf(buf, sizeof buf,
        "RESIDCTL_SIGDUMP n_chunks=%u resident=%u resident_unpinned=%u pinned=%u fetching=%u "
        "resident_bytes=%llu budget_bytes=%llu stat_pin_broken=%llu stat_infeasible=%llu "
        "stat_absent_handled=%llu stat_evictions=%llu stat_prefetches=%llu\n",
        g_r.n_chunks, resident, resident_unpinned, pinned, fetching,
        (unsigned long long)g_r.resident_bytes, (unsigned long long)g_r.budget_bytes,
        (unsigned long long)g_r.stat_pin_broken, (unsigned long long)g_r.stat_infeasible,
        (unsigned long long)g_r.stat_absent_handled, (unsigned long long)g_r.stat_evictions,
        (unsigned long long)g_r.stat_prefetches);
    if (n > 0) { ssize_t w = write(2, buf, (size_t)n); (void)w; }
}

typedef struct { region_t *r; volatile sig_atomic_t *stop; } pager_args_t;
static void *pager_trampoline(void *a) {
    pager_args_t *p = a;
    pager_run(p->r, p->stop, 50);
    return NULL;
}

void *residctl_llama_mmap(int llama_fd, size_t file_size) {
    (void)llama_fd;
    load_config();
    build_inventory_and_table();

    if ((uint64_t)file_size != g_file_size)
        fprintf(stderr, "residctl_llama: WARNING llama file_size=%zu != stat size=%llu\n",
                file_size, (unsigned long long)g_file_size);

    region_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.region_len = g_region_len;
    cfg.chunk_size = 0;
    cfg.model_path = g_model_path;
    cfg.cgroup_path = g_cgroup;
    cfg.budget_bytes = g_budget_bytes;
    cfg.reconcile_interval = g_eager_reconcile ? 1 : 0;
    cfg.prefetch_depth = g_prefetch_on ? g_prefetch_depth : 0;
    cfg.sync_handler = 0;               // async (A-5)
    cfg.fetch_workers = g_fetch_workers;
    cfg.prefetch_admission_always = 0;  // guarded (A-6 default)
    cfg.prefetch_retention_none = g_retention_none;
    cfg.explicit_chunks = g_specs;
    cfg.n_explicit_chunks = g_n_specs;
    cfg.fetching_timeout_ms = g_fetching_timeout_ms; // CLEANUP session, part b

    region_startup(&g_r, &cfg, &g_manifest);

    g_chunk_notify_count = calloc(g_r.n_chunks, sizeof(uint32_t)); // Phase 0b audit

    if      (!strcmp(g_policy_name, "lru"))
        g_policy = policy_lru_create();
    else if (!strcmp(g_policy_name, "layer_order_learned"))
        g_policy = policy_layer_order_learned_create(g_r.n_chunks);
    else if (!strcmp(g_policy_name, "layer_order_declared"))
        g_policy = policy_layer_order_declared_create(g_r.n_chunks);
    else if (!strcmp(g_policy_name, "default"))
        g_policy = NULL;
    else { fprintf(stderr, "residctl_llama: unknown policy '%s'\n", g_policy_name); _exit(3); }
    g_r.policy = g_policy;
    g_r.prefetch_enabled = g_prefetch_on ? true : false;
    policy_set_protect_current(g_protect_current);  // FINAL SESSION Phase 2/3
    // LIVELOCK FIX Defect 1 + Defect 2: wp2_gen.cpp:eval_cb() now fires the
    // consumption signal on the eval callback's PRE-compute pass, so the
    // signal precedes the weight read -> pre-consumption mode (seq[pos] has
    // next-use distance 0). This is what makes --protect-current redundant on
    // the real-model path; Phase 3 measures with it off.
    policy_set_signal_mode(1);

    if (g_policy && g_policy->declare_sequence) {
        // WP2: declared sequence = the real per-token consumption order
        // (token_embd, layer 0, layer 1, ..., output_norm, output). Built in
        // build_inventory_and_table(); NOT the naive 0..n-1 -- this GGUF's
        // tensors are stored in name-lexicographic order, so chunk index !=
        // layer order.
        policy_declare_sequence(&g_r, g_declared_seq, g_declared_len);
    }

    metrics_init(&g_metrics);
    g_r.metrics = &g_metrics;

    if (g_reftrace_path[0]) {
        g_reftrace = trace_open(g_reftrace_path, TRACE_TYPE_REFERENCE);
    }
    if (g_fetchtrace_path[0]) {   // FINAL SESSION Phase 3
        g_fetchtrace = fetch_trace_open(g_fetchtrace_path, 2000000);
        g_r.diag_fetch_trace = g_fetchtrace;
    }
    if (g_policytrace_path[0]) {  // FINAL SESSION Phase 3
        g_policytrace = policy_trace_open(g_policytrace_path, 2000000);
        g_r.diag_policy_trace = g_policytrace;
    }
    signal(SIGUSR1, residctl_llama_sigusr1);  // FINAL SESSION Phase 3 watchdog dump

    pager_args_t *pa = malloc(sizeof *pa);
    pa->r = &g_r; pa->stop = &g_stop;
    pthread_create(&g_pager_thread, NULL, pager_trampoline, pa);
    g_started = 1;

    fprintf(stderr, "residctl_llama: region up. policy=%s prefetch=%s depth=%u retention=%s "
            "budget=%llu n_chunks=%u map_a=%p\n",
            g_policy_name, g_prefetch_on ? "on" : "off", g_prefetch_depth,
            g_retention_none ? "none" : "pinned",
            (unsigned long long)g_budget_bytes, g_r.n_chunks, (void *)g_r.map_a);

    return g_r.map_a;
}

// LIVELOCK FIX Phase 0b: one-shot audit -- after >= 2 full declared passes,
// every chunk in the declared sequence must have received at least one
// consumption signal. A zero means eval_cb()'s node-name match for that
// chunk's role is broken (the Phase 0 finding for token_embd). Abort loudly
// with the chunk id AND its role label rather than degrade to silent thrash.
static void notify_audit_maybe(void) {
    if (g_notify_audit_done || !g_chunk_notify_count || g_declared_len == 0) return;
    if (g_notify_seq < (uint64_t)g_declared_len * 2) return;
    g_notify_audit_done = 1;
    uint32_t missing = 0;
    for (uint32_t i = 0; i < g_declared_len; i++) {
        uint32_t ci = g_declared_seq[i];
        if (ci >= g_r.n_chunks || g_chunk_notify_count[ci] != 0) continue;
        missing++;
        fprintf(stderr,
            "residctl_llama: FATAL -- declared chunk %u (%s) received 0 consumption signals "
            "after %llu notifies (>= 2 full declared passes). The workload consumes this chunk "
            "but wp2_gen.cpp:eval_cb() never signals it (node-name mismatch) -- the declared "
            "policy would treat it as far-future and thrash it. Aborting rather than measuring "
            "a silently-degraded run.\n",
            ci, chunk_group_label(g_chunk_group[ci]), (unsigned long long)g_notify_seq);
    }
    if (missing) abort();
    fprintf(stderr, "residctl_llama: consumption-signal audit OK -- all %u declared chunks "
            "signalled within 2 passes (%llu notifies)\n",
            g_declared_len, (unsigned long long)g_notify_seq);
}

static void notify_chunk(uint32_t c) {
    if (c == 0xFFFFFFFFu || c >= g_r.n_chunks) return;
    pager_notify_access(&g_r, &g_r.chunks[c]);
    g_notify_seq++;
    if (g_chunk_notify_count) g_chunk_notify_count[c]++;
    if (g_reftrace)
        trace_record(g_reftrace, g_notify_seq, c, TRACE_NA, TRACE_NA);
    notify_audit_maybe();
}

void residctl_llama_notify_layer(int layer) {
    if (!g_started || layer < 0 || layer >= g_n_layers) return;
    for (uint8_t k = 0; k < g_layer_nchunks[layer]; k++)
        notify_chunk(g_layer_chunks[layer][k]);
}

void residctl_llama_notify_role(int role) {
    if (!g_started) return;
    if      (role == 0) notify_chunk(g_chunk_embd);
    else if (role == 1) notify_chunk(g_chunk_outnorm);
    else if (role == 2) notify_chunk(g_chunk_output);
}

int residctl_llama_n_layers(void) { return (int)g_n_layers; }

void residctl_llama_write_inventory(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# GGUF tensor inventory -- %s\n", g_model_path);
    fprintf(f, "file_size=%llu region_len=%llu data_offset=%llu n_tensors=%lld n_layers=%lld n_chunks=%u\n",
            (unsigned long long)g_file_size, (unsigned long long)g_region_len,
            (unsigned long long)g_data_offset, (long long)g_n_tensors, (long long)g_n_layers, g_n_specs);
    fprintf(f, "\n## tensors (name, file_off, size, group[layer N or 'x'])\n");
    for (int64_t i = 0; i < g_n_tensors; i++) {
        if (g_tensors[i].group >= 0)
            fprintf(f, "%-40s %12llu %12llu  L%lld\n", g_tensors[i].name,
                    (unsigned long long)g_tensors[i].file_off, (unsigned long long)g_tensors[i].size,
                    (long long)g_tensors[i].group);
        else
            fprintf(f, "%-40s %12llu %12llu  x\n", g_tensors[i].name,
                    (unsigned long long)g_tensors[i].file_off, (unsigned long long)g_tensors[i].size);
    }
    fprintf(f, "\n## chunk table (idx, region_off, len, group)\n");
    for (uint32_t c = 0; c < g_n_specs; c++) {
        const char *g = chunk_group_label(g_chunk_group[c]);
        fprintf(f, "%3u  off=%12llu  len=%11llu  %s\n", c,
                (unsigned long long)g_specs[c].region_off, (unsigned long long)g_specs[c].len, g);
    }
    fprintf(f, "\n## declared sequence (chunk ids, %u entries)\n", g_declared_len);
    for (uint32_t i = 0; i < g_declared_len; i++)
        fprintf(f, "%u%s", g_declared_seq[i], (i + 1 < g_declared_len) ? " " : "\n");
    fprintf(f, "\n## split layers (>1 chunk)\n");
    for (int64_t L = 0; L < g_n_layers; L++)
        if (g_layer_nchunks[L] > 1) {
            fprintf(f, "L%lld:", (long long)L);
            for (uint8_t k = 0; k < g_layer_nchunks[L]; k++) fprintf(f, " %u", g_layer_chunks[L][k]);
            fprintf(f, "\n");
        }
    // chunk byte-size table for wp2_opt (one len per line, chunk-id order)
    fprintf(f, "\n## chunk_bytes (for wp2_opt)\n");
    for (uint32_t c = 0; c < g_n_specs; c++)
        fprintf(f, "%llu\n", (unsigned long long)g_specs[c].len);
    fclose(f);
}

static uint64_t cgroup_u64(const char *field) {
    char path[700], buf[64];
    snprintf(path, sizeof path, "%s/%s", g_cgroup, field);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return 0; }
    fclose(f);
    return strtoull(buf, NULL, 10);
}

void residctl_llama_teardown(void) {
    if (!g_started) return;
    g_stop = 1;
    pthread_join(g_pager_thread, NULL);
    if (g_reftrace) { trace_close(g_reftrace); g_reftrace = NULL; }
    if (g_fetchtrace) {   // FINAL SESSION Phase 3: once, after the pager stopped
        fetch_trace_flush(g_fetchtrace); fetch_trace_close(g_fetchtrace);
        g_r.diag_fetch_trace = NULL; g_fetchtrace = NULL;
    }
    if (g_policytrace) {
        policy_trace_flush(g_policytrace); policy_trace_close(g_policytrace);
        g_r.diag_policy_trace = NULL; g_policytrace = NULL;
    }

    uint64_t mem_peak = cgroup_u64("memory.peak");
    double p99 = (double)latency_hist_percentile_ns(&g_metrics.handler_latency, 0.99);

    printf("RESIDCTL_STATS,policy=%s,prefetch=%s,budget_bytes=%llu,n_chunks=%u,n_layers=%lld,"
           "region_len=%llu,file_size=%llu,"
           "absent_handled=%llu,evictions=%llu,infeasible=%llu,prefetches=%llu,"
           "pager_bytes_fetched=%llu,dedup_resident=%llu,dedup_fetching=%llu,pin_broken=%llu,"
           "resident_bytes_end=%llu,memory_peak=%llu,handler_p99_ns=%.0f,notify_layers=%llu,"
           "stat_fetching_timeout=%llu,protect_current=%s,signal_mode=%s,stat_prefetch_declined=%llu\n",
           g_policy_name, g_prefetch_on ? "on" : "off", (unsigned long long)g_budget_bytes,
           g_r.n_chunks, (long long)g_n_layers,
           (unsigned long long)g_r.region_len, (unsigned long long)g_file_size,
           (unsigned long long)g_r.stat_absent_handled, (unsigned long long)g_r.stat_evictions,
           (unsigned long long)g_r.stat_infeasible, (unsigned long long)g_r.stat_prefetches,
           (unsigned long long)g_r.stat_bytes_fetched, (unsigned long long)g_r.stat_dedup_resident,
           (unsigned long long)g_r.stat_dedup_fetching, (unsigned long long)g_r.stat_pin_broken,
           (unsigned long long)g_r.resident_bytes, (unsigned long long)mem_peak, p99,
           (unsigned long long)g_notify_seq,
           (unsigned long long)g_r.stat_fetching_timeout,
           policy_get_protect_current() ? "on" : "off",
           policy_get_signal_mode() ? "pre" : "post",
           (unsigned long long)g_r.stat_prefetch_declined);
    fflush(stdout);

    region_teardown(&g_r);
    policy_destroy(g_policy);
    g_started = 0;
}
