// prefetch_pool.c -- see prefetch_pool.h.
#define _GNU_SOURCE
#include "prefetch_pool.h"
#include "policy.h"
#include "pager.h"    // CLEANUP session: pager_abandon_fetch()
#include "budget.h"
#include "fetch.h"
#include "trace.h"
#include "fetch_trace.h"
#include "metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct prefetch_pool {
    region_t *r;
    uint32_t depth;       // cached r->prefetch_depth at start time
    pthread_t *workers;
    uint32_t n_workers;   // item 10c: independent of depth -- see prefetch_pool.h

    pthread_mutex_t lock;
    pthread_cond_t cv;
    int stop;

    // item 10c: demand queue. No dedup tracking needed -- the dispatcher
    // (pager.c handle_fault) only ever enqueues a chunk once per fetch cycle;
    // any subsequent fault on the same still-FETCHING chunk hits the
    // dedup_fetching branch there and never reaches this pool.
    uint32_t *demand_queue;
    uint32_t demand_cap, demand_head, demand_len;

    // item 10b's original prefetch queue, unchanged in spirit: per-chunk
    // dedup via pf_tracked (top_up may be called repeatedly by multiple
    // completing demand fetches and must not double-enqueue the same
    // speculative target).
    uint32_t *pf_queue;
    uint32_t pf_cap, pf_head, pf_len;
    uint32_t pf_outstanding; // queued + in-flight, not yet resident/dropped
    uint8_t *pf_tracked;     // per-chunk: 1 if queued or in-flight, size n_chunks
};

