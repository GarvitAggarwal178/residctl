// policy.h -- MECHANISM_SPEC.md §8 policy interface + the two live policies.
#ifndef RESIDCTL_POLICY_H
#define RESIDCTL_POLICY_H

#include "region.h"

#define CHUNK_NONE UINT32_MAX

// Tag must be `policy` to match region.h's forward declaration.
struct policy {
    const char *name;
    void (*on_fault)(region_t *, chunk_t *);
    void (*on_resident)(region_t *, chunk_t *);
    uint32_t (*select_victim)(region_t *);      // CHUNK_NONE if none evictable
    int32_t (*predict_next)(region_t *, chunk_t *); // -1 if no prediction
    // item 10c Task B (A-6): how many hops away (by the SAME notion of
    // "next use" select_victim already uses) chunk c is from "now."
    // INT64_MAX means unknown/unreachable. Used by budget.c's
    // prefetch_admit() to compare a candidate eviction victim against a
    // prefetch's own target -- a prefetch may not evict something needed
    // SOONER than itself. lru always returns INT64_MAX (it has no
    // predict_next either, so it can never show a prefetch target is
    // closer than a victim -- guarded admission correctly declines every
    // eviction-requiring prefetch under lru).
    int64_t (*next_use_distance)(region_t *, chunk_t *);
    void *state; // policy-private; NULL for lru (needs none)
};

// Control policy. select_victim: oldest chunk->last_fault_seq among
// RESIDENT+unpinned (that field is already maintained by pager.c's
// handle_absent for every policy, not just this one). predict_next: always
// -1, matching MECHANISM_SPEC §8's table ("isolates authority from
// policy... predicted to tie the baseline").
policy_t *policy_lru_create(void);

// The informed policy. Learns a next-chunk mapping online from the ACTUAL
// fetch sequence (on_fault is called once per real ABSENT->FETCHING
// transition, in fetch order) -- never from a declared/assumed access
// order, per §8's explicit requirement not to hardcode "next = chunk_id+1".
// predict_next(c) returns the chunk historically observed to follow c, or
// -1 if never observed. select_victim walks the learned chain forward from
// "now" and evicts the RESIDENT+unpinned chunk with the largest next-use
// distance (a chunk never reached by the walk is treated as infinitely far,
// i.e. the best possible victim -- the standard Belady-approximation rule).
policy_t *policy_layer_order_create(uint32_t n_chunks);

void policy_destroy(policy_t *p);

#endif
