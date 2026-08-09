// prefetch_pool.h -- item 10b Task B's fixed worker pool, GENERALIZED by
// item 10c Task A into the shared fetch pool for BOTH demand faults and
// speculative prefetches.
//
// History: item 10b Task B introduced this as a prefetch-only pool of
// r->prefetch_depth workers (§6.3's original text allowed exactly one
// outstanding prefetch; item 8 implemented that inline in the single
// handler thread, unchanged in prefetch.c). item 10c Task A found (via
// Task C's dedup_fetching proof, see CLAUDE.md) that keeping demand fetches
// INLINE in the handler thread was itself the deeper defect: it made
// CHUNK_FETCHING unobservable by construction and serialized every
// concurrent fault behind one blocking pread. The fix (spec amendment A-5)
// is architectural: the handler thread now only DISPATCHES (§5), and every
// real fetch -- demand or speculative -- runs on this SAME pool, so there is
// exactly one place multi-threaded fetching happens, not two.
//
// Two-tier queue, demand strictly prioritized: a worker always drains the
// demand queue before ever looking at the prefetch queue. A speculative
// fetch must never delay a real one. Demand jobs are never deduplicated in
// the queue (the dispatcher only ever enqueues a chunk once -- see pager.c;
// a second fault on the same chunk before its fetch completes hits the
// FETCHING dedup branch in handle_fault() and is dropped there, never
// reaching this pool at all). Prefetch jobs keep item 10b's per-chunk
// `tracked` dedup flag, since prefetch_pool_top_up() may be called
// repeatedly from multiple completing demand fetches.
//
// All existing invariants still hold: I-4 (a prefetch that would need to
// evict a pinned chunk is dropped via ensure_budget()'s infeasible return,
// never forced), I-6 (each chunk's own lock is held across its whole
// fetch), and budget enforcement is unchanged in effect -- reserved_bytes
// (budget.h) still lets multiple in-flight fetches share one budget without
// over-committing before any of them has actually completed.
#ifndef RESIDCTL_PREFETCH_POOL_H
#define RESIDCTL_PREFETCH_POOL_H

#include "region.h"

typedef struct prefetch_pool prefetch_pool_t;

// Starts n_workers worker threads and assigns the pool to
// r->prefetch_pool_handle. Two callers, two lifecycles:
//
//   - item 10c async handler (r->async_handler==true, the default):
//     pager_run() itself calls this at the top (n_workers = r->fetch_workers)
//     and prefetch_pool_stop() at the bottom -- the pool's lifetime is
//     entirely internal to "run the handler," since demand fetches need it
//     unconditionally, not just when prefetch is enabled.
//   - item 10b sync-handler path (--sync-handler, r->async_handler==false):
//     unchanged from before -- the CALLER (e.g. replay_main.c) starts this
//     itself, only when prefetch_on && r->prefetch_depth > 1, with
//     n_workers = r->prefetch_depth (one worker per unit of depth, exactly
//     item 10b's original rule), and stops it after the handler thread exits.
prefetch_pool_t *prefetch_pool_start(region_t *r, uint32_t n_workers);

// item 10c: called from the handler thread's dispatch path (pager.c,
// async_handler==true) for a genuine ABSENT->FETCHING transition. The
// caller must already hold chunk->lock, have set chunk->state=CHUNK_FETCHING,
// stamped chunk->last_fault_seq, and recorded the fault trace -- this
// function only enqueues; it never blocks and does no I/O. Demand jobs are
// always drained before prefetch jobs (strict priority).
void prefetch_pool_enqueue_demand(prefetch_pool_t *pool, uint32_t chunk_idx);

// Called after `just_resident` becomes RESIDENT (either from the async
// dispatch worker after a demand fetch, item 10c, or from the sync
// handler's handle_absent(), item 10b -- both call this the same way).
// Tops up the pool's outstanding PREFETCH requests (queued + in-flight)
// toward r->prefetch_depth by walking the policy's predict_next chain
// forward from just_resident, skipping any chunk that's already
// RESIDENT/FETCHING/queued/in-flight, or that predict_next can't extend to
// (chain runs out). Never blocks the caller -- just enqueues and returns.
void prefetch_pool_top_up(region_t *r, chunk_t *just_resident);

// Signals all workers to stop once their queues drain, joins them, and
// frees the pool. Call after no more callers will enqueue anything (the
// dispatch/handler thread has stopped).
void prefetch_pool_stop(prefetch_pool_t *pool);

#endif
