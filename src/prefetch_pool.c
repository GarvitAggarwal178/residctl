// prefetch_pool.c -- see prefetch_pool.h.
#define _GNU_SOURCE
#include "prefetch_pool.h"
#include "policy.h"
#include "budget.h"
#include "fetch.h"
#include "trace.h"
#include "fetch_trace.h"

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
    uint32_t depth;
    pthread_t *workers;
    uint32_t n_workers;

    pthread_mutex_t lock;
    pthread_cond_t cv;
    int stop;

    uint32_t *queue;   // ring buffer of pending chunk indices
    uint32_t queue_cap;
    uint32_t queue_head, queue_len;

    uint32_t outstanding; // queued + in-flight, not yet resident/dropped
    uint8_t *tracked;     // per-chunk: 1 if queued or in-flight (dedupe), size n_chunks
};

// Fetches one chunk on behalf of the pool. Mirrors prefetch.c's
// maybe_prefetch() body (item 8) but takes an explicit target index
// instead of computing one via predict_next -- the caller
// (prefetch_pool_top_up) already did that part of the chain walk.
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
    target->pin++; // I-4: never punched while this prefetch targets it
    target->state = CHUNK_FETCHING;
    uint64_t seq = __sync_add_and_fetch(&r->fault_seq, 1); // atomic: shared across workers + handler
    target->last_fault_seq = seq;
    if (r->trace)
        trace_record(r->trace, seq, idx, TRACE_FAULT_MISSING, 1 /* was_prefetched */);

    int budget_rc = ensure_budget(r, target->len); // may itself select/evict a victim; never forces a pinned one (I-4)

    if (budget_rc != 0) {
        // Infeasible: abandon this prefetch, not fatal -- exactly Task B's
        // "a prefetch that would force eviction of a pinned chunk is
        // dropped, not forced" (ensure_budget's infeasible return already
        // means no evictable, unpinned victim existed).
        target->state = CHUNK_ABSENT;
        target->pin--;
        r->stat_prefetch_infeasible++;
        pthread_mutex_unlock(&target->lock);
        return;
    }

    fetch_timing_t timing;
    fetch_chunk(r, target, r->diag_fetch_trace ? &timing : NULL);
    // Commit before marking RESIDENT -- see pager.c's note. This ordering
    // is what actually matters here: with a real worker pool, another
    // worker's ensure_budget() genuinely can run concurrently and would
    // otherwise be able to select this chunk as its victim while this
    // thread still holds target->lock and is about to need budget_lock via
    // commit_reserved -- a real, reproduced deadlock at --prefetch-depth 8,
    // not a hypothetical.
    commit_reserved(r, target->len);
    target->state = CHUNK_RESIDENT;
    r->stat_bytes_fetched += target->len;
    target->pin--;
    r->stat_prefetches++;
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

    pthread_mutex_unlock(&target->lock);
}

static void *worker_main(void *argp) {
    prefetch_pool_t *pool = argp;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (pool->queue_len == 0 && !pool->stop)
            pthread_cond_wait(&pool->cv, &pool->lock);
        if (pool->queue_len == 0 && pool->stop) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        uint32_t idx = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_cap;
        pool->queue_len--;
        pthread_mutex_unlock(&pool->lock);

        do_one_prefetch(pool, idx);

        pthread_mutex_lock(&pool->lock);
        pool->outstanding--;
        pool->tracked[idx] = 0;
        pthread_mutex_unlock(&pool->lock);
    }
    return NULL;
}

prefetch_pool_t *prefetch_pool_start(region_t *r) {
    prefetch_pool_t *pool = calloc(1, sizeof *pool);
    if (!pool) { fprintf(stderr, "prefetch_pool_start: calloc failed\n"); abort(); }
    pool->r = r;
    pool->depth = r->prefetch_depth;
    pool->n_workers = r->prefetch_depth; // one worker per unit of depth: up to `depth` genuinely concurrent fetches
    pool->workers = calloc(pool->n_workers, sizeof(pthread_t));
    pool->queue_cap = r->n_chunks + 1; // never more than n_chunks distinct chunks can be queued (dedup via `tracked`)
    pool->queue = calloc(pool->queue_cap, sizeof(uint32_t));
    pool->tracked = calloc(r->n_chunks, sizeof(uint8_t));
    if (!pool->workers || !pool->queue || !pool->tracked) {
        fprintf(stderr, "prefetch_pool_start: calloc failed\n"); abort();
    }
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cv, NULL);
    pool->stop = 0;
    pool->queue_head = pool->queue_len = pool->outstanding = 0;

    for (uint32_t i = 0; i < pool->n_workers; i++) {
        if (pthread_create(&pool->workers[i], NULL, worker_main, pool) != 0) {
            fprintf(stderr, "prefetch_pool_start: pthread_create failed\n"); abort();
        }
    }
    r->prefetch_pool_handle = pool;
    return pool;
}

void prefetch_pool_top_up(region_t *r, chunk_t *just_resident) {
    prefetch_pool_t *pool = r->prefetch_pool_handle;
    if (!pool || !r->policy || !r->policy->predict_next) return;

    pthread_mutex_lock(&pool->lock);
    uint32_t need = (pool->outstanding < pool->depth) ? (pool->depth - pool->outstanding) : 0;
    pthread_mutex_unlock(&pool->lock);
    if (need == 0) return;

    // just_resident is already pinned by the caller (handle_absent, via
    // commit_reserved_and_pin -- see pager.c and budget.c) for this exact
    // reason: it still holds just_resident->lock, and layer_order's
    // select_victim tends to pick the just-resident chunk specifically (it
    // looks "unreachable from itself" in the successor walk, i.e.
    // infinitely far -- exactly its preferred victim profile). An earlier
    // version pinned it HERE instead, one step too late -- there was a
    // window between state=RESIDENT and this function's own pin++ where a
    // concurrent worker could still select it, producing a flaky
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
        bool already = pool->tracked[idx];
        chunk_state_t st = cursor->state; // benign racy read: worst case we enqueue a now-resident chunk, do_one_prefetch drops it harmlessly
        if (!already && st == CHUNK_ABSENT && pool->queue_len < pool->queue_cap) {
            pool->tracked[idx] = 1;
            pool->queue[(pool->queue_head + pool->queue_len) % pool->queue_cap] = idx;
            pool->queue_len++;
            pool->outstanding++;
            pthread_cond_signal(&pool->cv);
        }
        pthread_mutex_unlock(&pool->lock);
    }
    // No unpin here -- the caller (handle_absent) owns the pin taken via
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
    free(pool->queue);
    free(pool->tracked);
    free(pool);
}
