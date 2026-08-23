// policy_trace.h -- Campaign 13 Phase A.3: per-eviction victim-decision
// record, diagnostic-only (mirrors fetch_trace.h's design: preallocated
// array, written once at exit, gated behind --policy-trace). Not part of
// MECHANISM_SPEC's data model; not consumed by any solver.
//
// One record is written per select_victim() call that returns a real
// victim (CHUNK_NONE-returning calls are not eviction decisions and are
// not recorded). Both ensure_budget() and ensure_budget_prefetch() call
// sites are instrumented -- the struct does not distinguish which, since
// the question Phase A asks (does the SEQUENCE of victim choices match
// between two runs) doesn't need that tag.
#ifndef RESIDCTL_POLICY_TRACE_H
#define RESIDCTL_POLICY_TRACE_H

#include <stdint.h>

// Fixed 64-bit resident-set bitmap: valid for n_chunks <= 64. Sufficient
// for every cell this campaign's Phase A actually traces (128 MiB chunks,
// 16 chunks per 2 GiB region) -- NOT valid at 8 MiB (256 chunks) or
// smaller. Caller must check n_chunks before relying on the bitmap fields;
// n_resident and the other fields remain correct at any n_chunks.
typedef struct {
    uint64_t seq;
    uint32_t victim_chunk;
    uint32_t cursor_chunk;               // CHUNK_NONE if the policy has no cursor concept (lru)
    int64_t victim_next_use_distance;    // INT64_MAX if unknown
    uint32_t n_resident;
    uint32_t resident_set_bitmap_lo;     // chunks 0-31
    uint32_t resident_set_bitmap_hi;     // chunks 32-63
} __attribute__((packed)) policy_trace_record_t;

typedef struct policy_trace {
    policy_trace_record_t *records;
    uint64_t capacity;
    uint64_t count;
    uint64_t next_seq;
    char path[256];
} policy_trace_t;

// Preallocates `capacity` records. Aborts if capacity is exceeded at
// runtime (a diagnostic run should size this generously).
policy_trace_t *policy_trace_open(const char *path, uint64_t capacity);

// Caller must already hold r->budget_lock (every call site does -- see
// budget.c). Not thread-safe on its own; relies on that external lock,
// same as the policy state it's observing.
policy_trace_record_t *policy_trace_reserve(policy_trace_t *pt);

void policy_trace_flush(policy_trace_t *pt);
void policy_trace_close(policy_trace_t *pt);

#endif