// item 10c: runs a DEMAND fetch. The dispatcher (pager.c) has already
// transitioned c to CHUNK_FETCHING, stamped last_fault_seq, and recorded the
// fault trace under c->lock before enqueueing -- this function re-acquires
// c->lock and performs the actual I/O (§6, I-6: the fetch itself -- read +
// CONTINUE + state update -- is one continuous critical section here, even
// though the ABSENT->FETCHING transition that preceded it was a separate,
// already-closed critical section in the dispatcher; see MECHANISM_SPEC.md
// A-5 for why this satisfies I-6's letter and intent).
static void do_one_demand(prefetch_pool_t *pool, uint32_t idx) {
    region_t *r = pool->r;
    chunk_t *c = &r->chunks[idx];
    uint64_t t_start = now_ns();

    pthread_mutex_lock(&c->lock);
    if (c->state != CHUNK_FETCHING) {
        fprintf(stderr,
                "PAGER FAILED: demand fetch worker picked up chunk %u in state %d, "
                "expected CHUNK_FETCHING (the dispatcher must have already set this "
                "before enqueueing -- I-6 violation if not)\n", idx, (int)c->state);
        abort();
    }
    // item 10d Task B: captured now, under c->lock, before anything else can
    // touch these fields (a chunk can't be re-dispatched while FETCHING).
    uint64_t dispatch_entry_ns = c->diag_dispatch_entry_ns;
    uint64_t dispatch_enqueue_ns = c->diag_dispatch_enqueue_ns;

    int budget_rc = ensure_budget(r, c->len);
    if (budget_rc != 0) {
        // Bounded retry: a real fault's ensure_budget() can go transiently
        // infeasible because prefetch workers reserved headroom via
        // reserved_bytes before anything they're fetching became actually
        // resident (evictable) -- always possible now that demand and
        // prefetch share one pool (item 10b Task B originally saw this only
        // at prefetch_depth>1; item 10c makes it reachable whenever any
        // prefetching is enabled, since fetch_workers no longer has to
        // equal 1 the way a synchronous handler implicitly did). Reserved
        // bytes are always transient (every prefetch either commits or is
        // dropped within one fetch's duration), so a short wait should let
        // a subsequent ensure_budget() succeed.
        for (int attempt = 0; attempt < 200 && budget_rc != 0; attempt++) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 }; // 2ms
            nanosleep(&ts, NULL);
            budget_rc = ensure_budget(r, c->len);
        }
    }
    if (budget_rc != 0) {
        // Infeasible at this budget (§7/§11 censoring rule). Revert to ABSENT.
        // CLEANUP session (7th concurrency-class fix, part a): ALSO UFFDIO_WAKE.
        // The old comment ("the faulting thread(s) stay blocked, same honest
        // behavior as the ... synchronous handler") was WRONG for the async
        // path: the dispatcher already consumed the uffd message, and every
        // faulter that deduped against this FETCHING episode (handle_fault's
        // CHUNK_FETCHING branch) is waiting on a CONTINUE that will never come.
        // pager_abandon_fetch() wakes them so they refault and retry once
        // budget frees. This is the primary cause of the arm-E r<=0.375
        // deadlock (see results/cleanup/phase1_deadlock_fix.md).
        pager_abandon_fetch(r, c); // -> ABSENT, fetching_since_ns=0, UFFDIO_WAKE
        pthread_mutex_unlock(&c->lock);
        return;
    }

    fetch_timing_t timing;
    fetch_chunk(r, c, r->diag_fetch_trace ? &timing : NULL); // §6, lock held throughout (I-6)
    // Commit before marking RESIDENT -- item 10b's deadlock fix #2, still
    // required: another worker's ensure_budget() can otherwise pick this
    // chunk as its victim while this thread still holds c->lock and is
    // about to need budget_lock via commit_reserved.
    commit_reserved_and_pin(r, c, c->len); // item 10b fix #3: pin atomically with the commit
    c->state = CHUNK_RESIDENT;
    c->fetching_since_ns = 0; // CLEANUP session: left FETCHING
    // item 10c: multiple fetch-pool workers now increment these stat
    // counters concurrently (previously only reachable from >1 thread at
    // prefetch_depth>1's OLD prefetch-only pool, and even then this exact
    // race existed but was never triggered by a test that noticed --
    // fixing it properly now rather than carrying it forward silently).
    __sync_fetch_and_add(&r->stat_bytes_fetched, c->len); // Defect 3: pager's own byte accounting
    if (r->policy && r->policy->on_resident) r->policy->on_resident(r, c);
    if (r->prefetch_enabled) {
        // item 10c: ALL speculative fetches -- regardless of prefetch_depth,
        // including depth==1 -- now go through the shared pool's prefetch
        // queue rather than item 8's inline maybe_prefetch() path. That
        // inline path ran synchronously in whichever thread called it,
        // which would let a single depth==1 prefetch silently skip the
        // demand-priority queue entirely; routing everything through
        // prefetch_pool_top_up() keeps exactly one mechanism and one
        // priority rule for every prefetch, at every depth, whenever the
        // async handler is active. (--sync-handler keeps the OLD
        // depth-conditional inline/pool split -- see pager.c's
        // handle_absent(), unchanged.)
        prefetch_pool_top_up(r, c);
    }
    unpin_chunk(r, c);
    __sync_fetch_and_add(&r->stat_absent_handled, 1);

    uint64_t t_exit = now_ns();
    if (r->metrics)
        latency_hist_record(&r->metrics->handler_latency, t_exit - t_start);
    if (r->diag_fetch_trace) {
        fetch_trace_record_t *rec = fetch_trace_reserve(r->diag_fetch_trace); // internally thread-safe
        rec->chunk_id = idx;
        rec->was_prefetch = 0;
        rec->t_handler_entry_ns = t_start;
        rec->t_read_start_ns = timing.read_start_ns;
        rec->t_read_end_ns = timing.read_end_ns;
        rec->t_continue_end_ns = timing.continue_end_ns;
        rec->t_handler_exit_ns = t_exit;
        rec->bytes_read = c->len;
        rec->t_dispatch_entry_ns = dispatch_entry_ns;
        rec->t_dispatch_enqueue_ns = dispatch_enqueue_ns;
    }

    // CLEANUP session (part a): a demand worker must never leave a chunk in
    // CHUNK_FETCHING -- every exit path resolves it to RESIDENT (this one) or,
    // on budget-infeasible, to ABSENT via pager_abandon_fetch(). Held under
    // c->lock, so no re-fault race.
    if (c->state == CHUNK_FETCHING) {
        fprintf(stderr, "PAGER FAILED: do_one_demand left chunk %u in CHUNK_FETCHING "
                "(7th-bug regression -- an exit path skipped the state reset)\n",
                (uint32_t)(c - r->chunks));
        abort();
    }
    pthread_mutex_unlock(&c->lock);
}

