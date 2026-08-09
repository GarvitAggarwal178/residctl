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

// Forward-declared; defined by later build-order items. NULL until then.
typedef struct policy policy_t;
typedef struct trace trace_t;
typedef struct metrics metrics_t;

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
    trace_t *trace;     // NULL until item 5
    metrics_t *metrics; // NULL until item 5
    char cgroup_path[256];

    // Bare counters standing in for item 5's real metrics_t. Recorded per
    // I-8 ("record type as a metric; do not branch on it") so the handler
    // loop is honestly testable before item 5 exists. Item 5 replaces these
    // with the real metrics_t and this block goes away.
    uint64_t stat_fault_missing;
    uint64_t stat_fault_minor;
    uint64_t stat_dedup_resident;
    uint64_t stat_dedup_fetching;
    uint64_t stat_absent_handled;
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
