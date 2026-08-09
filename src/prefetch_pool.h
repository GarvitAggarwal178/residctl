// prefetch_pool.h -- item 10b Task B: prefetch depth > 1.
//
// §6.3's original text allowed exactly one outstanding prefetch, and item
// 8 implemented that inline in the single handler thread (prefetch.c,
// maybe_prefetch() -- completely unchanged by this file). This is the ONE
// place multi-threading enters the pager, exactly as Task B specifies: a
// FIXED pool of `r->prefetch_depth` worker threads, each capable of having
// one fetch in flight, so up to `depth` chunks can be fetched concurrently
// ahead of the current one. The fault handler itself stays single-threaded.
//
// All existing invariants still hold: I-4 (a prefetch that would need to
// evict a pinned chunk is dropped via ensure_budget()'s infeasible return,
// never forced), I-6 (each chunk's own lock is still held across its whole
// fetch), and budget enforcement is unchanged in effect -- only its
// internal bookkeeping gained a reserved_bytes stage (budget.h) so
// multiple in-flight fetches can't over-commit the same budget space
// before any of them has actually completed.
#ifndef RESIDCTL_PREFETCH_POOL_H
#define RESIDCTL_PREFETCH_POOL_H

#include "region.h"

typedef struct prefetch_pool prefetch_pool_t;

// Starts r->prefetch_depth worker threads and assigns the pool to
// r->prefetch_pool_handle. Call once, after region_startup(), before the
// handler thread starts touching r->prefetch_depth>1 codepaths. Only call
// when r->prefetch_depth > 1 (depth==1 uses prefetch.c's original path and
// needs no pool).
prefetch_pool_t *prefetch_pool_start(region_t *r);

// Called from the handler thread (pager.c) after `just_resident` becomes
// RESIDENT. Tops up the pool's outstanding requests (queued + in-flight)
// toward r->prefetch_depth by walking the policy's predict_next chain
// forward from just_resident, skipping any chunk that's already
// RESIDENT/FETCHING/queued/in-flight, or that predict_next can't extend to
// (chain runs out). Never blocks the handler thread -- just enqueues and
// returns.
void prefetch_pool_top_up(region_t *r, chunk_t *just_resident);

// Signals all workers to stop once their queue drains, joins them, and
// frees the pool. Call after the handler thread has stopped.
void prefetch_pool_stop(prefetch_pool_t *pool);

#endif
