// budget.h -- MECHANISM_SPEC.md §7 eviction, budget, reconcile.
#ifndef RESIDCTL_BUDGET_H
#define RESIDCTL_BUDGET_H

#include "region.h"

// I-7: read memory.stat[shmem] from r->cgroup_path, compare to
// resident_bytes + known_overhead_bytes. Aborts if they diverge by more
// than one chunk's worth of bytes -- "the only defence against an eviction
// that silently didn't happen."
void reconcile(region_t *r);

// §7 evict(): punches the chunk's hole via FALLOC_FL_PUNCH_HOLE. Asserts
// the chunk is RESIDENT and unpinned (I-4). Takes the chunk's lock itself.
void evict_chunk(region_t *r, chunk_t *c);

// §7 ensure_budget(): reconciles, then evicts until
// resident_bytes + need <= budget_bytes. Returns 0 on success, -1 if
// infeasible (no evictable victim left) -- the caller records this as a
// censored data point (§11), it is NOT an abort-worthy bug.
//
// TEMPORARY victim selection until build-order item 7 wires in the real
// policy_t: picks the lowest layer_id among RESIDENT, unpinned chunks. This
// exists only to make ensure_budget's own mechanism (the reconcile/evict
// loop, infeasible detection) testable before lru/layer_order exist.
int ensure_budget(region_t *r, uint64_t need);

#endif
