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

// item 10c Task B (A-6): like ensure_budget(), but for a SPECULATIVE fetch
// (target is the prefetch candidate chunk, still CHUNK_ABSENT). Demand
// fetches always call ensure_budget() unchanged -- only prefetches go
// through this gated path. Under r->prefetch_admission_always==false (the
// default, "guarded"), each eviction this would otherwise force is checked
// against r->policy->next_use_distance(): a victim must be STRICTLY colder
// (needed later) than target itself, or the eviction -- and therefore the
// whole prefetch -- is declined (stat_prefetch_declined++, budget_lock
// released, returns -1, same "abandon this prefetch, not fatal" contract
// ensure_budget() already has). Under prefetch_admission_always==true,
// behaves identically to ensure_budget(r, target->len) (item 10b's original
// unconditional-eviction behavior, for A/B comparison).
int ensure_budget_prefetch(region_t *r, chunk_t *target);

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

// Pins `c` under budget_lock (symmetric with unpin_chunk). item 10d: used to
// close a latent unlocked-pin race found while implementing Task C -- see
// prefetch_pool.c/prefetch.c's do_one_prefetch()/maybe_prefetch(), which
// used to do a bare `target->pin++` with no lock at all.
void pin_chunk(region_t *r, chunk_t *c);

// item 10d Task C (A-9): called once, right after a PREFETCH target `target`
// becomes CHUNK_RESIDENT, only when r->prefetch_retention_pinned is true.
// Does NOT touch target->pin itself -- the pre-fetch pin++ (I-4, protects
// the in-flight fetch) is repurposed as the retention pin, so callers must
// NOT also unpin after calling this. Pushes target onto the bounded FIFO
// (region_t.pinned_prefetch_queue); if this would exceed
// r->pinned_prefetch_cap (== prefetch_depth) entries, the OLDEST pinned
// entry is unpinned and evicted from the queue first ("it has waited
// longest and is most likely stale," per §6.3 Amendment A-9). Caller must
// hold budget_lock.
void prefetch_retain_on_resident(region_t *r, chunk_t *target);

// item 10d Task C (A-9): the "consumed" trigger. If chunk_idx is currently
// in the bounded pinned-prefetch queue, removes it and releases its
// retention pin; otherwise a no-op (the common case -- most references are
// not to a currently-retained prefetch target). Caller must hold
// budget_lock. See pager.h's pager_notify_access() for the public,
// lock-acquiring entry point the workload calls.
void prefetch_retain_release(region_t *r, uint32_t chunk_idx);

// item 10d Task C (A-9): "Demand fetches must never be blocked by pinned
// prefetch targets." Called by ensure_budget()'s eviction loop ONLY (never
// ensure_budget_prefetch() -- a prefetch never breaks another prefetch's
// retention pin) when select_victim() found no RESIDENT+unpinned chunk.
// Returns the COLDEST currently-pinned prefetch target by
// policy->next_use_distance (falling back to the queue's oldest entry if
// the policy has none, or all distances tie), or CHUNK_NONE if the pinned
// set is empty. Caller must hold budget_lock; caller is responsible for
// calling prefetch_retain_release() on the result before evicting it (so
// evict_chunk()'s I-4 pin==0 assertion still holds unweakened).
uint32_t pin_break_select_victim(region_t *r);

#endif