// Fetches one chunk on behalf of the pool as a PREFETCH. Mirrors prefetch.c's
// maybe_prefetch() body (item 8) but takes an explicit target index instead
// of computing one via predict_next -- the caller (prefetch_pool_top_up)
// already did that part of the chain walk. Unchanged from item 10b except
// for the renamed pf_* fields it's called alongside.
static void do_one_prefetch(prefetch_pool_t *pool, uint32_t idx) {
    region_t *r = pool->r;
    chunk_t *target = &r->chunks[idx];

    pthread_mutex_lock(&target->lock);
    if (target->state != CHUNK_ABSENT) {
        // Raced with a real fault or another prefetch that already
        // resolved this chunk between enqueue and pickup -- drop, not an
        // error (I-4/§6.3 spirit: never force, never wait).
        pthread_mutex_unlock(&target->lock);
        return;
    }

    uint64_t t_start = now_ns();
    // item 10d: this used to be a bare `target->pin++` with no lock at all --
    // harmless at item 10b's original prefetch-only pool sizes (still a real
    // data race, just one that had never been observed to matter), but a
    // genuine latent race now that this pool is shared with demand workers
    // (item 10c) and multiple prefetch workers can run concurrently. Found
    // by inspection while touching this exact line for Task C, fixed the
    // same way the two item 10c latent bugs were: use the lock, not carry it
    // forward silently.
    pin_chunk(r, target); // I-4: never punched while this prefetch targets it
    target->state = CHUNK_FETCHING;
    target->fetching_since_ns = t_start; // CLEANUP session: FETCHING watchdog clock
    uint64_t seq = __sync_add_and_fetch(&r->fault_seq, 1); // atomic: shared across workers + handler
    target->last_fault_seq = seq;
    if (r->trace)
        trace_record(r->trace, seq, idx, TRACE_FAULT_MISSING, 1 /* was_prefetched */);

    // item 10c Task B (A-6): prefetches go through the GATED budget path --
    // ensure_budget_prefetch() declines an eviction (rather than forcing
    // it) when the only available victim is needed sooner than this
    // prefetch's own target. Demand fetches (do_one_demand) still call the
    // unconditional ensure_budget() unchanged.
    int budget_rc = ensure_budget_prefetch(r, target);

    if (budget_rc != 0) {
        // Infeasible / declined: abandon this prefetch, not fatal.
        // CLEANUP session (part a): reset FETCHING + UFFDIO_WAKE. A demand
        // fault can dedup against this brief FETCHING window (target->lock is
        // held throughout, so handle_fault blocks on it and then sees the
        // reset state -- but pager_abandon_fetch's WAKE also covers a faulter
        // that got past the lock in the gap between our unlock and its
        // handle_fault, which the FETCHING watchdog would otherwise have to
        // catch 30 s later).
        unpin_chunk(r, target); // item 10d: was a bare `target->pin--` -- see pin_chunk()'s note above
        pager_abandon_fetch(r, target); // -> ABSENT, fetching_since_ns=0, WAKE
        target->decline_until_ns = now_ns() + 100 * 1000 * 1000ULL; // Defect 3: 100 ms backoff
        __sync_fetch_and_add(&r->stat_prefetch_infeasible, 1);
        pthread_mutex_unlock(&target->lock);
        return;
    }

    fetch_timing_t timing;
    fetch_chunk(r, target, r->diag_fetch_trace ? &timing : NULL);
    // Commit before marking RESIDENT -- see do_one_demand's note; the same
    // deadlock risk applies here (a real, reproduced hang at
    // --prefetch-depth 8 in item 10b, not a hypothetical).
    commit_reserved(r, target->len);
    target->state = CHUNK_RESIDENT;
    target->fetching_since_ns = 0; // CLEANUP session: left FETCHING
    __sync_fetch_and_add(&r->stat_bytes_fetched, target->len); // item 10c: now genuinely concurrent, see do_one_demand's note
    // item 10d Task C (A-9): under --prefetch-retention pinned (the
    // default), keep target pinned and register it in the bounded
    // retention FIFO instead of unpinning immediately -- the pre-fetch
    // pin_chunk() above is repurposed as the retention pin. Under
    // --prefetch-retention none, restore item 10c's original behavior
    // (unpin right away).
    if (r->prefetch_retention_pinned) {
        pthread_mutex_lock(&r->budget_lock);
        prefetch_retain_on_resident(r, target);
        pthread_mutex_unlock(&r->budget_lock);
    } else {
        unpin_chunk(r, target); // item 10d: was a bare `target->pin--` -- see pin_chunk()'s note above
    }
    __sync_fetch_and_add(&r->stat_prefetches, 1);
    if (r->policy->on_resident) r->policy->on_resident(r, target);

    uint64_t t_exit = now_ns();
    if (r->diag_fetch_trace) {
        fetch_trace_record_t *rec = fetch_trace_reserve(r->diag_fetch_trace); // internally thread-safe
        rec->chunk_id = idx;
        rec->was_prefetch = 1;
        rec->t_handler_entry_ns = t_start;
        rec->t_read_start_ns = timing.read_start_ns;
        rec->t_read_end_ns = timing.read_end_ns;
        rec->t_continue_end_ns = timing.continue_end_ns;
        rec->t_handler_exit_ns = t_exit;
        rec->bytes_read = target->len;
    }

    // CLEANUP session (part a): same invariant as do_one_demand -- a prefetch
    // worker must never leave a chunk in CHUNK_FETCHING.
    if (target->state == CHUNK_FETCHING) {
        fprintf(stderr, "PAGER FAILED: do_one_prefetch left chunk %u in CHUNK_FETCHING\n", idx);
        abort();
    }
    pthread_mutex_unlock(&target->lock);
}

