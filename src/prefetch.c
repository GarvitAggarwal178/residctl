// prefetch.c -- see prefetch.h for the "inline, not a separate thread" design note.
#define _GNU_SOURCE
#include "prefetch.h"
#include "policy.h"
#include "budget.h"
#include "fetch.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>

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

    target->pin++; // I-4: never punched while a prefetch targets it
    target->state = CHUNK_FETCHING;
    target->last_fault_seq = ++r->fault_seq;
    if (r->trace)
        trace_record(r->trace, r->fault_seq, idx, TRACE_FAULT_MISSING, 1 /* was_prefetched */);

    // Pin just_resident too, for this call only: its own lock is held by
    // our caller (handle_absent, I-6), but a policy's select_victim() can
    // legitimately pick it as the best victim -- e.g. layer_order sees it
    // as "unreachable from itself" in the successor walk, i.e. infinitely
    // far, exactly the profile of its preferred victim. Without this,
    // evict_chunk() would try to lock a mutex this thread already holds.
    just_resident->pin++;
    int budget_rc = ensure_budget(r, target->len);
    just_resident->pin--;

    if (budget_rc != 0) {
        // Infeasible: abandon this prefetch, not fatal (§7/§11 censoring
        // logic already ran inside ensure_budget). The chunk stays ABSENT
        // and will be fetched normally on a real fault later.
        target->state = CHUNK_ABSENT;
        target->pin--;
        r->stat_prefetch_infeasible++;
        pthread_mutex_unlock(&target->lock);
        return;
    }

    fetch_chunk(r, target);
    target->state = CHUNK_RESIDENT;
    r->resident_bytes += target->len;
    target->pin--;
    r->stat_prefetches++;
    if (r->policy->on_resident) r->policy->on_resident(r, target);
    // No recursive maybe_prefetch() here -- "one outstanding prefetch maximum."
    pthread_mutex_unlock(&target->lock);
}
