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
} chunk_t;

// Forward-declared; concrete definitions live in trace.h / metrics.h / a
// future policy.h, kept out of this header to avoid a circular include -- a
// .c file that needs the concrete types includes those headers directly
// alongside this one (pager.c does, for trace_t and metrics_t).
typedef struct policy policy_t;   // NULL until item 7
typedef struct trace trace_t;     // caller-assigned after region_startup(); NULL if untraced
typedef struct metrics metrics_t; // caller-assigned after region_startup(); NULL if unmeasured
typedef struct fetch_trace fetch_trace_t; // item 10b Task A; NULL unless --fetch-trace
typedef struct prefetch_pool prefetch_pool_t; // item 10b Task B; NULL unless prefetch_depth>1

typedef struct {
    int memfd;
    uint8_t *map_a;          // registered; HANDLER MUST NOT TOUCH (I-1)
    uint8_t *map_b;           // unregistered; population path
    uint64_t region_len;
    int uffd;                 // O_NONBLOCK (I-2)
    int model_fd;              // O_DIRECT, or buffered fallback (see used_o_direct)
    bool used_o_direct;
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

    // Item 10b Task B: prefetch depth. depth==1 (default) is the ORIGINAL
    // item-8 inline-synchronous path (prefetch.c's maybe_prefetch()),
    // completely unchanged -- budget_lock/reserved_bytes exist below but
    // are only exercised by more than one thread when depth>1, since
    // that's the only case with a worker pool. See prefetch_pool.h.
    uint32_t prefetch_depth; // set by region_startup from cfg; default 1
    pthread_mutex_t budget_lock;  // serializes ensure_budget/evict_chunk/reserve
                                   // across the handler thread and any prefetch
                                   // workers (depth>1 only introduces real
                                   // contention on this; at depth==1 it's
                                   // uncontended and costs one uncontended
                                   // lock/unlock per fetch)
    uint64_t reserved_bytes; // bytes "spoken for" by in-flight FETCHING chunks
                              // (real fetch or prefetch), not yet actually
                              // resident. NOT included in reconcile()'s
                              // comparison against memory.stat[shmem] -- only
                              // resident_bytes represents real, populated pages.
    prefetch_pool_t *prefetch_pool_handle; // NULL unless prefetch_depth>1 (Task B)
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
} region_config_t;

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
