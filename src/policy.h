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
    // Campaign 13 Phase A.3: diagnostic-only accessor for --policy-trace.
    // Returns the chunk currently used as this policy's ordering cursor
    // (layer_order_learned: the successor-chain walk's starting point, i.e.
    // the chunk most recently passed to on_fault; layer_order_declared: the
    // chunk at the current position in the declared sequence), or CHUNK_NONE
    // if the policy has no cursor concept (lru). Read-only; does not affect
    // select_victim/predict_next/next_use_distance's outcome.
    uint32_t (*trace_cursor)(region_t *);

    // WP1 (Amendment A-12): the workload's own "I am about to consume this
    // chunk" signal, fired once per reference by pager_notify_access()
    // (pager.h) -- the same call the retention FIFO already listens on.
    // layer_order_declared advances its position in the declared sequence
    // HERE, never from on_fault/on_resident, so the position is a function
    // of the workload's declared order and not of fault-dispatch timing.
    // NULL for policies with no declared-sequence position (lru,
    // layer_order_learned).
    void (*on_access)(region_t *, chunk_t *);

    // WP1 (Amendment A-12): receives the workload's access sequence in
    // advance (the reference string for one pass; the policy may assume it
    // repeats cyclically). Called once at startup by policy_declare_sequence()
    // before the first touch. NULL for policies that infer order online
    // (lru, layer_order_learned).
    void (*declare_sequence)(region_t *, const uint32_t *, uint32_t);

    void *state; // policy-private; NULL for lru (needs none)
};

// Control policy. select_victim: oldest chunk->last_fault_seq among
// RESIDENT+unpinned (that field is already maintained by pager.c's
// handle_absent for every policy, not just this one). predict_next: always
// -1, matching MECHANISM_SPEC §8's table ("isolates authority from
// policy... predicted to tie the baseline").
policy_t *policy_lru_create(void);

// The informed policy, LEARNED variant (was `layer_order` through Campaign
// 13; renamed by WP1 / Amendment A-12, behaviour byte-for-byte unchanged).
// Learns a next-chunk mapping online from the ACTUAL fetch sequence
// (on_fault is called once per real ABSENT->FETCHING transition, in fetch
// order). predict_next(c) returns the chunk historically observed to follow
// c, or -1 if never observed. select_victim walks the learned chain forward
// from "now" and evicts the RESIDENT+unpinned chunk with the largest
// next-use distance (a chunk never reached by the walk is treated as
// infinitely far, i.e. the best possible victim -- the standard
// Belady-approximation rule). Campaign 13 Phase A found its chain
// construction to be fault-dispatch-order-dependent under concurrent driver
// threads with real compute; retained as the comparison arm A-12 measures
// declared order against.
policy_t *policy_layer_order_learned_create(uint32_t n_chunks);

// The informed policy, DECLARED variant (WP1 / Amendment A-12). Takes the
// workload's access sequence in advance via policy_declare_sequence() and
// derives next-use distance as a lookup into that sequence from the current
// consumption position (advanced only by on_access(), i.e.
// pager_notify_access() -- never from a fault). predict_next(c) returns the
// chunk at the next position of the declared sequence relative to c.
// select_victim evicts the RESIDENT+unpinned chunk whose declared next use
// is furthest ahead (never appears again => INT64_MAX => best victim). This
// implements §1's claim that the application knows its access order in
// advance, rather than inferring it from the past as the kernel does.
policy_t *policy_layer_order_declared_create(uint32_t n_chunks);

// WP1 (A-12): hand the workload's one-pass access sequence to r->policy. A
// no-op for policies that infer order online (lru, layer_order_learned).
// Call once at startup, before the first touch.
void policy_declare_sequence(region_t *r, const uint32_t *chunk_ids, uint32_t n);

// FINAL SESSION Phase 2: --protect-current {on,off}. Toggles the session-2 WP0
// heuristic in layer_order_declared's lo_declared_dist() (return 0 for
// seq[pos]/seq[pos-1]). Default on. Set once at startup, before the first
// touch. No effect on lru or layer_order_learned.
void policy_set_protect_current(int on);
int  policy_get_protect_current(void);

void policy_destroy(policy_t *p);

#endif