// item 10c: strict priority -- always drain the demand queue first. Only
// when it's empty does a worker look at the prefetch queue.
static void *worker_main(void *argp) {
    prefetch_pool_t *pool = argp;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (pool->demand_len == 0 && pool->pf_len == 0 && !pool->stop)
            pthread_cond_wait(&pool->cv, &pool->lock);
        if (pool->demand_len == 0 && pool->pf_len == 0 && pool->stop) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        if (pool->demand_len > 0) {
            uint32_t idx = pool->demand_queue[pool->demand_head];
            pool->demand_head = (pool->demand_head + 1) % pool->demand_cap;
            pool->demand_len--;
            pthread_mutex_unlock(&pool->lock);

            do_one_demand(pool, idx);
        } else {
            uint32_t idx = pool->pf_queue[pool->pf_head];
            pool->pf_head = (pool->pf_head + 1) % pool->pf_cap;
            pool->pf_len--;
            pthread_mutex_unlock(&pool->lock);

            do_one_prefetch(pool, idx);

            pthread_mutex_lock(&pool->lock);
            pool->pf_outstanding--;
            pool->pf_tracked[idx] = 0;
            pthread_mutex_unlock(&pool->lock);
        }
    }
    return NULL;
}

prefetch_pool_t *prefetch_pool_start(region_t *r, uint32_t n_workers) {
    prefetch_pool_t *pool = calloc(1, sizeof *pool);
    if (!pool) { fprintf(stderr, "prefetch_pool_start: calloc failed\n"); abort(); }
    pool->r = r;
    pool->depth = r->prefetch_depth;
    pool->n_workers = n_workers > 0 ? n_workers : 1;
    pool->workers = calloc(pool->n_workers, sizeof(pthread_t));
    pool->demand_cap = r->n_chunks + 1; // never more than n_chunks distinct chunks can be FETCHING at once
    pool->demand_queue = calloc(pool->demand_cap, sizeof(uint32_t));
    pool->pf_cap = r->n_chunks + 1; // never more than n_chunks distinct chunks can be queued (dedup via pf_tracked)
    pool->pf_queue = calloc(pool->pf_cap, sizeof(uint32_t));
    pool->pf_tracked = calloc(r->n_chunks, sizeof(uint8_t));
    if (!pool->workers || !pool->demand_queue || !pool->pf_queue || !pool->pf_tracked) {
        fprintf(stderr, "prefetch_pool_start: calloc failed\n"); abort();
    }
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cv, NULL);
    pool->stop = 0;
    pool->demand_head = pool->demand_len = 0;
    pool->pf_head = pool->pf_len = pool->pf_outstanding = 0;

    for (uint32_t i = 0; i < pool->n_workers; i++) {
        if (pthread_create(&pool->workers[i], NULL, worker_main, pool) != 0) {
            fprintf(stderr, "prefetch_pool_start: pthread_create failed\n"); abort();
        }
    }
    r->prefetch_pool_handle = pool;
    return pool;
}

