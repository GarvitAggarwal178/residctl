// region.h -- data structures and startup sequence, MECHANISM_SPEC.md §3-4.
#ifndef RESIDCTL_REGION_H
#define RESIDCTL_REGION_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define RESIDCTL_ALIGN 4096ULL

typedef enum { CHUNK_ABSENT = 0, CHUNK_FETCHING, CHUNK_RESIDENT } chunk_state_t;

typedef struct {
    uint64_t file_off;      // offset in the source file
    uint64_t region_off;    // offset within the mapped region
    uint64_t len;           // bytes; == aligned chunk size except possibly last
    uint32_t layer_id;      // for the layer-order policy (item 7)
    chunk_state_t state;
    uint64_t last_fault_seq;  // for LRU (item 7)
    uint32_t pin;             // >0 => never evict (I-4)
    pthread_mutex_t lock;     // guards state + fetch (I-6)
    pthread_cond_t cv;        // for waiters on FETCHING (item 2)

    // item 10d Task B: diagnostic-only, populated by handle_absent_dispatch()
    // (async handler) immediately before enqueueing this chunk to the fetch
    // pool, consumed by whichever worker picks it up to fill the
    // --fetch-trace record's dispatch-latency fields. Stays 0 under
    // --sync-handler (no separate dispatch phase to measure there) or when
    // --fetch-trace isn't requested. Safe without extra locking: only one
    // dispatch can be in flight for a given chunk at a time (it's
    // CHUNK_FETCHING the whole window between the write here and the read in
    // prefetch_pool.c's do_one_demand()), and both sides hold c->lock.
    uint64_t diag_dispatch_entry_ns;
    uint64_t diag_dispatch_enqueue_ns;
} chunk_t;

// Forward-declared; concrete definitions live in trace.h / metrics.h / a
// future policy.h, kept out of this header to avoid a circular include -- a
// .c file that needs the concrete types includes those headers directly
// alongside this one (pager.c does, for trace_t and metrics_t).
typedef struct policy policy_t;   // NULL until item 7
typedef struct trace trace_t;     // caller-assigned after region_startup(); NULL if untraced
typedef struct metrics metrics_t; // caller-assigned after region_startup(); NULL if unmeasured
typedef struct fetch_trace fetch_trace_t; // item 10b Task A; NULL unless --fetch-trace
typedef struct policy_trace policy_trace_t; // Campaign 13 Phase A.3; NULL unless --policy-trace
typedef struct prefetch_pool prefetch_pool_t; // item 10b Task B; NULL unless prefetch_depth>1

