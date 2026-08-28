// prefetch.c -- see prefetch.h for the "inline, not a separate thread" design note.
#define _GNU_SOURCE
#include "prefetch.h"
#include "policy.h"
#include "pager.h"    // CLEANUP session: pager_abandon_fetch()
#include "budget.h"
#include "fetch.h"
#include "trace.h"
#include "fetch_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void maybe_prefetch(region_t *r, chunk_t *just_resident) {
    if (!r->policy || !r->policy->predict_next) return;

    int32_t predicted = r->policy->predict_next(r, just_resident);
    if (predicted < 0) return;
    uint32_t idx = (uint32_t)predicted;
    if (idx >= r->n_chunks) {
        fprintf(stderr, "PREFETCH FAILED: policy predicted out-of-range chunk %u (n_chunks=%u)\n",
                idx, r->n_chunks);
        abort();
    }

    chunk_t *target = &r->chunks[idx];
    if (target == just_resident) return; // degenerate self-prediction, nothing to do

    pthread_mutex_lock(&target->lock);
    if (target->state != CHUNK_ABSENT) {
        // Already resident or being fetched by something else (there is no
        // "something else" today since the handler is single-threaded, but
        // this check costs nothing and stays correct if that ever changes).
        pthread_mutex_unlock(&target->lock);
        return;
    }

    uint64_t t_start = now_ns();
    target->pin++; // I-4: never punched while a prefetch targets it
    target->state = CHUNK_FETCHING;
    target->fetching_since_ns = t_start; // CLEANUP session: FETCHING watchdog clock
    uint64_t seq = __sync_add_and_fetch(&r->fault_seq, 1); // Task B: atomic, see pager.c's note
    target->last_fault_seq = seq;
    if (r->trace)
        trace_record(r->trace, seq, idx, TRACE_FAULT_MISSING, 1 /* was_prefetched */);

    // Pin just_resident too, for this call only: its own lock is held by
    // our caller (handle_absent, I-6), but a policy's select_victim() can
    // legitimately pick it as the best victim -- e.g. layer_order sees it
    // as "unreachable from itself" in the successor walk, i.e. infinitely
    // far, exactly the profile of its preferred victim. Without this,
    // evict_chunk() would try to lock a mutex this thread already holds.
    just_resident->pin++;
    // item 10c Task B (A-6): gated the same way as prefetch_pool.c's
    // do_one_prefetch -- see that file's note. This path is only reachable
    // under --sync-handler at --prefetch-depth 1 (item 10c's async handler
    // routes every prefetch, at every depth, through prefetch_pool.c
    // instead), but it's the same "prefetch, not demand" call, so it gets
    // the same gate for consistency.
    int budget_rc = ensure_budget_prefetch(r, target);
    just_resident->pin--;

    if (budget_rc != 0) {
        // Infeasible: abandon this prefetch, not fatal (§7/§11 censoring
        // logic already ran inside ensure_budget). CLEANUP session (part a):
        // reset FETCHING + UFFDIO_WAKE via pager_abandon_fetch so any faulter
        // that deduped against this window is not left blocked (matters only
        // when a fetch pool runs under --sync-handler at depth>1; harmless
        // otherwise).
        target->pin--;
        pager_abandon_fetch(r, target); // -> ABSENT, fetching_since_ns=0, WAKE
        r->stat_prefetch_infeasible++;
        pthread_mutex_unlock(&target->lock);
        return;
    }

    fetch_timing_t timing;
    fetch_chunk(r, target, r->diag_fetch_trace ? &timing : NULL);
    // Commit before marking RESIDENT -- see the matching note in pager.c
    // (found via a real deadlock at --prefetch-depth 8). Not load-bearing
    // at depth==1 (only one thread exists here), kept for consistency with
    // the other two call sites so all three reason about it the same way.
    commit_reserved(r, target->len); // moves the ensure_budget() reservation into resident_bytes
    target->state = CHUNK_RESIDENT;
    target->fetching_since_ns = 0; // CLEANUP session: left FETCHING
    r->stat_bytes_fetched += target->len; // Defect 3: pager's own byte accounting
    // item 10d Task C (A-9): mirrors prefetch_pool.c's do_one_prefetch --
    // under --prefetch-retention pinned (default), keep target pinned via
    // the retention FIFO instead of unpinning immediately.
    if (r->prefetch_retention_pinned) {
        pthread_mutex_lock(&r->budget_lock);
        prefetch_retain_on_resident(r, target);
        pthread_mutex_unlock(&r->budget_lock);
    } else {
        target->pin--;
    }
    r->stat_prefetches++;
    if (r->policy->on_resident) r->policy->on_resident(r, target);
    // No recursive maybe_prefetch() here -- "one outstanding prefetch maximum."

    uint64_t t_exit = now_ns();
    if (r->diag_fetch_trace) {
        fetch_trace_record_t *rec = fetch_trace_reserve(r->diag_fetch_trace);
        rec->chunk_id = idx;
        rec->was_prefetch = 1;
        rec->t_handler_entry_ns = t_start;
        rec->t_read_start_ns = timing.read_start_ns;
        rec->t_read_end_ns = timing.read_end_ns;
        rec->t_continue_end_ns = timing.continue_end_ns;
        rec->t_handler_exit_ns = t_exit;
        rec->bytes_read = target->len;
    }

    pthread_mutex_unlock(&target->lock);
}