void prefetch_pool_enqueue_demand(prefetch_pool_t *pool, uint32_t chunk_idx) {
    pthread_mutex_lock(&pool->lock);
    if (pool->demand_len >= pool->demand_cap) {
        // Should be impossible: at most n_chunks distinct chunks can ever be
        // CHUNK_FETCHING simultaneously, and demand_cap == n_chunks + 1.
        fprintf(stderr,
                "PAGER FAILED: demand queue full (cap=%u) enqueueing chunk %u -- "
                "more distinct FETCHING chunks than n_chunks, should be impossible\n",
                pool->demand_cap, chunk_idx);
        abort();
    }
    pool->demand_queue[(pool->demand_head + pool->demand_len) % pool->demand_cap] = chunk_idx;
    pool->demand_len++;
    pthread_cond_signal(&pool->cv);
    pthread_mutex_unlock(&pool->lock);
}

void prefetch_pool_top_up(region_t *r, chunk_t *just_resident) {
    prefetch_pool_t *pool = r->prefetch_pool_handle;
    if (!pool || !r->policy || !r->policy->predict_next) return;

    pthread_mutex_lock(&pool->lock);
    uint32_t need = (pool->pf_outstanding < pool->depth) ? (pool->depth - pool->pf_outstanding) : 0;
    pthread_mutex_unlock(&pool->lock);
    if (need == 0) return;

    // just_resident is already pinned by the caller (handle_absent /
    // do_one_demand, via commit_reserved_and_pin -- see budget.c) for this
    // exact reason: it still holds just_resident->lock, and layer_order's
    // select_victim tends to pick the just-resident chunk specifically (it
    // looks "unreachable from itself" in the successor walk, i.e.
    // infinitely far -- exactly its preferred victim profile). An earlier
    // version (item 10b) pinned it HERE instead, one step too late -- there
    // was a window between state=RESIDENT and this function's own pin++
    // where a concurrent worker could still select it, producing a flaky
    // (timing-dependent) deadlock at --prefetch-depth 4/8. Not pinning
    // again here; the caller's pin already covers this whole call.
    chunk_t *cursor = just_resident;
    for (uint32_t i = 0; i < need; i++) {
        pthread_mutex_lock(&r->budget_lock);
        int32_t predicted = r->policy->predict_next(r, cursor);
        pthread_mutex_unlock(&r->budget_lock);
        if (predicted < 0) break; // chain doesn't extend further yet
        uint32_t idx = (uint32_t)predicted;
        if (idx >= r->n_chunks) break;
        cursor = &r->chunks[idx];
        if (cursor == just_resident) break; // degenerate loop

        pthread_mutex_lock(&pool->lock);
        bool already = pool->pf_tracked[idx];
        chunk_state_t st = cursor->state; // benign racy read: worst case we enqueue a now-resident chunk, do_one_prefetch drops it harmlessly
        bool backoff = cursor->decline_until_ns > now_ns(); // Defect 3: recently declined -- don't re-enqueue yet
        if (!already && st == CHUNK_ABSENT && !backoff && pool->pf_len < pool->pf_cap) {
            pool->pf_tracked[idx] = 1;
            pool->pf_queue[(pool->pf_head + pool->pf_len) % pool->pf_cap] = idx;
            pool->pf_len++;
            pool->pf_outstanding++;
            pthread_cond_signal(&pool->cv);
        }
        pthread_mutex_unlock(&pool->lock);
    }
    // No unpin here -- the caller owns the pin taken via
    // commit_reserved_and_pin() and releases it via unpin_chunk() after
    // this function returns.
}

void prefetch_pool_stop(prefetch_pool_t *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->cv);
    pthread_mutex_unlock(&pool->lock);
    for (uint32_t i = 0; i < pool->n_workers; i++)
        pthread_join(pool->workers[i], NULL);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cv);
    free(pool->workers);
    free(pool->demand_queue);
    free(pool->pf_queue);
    free(pool->pf_tracked);
    free(pool);
}