typedef struct {
    int memfd;
    uint8_t *map_a;          // registered; HANDLER MUST NOT TOUCH (I-1)
    uint8_t *map_b;           // unregistered; population path
    uint64_t region_len;
    int uffd;                 // O_NONBLOCK (I-2)
    int model_fd;              // O_DIRECT, or buffered fallback (see used_o_direct)
    int model_fd_buf;          // WP2: always a plain buffered fd, used only for the
                                // sub-4096 tail of the final chunk (O_DIRECT can't do
                                // an unaligned partial read). -1 if not opened.
    bool used_o_direct;
    uint64_t model_file_size;  // WP2: real source-file length. fetch_read() clamps
                                // reads here and zero-fills the tail padding when a
                                // chunk's 4096-aligned end runs past EOF (a real GGUF
                                // is not a 4096 multiple). 0 => no clamp (replay path,
                                // where file_size == region_len exactly).
    chunk_t *chunks;           // sorted by region_off
    uint32_t n_chunks;
    uint64_t resident_bytes;   // our accounting (I-7)
    uint64_t budget_bytes;
    uint64_t fault_seq;        // monotonic, for LRU and trace ordering
    uint64_t known_overhead_bytes; // pager-owned shmem charges outside chunk data (§7 note)
    policy_t *policy;   // NULL until item 7
    bool prefetch_enabled; // item 8's maybe_prefetch() only runs when this is true; default false
    trace_t *trace;     // NULL unless the caller opens one (trace_open()) and assigns it
    metrics_t *metrics; // NULL unless the caller inits one (metrics_init()) and assigns it
    fetch_trace_t *diag_fetch_trace; // item 10b Task A; NULL unless --fetch-trace
    policy_trace_t *diag_policy_trace; // Campaign 13 Phase A.3; NULL unless --policy-trace
    char cgroup_path[256];

    // Bare fault/dedup/eviction counters. These satisfy §9's "treatment-arm
    // only" list directly (fault count by type, dedup hit count, eviction
    // count + bytes punched); item 5 added the pieces these didn't cover
    // (trace_t, metrics_t's latency histogram + queue depth). Kept as plain
    // fields rather than folded into metrics_t because they're needed even
    // when metrics is NULL (items 2-4's own tests read them directly).
    uint64_t stat_fault_missing;
    uint64_t stat_fault_minor;
    uint64_t stat_dedup_resident;
    uint64_t stat_dedup_fetching;
    uint64_t stat_absent_handled;
    uint64_t stat_evictions;
    uint64_t stat_bytes_punched;
    uint64_t stat_infeasible;
    uint64_t stat_prefetches;            // successful prefetch completions (item 8)
    uint64_t stat_prefetch_infeasible;   // prefetch abandoned, budget couldn't fit it
    uint64_t stat_prefetch_declined;     // item 10c Task B (A-6): prefetch abandoned by
                                          // prefetch_admit() because its only available
                                          // victim is needed SOONER than the prefetch
                                          // target itself -- distinct from
                                          // stat_prefetch_infeasible (no victim existed at
                                          // all). Always 0 under --prefetch-admission always.
    uint64_t stat_bytes_fetched;         // pager's own byte accounting (item 10 Defect 3):
                                          // sum of c->len over every successful fetch_chunk()
                                          // call (real faults + successful prefetches). Cross-
                                          // checked against /proc/PID/io read_bytes by callers.

    // Spec amendment A-3 (item 10 correction): reconcile() used to run on
    // every single fetch, paying a fresh open/read/close of memory.stat
    // each time -- a real, measured, un-amortized cost at small chunk
    // sizes. Now it runs on every eviction (unconditionally -- I-7's core
    // safety property is unchanged) and otherwise only every
    // reconcile_interval fetches. reconcile_interval==1 reproduces the old
    // eager behaviour exactly; the §13 correctness harness uses that via
    // region_config_t.reconcile_interval / an --eager-reconcile flag.
    uint32_t reconcile_interval; // set by region_startup from cfg; default 16
    uint64_t fetches_since_reconcile;

    // Item 10b Task B: prefetch depth. Caps how many prefetch requests may
    // be outstanding (queued+in-flight) at once -- see prefetch_pool.h.
    // Independent of worker-pool size since item 10c (fetch_workers, below):
    // depth used to imply "one worker per unit of depth" when the pool
    // existed only for prefetching; now the pool is shared with demand
    // fetches (item 10c Task A) and its size is fetch_workers, not depth.
    uint32_t prefetch_depth; // set by region_startup from cfg; default 1
    pthread_mutex_t budget_lock;  // serializes ensure_budget/evict_chunk/reserve
                                   // across the handler thread and any fetch/
                                   // prefetch workers
    uint64_t reserved_bytes; // bytes "spoken for" by in-flight FETCHING chunks
                              // (real fetch or prefetch), not yet actually
                              // resident. NOT included in reconcile()'s
                              // comparison against memory.stat[shmem] -- only
                              // resident_bytes represents real, populated pages.
    prefetch_pool_t *prefetch_pool_handle; // NULL unless a fetch pool is running
                                            // (item 10c: whenever async_handler is
                                            // true, owned/started/stopped by
                                            // pager_run itself; item 10b's old
                                            // sync-handler + depth>1 case still
                                            // has the CALLER start/stop it)

    // Item 10c (A-5, A-7): the handler thread must never block on I/O (§5
    // amendment). async_handler=true (the default) makes handle_fault()'s
    // ABSENT branch dispatch-only: it marks the chunk FETCHING and enqueues
    // it on the shared fetch pool (prefetch_pool_handle), then returns
    // immediately without ever calling fetch_chunk() itself. Actual fetches
    // (both real demand faults and speculative prefetches) run on
    // fetch_workers pool threads, with demand strictly prioritized over
    // prefetch in the queue. async_handler=false (--sync-handler) restores
    // the original item 2-10b behavior byte-for-byte, for A/B comparison --
    // see pager.c's handle_absent() (unchanged) vs handle_absent_dispatch()
    // (new).
    bool async_handler;      // !cfg->sync_handler; default true (async)
    uint32_t fetch_workers;  // set by region_startup from cfg; default 4.
                              // Only meaningful when async_handler is true.

    // item 10c Task B (A-6): "A prefetch may not force an eviction unless it
    // is strictly justified" -- see budget.h's ensure_budget_prefetch().
    // false (the default, "guarded"): a prefetch that would need to evict a
    // chunk needed SOONER than its own target (per policy->next_use_distance)
    // is dropped instead of forced. true (--prefetch-admission always):
    // restores the original unconditional-eviction behavior for comparison.
    bool prefetch_admission_always;

    // item 10d Task C (A-9): bounded retention of speculative fetch targets.
    // Supersedes item 10c's admission rule as the mechanism actually
    // targeting the 14-27% prefetch hit-rate ceiling (see
    // results/ASYNC_REPORT.md's traced explanation: admission gating never
    // declined anything because layer_order's own select_victim already
    // avoids evicting something closer than a prefetch target -- the real
    // waste was a prefetched chunk being evicted by a LATER fetch before its
    // own turn ever came, which admission gating never checked). A prefetch
    // target stays pinned (I-4) from the moment it becomes CHUNK_RESIDENT
    // until either the workload signals it was consumed
    // (pager_notify_access(), pager.h) or it falls off this bounded FIFO
    // once prefetch_depth NEWER targets have been pinned ("it has waited
    // longest and is most likely stale"). Guarded by budget_lock, same as
    // every other pin mutation in this codebase (commit_reserved_and_pin,
    // unpin_chunk).
    uint32_t *pinned_prefetch_queue; // ring buffer, capacity == pinned_prefetch_cap
    uint32_t pinned_prefetch_cap, pinned_prefetch_head, pinned_prefetch_len;
    uint64_t stat_pin_broken; // a demand fetch found no unpinned victim and had
                               // to break the coldest pinned prefetch target's
                               // pin instead of going infeasible -- "a
                               // speculative chunk must never starve a real
                               // one" (A-9).
    bool prefetch_retention_pinned; // true (default, --prefetch-retention
                                     // pinned) or false (--prefetch-retention
                                     // none, item 10c's immediate-unpin
                                     // behavior).
} region_t;

