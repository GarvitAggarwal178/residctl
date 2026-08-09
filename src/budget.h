// budget.h -- MECHANISM_SPEC.md §7 eviction, budget, reconcile.
#ifndef RESIDCTL_BUDGET_H
#define RESIDCTL_BUDGET_H

#include "region.h"

// I-7: read memory.stat[shmem] from r->cgroup_path, compare to
// resident_bytes + known_overhead_bytes. Aborts if they diverge by more
// than one chunk's worth of bytes -- "the only defence against an eviction
// that silently didn't happen." Deliberately does NOT include
// reserved_bytes (Task B): only actually-resident, populated pages are
// comparable to the kernel's memory.stat[shmem].
//
// Caller must hold r->budget_lock (ensure_budget() does this internally;
// this function assumes it's already held when called from there).
void reconcile(region_t *r);

// §7 evict(): punches the chunk's hole via FALLOC_FL_PUNCH_HOLE. Asserts
// the chunk is RESIDENT and unpinned (I-4). Takes the chunk's lock itself.
// Caller must hold r->budget_lock.
void evict_chunk(region_t *r, chunk_t *c);

// §7 ensure_budget(): acquires r->budget_lock, reconciles (per the A-3
// schedule), evicts until resident_bytes + reserved_bytes + need <=
// budget_bytes, then RESERVES `need` bytes (reserved_bytes += need) before
// releasing the lock. Returns 0 on success, -1 if infeasible (no evictable
// victim left, budget_lock already released) -- the caller records this as
// a censored data point (§11), it is NOT an abort-worthy bug, and per
// item 10b Task B: "a prefetch that would force eviction of a pinned chunk
// is dropped, not forced" -- this is exactly what an infeasible return
// already causes callers to do (see prefetch.c, prefetch_pool.c).
//
// On success, the caller MUST call commit_reserved(r, need) once the
// actual fetch has completed, to move the reservation into resident_bytes.
//
// Task B note: this used to be single-threaded-only (item 4's temporary
// victim selector comment). It is now safe to call concurrently from
// multiple threads (the handler thread and any prefetch_pool workers) --
// budget_lock serializes the whole reconcile+evict+reserve sequence.
int ensure_budget(region_t *r, uint64_t need);

// Moves `len` bytes from reserved_bytes to resident_bytes, under
// budget_lock. Call exactly once, immediately after fetch_chunk()
// completes successfully for a chunk whose budget was reserved via a
// prior successful ensure_budget(r, len) call.
void commit_reserved(region_t *r, uint64_t len);

// Task B: commit_reserved() plus pinning `c`, atomically under the same
// budget_lock acquisition. See budget.c for why this specific combination
// (not two separate calls) is required.
void commit_reserved_and_pin(region_t *r, chunk_t *c, uint64_t len);

// Unpins `c` (pairs with commit_reserved_and_pin). Call once the caller is
// done doing anything that might need budget_lock while still holding
// c->lock for a chunk that's already RESIDENT.
void unpin_chunk(region_t *r, chunk_t *c);

#endif