// Startup configuration. Everything the caller must supply to region_startup().
//
// NOTE: the caller/harness is responsible for placing this process into the
// cgroup at cgroup_path (e.g. write getpid() to cgroup_path/cgroup.procs,
// the same pattern the spike's orchestration scripts used) BEFORE calling
// region_startup(). region_startup only reads and verifies the cgroup's
// settings (§4 step 1); it does not join the cgroup itself -- MECHANISM_SPEC
// does not ask for that, and inventing it here would put a policy decision
// (which cgroup owns this process) in the wrong layer.
typedef struct {
    uint64_t region_len;        // total size of the mapped region, bytes
    uint64_t chunk_size;        // requested chunk size; rounded to RESIDCTL_ALIGN
    const char *model_path;     // source file for fetches (O_RDONLY, O_DIRECT preferred)
    const char *cgroup_path;    // e.g. "/sys/fs/cgroup/spike"
    uint64_t budget_bytes;      // must be nonzero; auto-compute (§4 step 9 formula) is not yet implemented
    uint32_t reconcile_interval; // 0 => default (16); 1 => eager, every fetch (A-3, --eager-reconcile)
    uint32_t prefetch_depth;     // 0 => default (1, item 8's original inline behavior). Task B: --prefetch-depth N
    bool sync_handler;           // item 10c (A-5/A-7): false (default) => async dispatch-only
                                  // handler; true (--sync-handler) => restores the original
                                  // fully-synchronous handler exactly, for A/B comparison.
    uint32_t fetch_workers;      // item 10c: 0 => default (4). Shared fetch-pool worker count
                                  // for the async handler (demand + prefetch). Independent of
                                  // prefetch_depth. Ignored when sync_handler is true.
    bool prefetch_admission_always; // item 10c Task B: false (default) => guarded
                                     // (--prefetch-admission guarded); true => always
                                     // (--prefetch-admission always, item 10b's original
                                     // unconditional-eviction behavior).
    bool prefetch_retention_none;   // item 10d Task C: false (default) => pinned
                                     // retention (--prefetch-retention pinned); true =>
                                     // --prefetch-retention none, item 10c's original
                                     // immediate-unpin-after-fetch behavior.

    // WP2 (llama.cpp integration): an explicit, non-uniform chunk table
    // built from a real GGUF's tensor layout. When n_explicit_chunks > 0,
    // build_chunk_table() ignores chunk_size and uses these specs verbatim
    // (they must be sorted by region_off, contiguous, 4096-aligned, and
    // cover [0, region_len)). NULL/0 => the original uniform table.
    const void *explicit_chunks;    // array of residctl_chunk_spec_t
    uint32_t    n_explicit_chunks;
} region_config_t;

// WP2: one entry of an explicit chunk table (region_config_t.explicit_chunks).
typedef struct {
    uint64_t file_off;
    uint64_t region_off;
    uint64_t len;
    uint32_t layer_id;
} residctl_chunk_spec_t;

// Run manifest, written once per run per §4 step 11.
typedef struct {
    char kernel_release[128];
    char shmem_enabled[64];
    uint64_t memory_max;         // UINT64_MAX if cgroup file reads "max"
    char memory_swap_max_raw[64]; // must be "0" (I-3)
    bool memory_high_set;
    uint64_t memory_high;
    uint64_t chunk_size;
    bool used_o_direct;
    uint64_t region_len;
    uint32_t n_chunks;
    char model_path[256];
    char cgroup_path[256];
} run_manifest_t;

// Returns 0 on success. On any assertion failure, calls abort() with a
// message to stderr identifying exactly which startup step failed (§4).
// Never warns and continues (I-3 is explicit about this).
int region_startup(region_t *r, const region_config_t *cfg, run_manifest_t *manifest_out);

void region_write_manifest(const run_manifest_t *m, const char *path);

void region_teardown(region_t *r);

#endif
