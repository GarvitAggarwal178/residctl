// replay.c -- see replay.h. Implements Defects 1 and 2's fixes, and item
// 10d Task A's multi-threaded driver (A-8).
#define _GNU_SOURCE
#include "replay.h"
#include "pager.h"

#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Elision guard: accumulated into across the whole run and returned to the
// caller (replay_result_t doesn't carry it, but it's printed by callers via
// replay_sink_value() below) so nothing -- not even whole-program
// optimization -- can prove this value is unobserved and delete the read
// loop. `volatile` forces every read of map_a[...] and every update to
// actually execute; see CLAUDE.md's item 10 correction note for the
// objdump/timing verification that this actually works.
static volatile uint64_t g_replay_sink = 0;

uint64_t replay_sink_value(void) { return g_replay_sink; }

replay_result_t replay_cyclic(region_t *r, uint32_t n_passes, trace_t *ref_trace) {
    uint64_t t_start = now_ns();
    uint64_t ref_seq = 0;
    uint64_t bytes_touched = 0;

    for (uint32_t pass = 0; pass < n_passes; pass++) {
        for (uint32_t i = 0; i < r->n_chunks; i++) {
            chunk_t *c = &r->chunks[i];

            // item 10d Task C (A-9): "consumed" signal, fired right before
            // the read below, regardless of hit/miss -- see pager.h.
            pager_notify_access(r, c);

            // Defect 2: consume the FULL chunk, not one byte. The first
            // page touched may fault (MISSING/MINOR); UFFDIO_CONTINUE
            // resolves the whole chunk in one call (confirmed by the
            // spike's S2), so subsequent pages in this same loop are plain
            // memory reads, not faults -- matching how a real consumer
            // (e.g. reading every weight in a transformer layer) behaves:
            // one fault-or-not event per chunk, but full-chunk bytes moved
            // either way.
            for (uint64_t off = 0; off < c->len; off += RESIDCTL_ALIGN) {
                g_replay_sink += r->map_a[c->region_off + off];
            }
            bytes_touched += c->len;

            // Defect 1: the WORKLOAD emits the ground-truth reference
            // trace -- every access, in order, regardless of whether the
            // pager happened to fault on it. fault_type/was_prefetched are
            // TRACE_NA: a reference isn't a fault, and this driver doesn't
            // know or care whether the pager missed on it.
            if (ref_trace)
                trace_record(ref_trace, ++ref_seq, i, TRACE_NA, TRACE_NA);
        }
    }

    uint64_t t_end = now_ns(); // actual replay wall time, excludes the settle wait below

    // Settle wait: chunk->state/resident_bytes/trace records for the LAST
    // touch are updated by the pager thread a short time after the
    // CONTINUE that unblocked this thread's read -- give it a moment
    // before returning so callers inspecting region_t state right after
    // this call see it fully caught up. Does not affect wall_ns above.
    usleep(20 * 1000);

    replay_result_t res;
    res.wall_ns = t_end - t_start;
    res.n_passes = n_passes;
    res.n_touches = n_passes * r->n_chunks;
    res.bytes_touched = bytes_touched;
    return res;
}

// ---- item 10d Task A (A-8), superseded by item 10e Task A (A-10) --------
// multi-threaded replay: bounded lookahead window instead of a hard barrier.

// Global-step completion tracking, shared by all n_threads driver threads.
// "Step" = pass*n_chunks + chunk_index -- a single monotonic counter across
// the whole run, so the gate below is unambiguous across pass boundaries
// (chunk INDEX alone repeats every pass and can't distinguish "chunk 3 in
// pass 0" from "chunk 3 in pass 1").
typedef struct {
    uint32_t *completed;   // completed[s] = how many of n_threads have finished step s
    pthread_mutex_t lock;
    pthread_cond_t cv;     // broadcast whenever any completed[] entry changes
    uint32_t n_threads;
    uint32_t window;
} lookahead_state_t;

// Blocks the calling thread until it may BEGIN step s. Per §11 Amendment
// A-10: "thread t may begin chunk i+1 once all threads have completed chunk
// i-W" -- restated in global-step terms, beginning step s requires step
// (s - window - 1) to be FULLY complete (every thread), if that step exists
// at all. The first (window+1) steps of the whole run have no such
// predecessor and never wait.
//
// Proof this bounds "at most window+1 chunks in flight" (not just asserted):
// each thread's own progression through steps is strictly sequential (a
// thread must finish its assigned pages for step s, via the plain for-loop
// below, before it can even attempt step s+1) -- so if step s reaches full
// completion (completed[s]==n_threads), EVERY thread individually passed
// through step s already, meaning no step earlier than s can still be
// incomplete once s is complete. That makes "the earliest not-yet-fully-
// complete step" (call it s_min) monotonically non-decreasing over the
// whole run. A thread can only be admitted to start step s' when
// completed[s'-window-1]==n_threads, i.e. s'-window-1 < s_min, i.e.
// s' <= s_min+window. Combined with s_min itself still being in progress,
// the set of steps any thread could possibly be working on right now is
// {s_min, ..., s_min+window} -- exactly window+1 distinct chunks, never more.
static void lookahead_wait_to_start(lookahead_state_t *st, int64_t s) {
    int64_t gate = s - (int64_t)st->window - 1;
    if (gate < 0) return; // nothing to wait for yet
    pthread_mutex_lock(&st->lock);
    while (st->completed[gate] < st->n_threads)
        pthread_cond_wait(&st->cv, &st->lock);
    pthread_mutex_unlock(&st->lock);
}

// Records that the calling thread finished its assigned pages for step s.
// Broadcasts once this step reaches full completion (all n_threads) -- the
// only event any waiter (lookahead_wait_to_start or the tid-0-only wait
// below) actually cares about.
static void lookahead_mark_done(lookahead_state_t *st, uint32_t s) {
    pthread_mutex_lock(&st->lock);
    st->completed[s]++;
    if (st->completed[s] == st->n_threads)
        pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->lock);
}

// tid 0 only. The reference trace must still be emitted "once per (pass,
// chunk) by the coordinating thread, in sequence order" (unchanged
// requirement from A-8) -- window>0 lets OTHER threads race ahead onto
// later steps before step s is fully done, so tid 0 explicitly waits for
// step s's full completion here before recording it, rather than assuming
// its own completion implies everyone else's. This does not gate anyone
// else's progress (only tid 0 blocks here); the proof above already shows
// full completion of step s happens before full completion of step s+1
// regardless, so tid 0's records come out in strict order by construction.
static void lookahead_wait_full(lookahead_state_t *st, uint32_t s) {
    pthread_mutex_lock(&st->lock);
    while (st->completed[s] < st->n_threads)
        pthread_cond_wait(&st->cv, &st->lock);
    pthread_mutex_unlock(&st->lock);
}

typedef struct {
    region_t *r;
    uint32_t tid;
    uint32_t n_threads;
    uint32_t n_passes;
    trace_t *ref_trace;
    lookahead_state_t *la;
    uint64_t bytes_touched; // written only by tid 0
    uint64_t ref_seq;       // written only by tid 0
} replay_thread_ctx_t;

static void *replay_thread_main(void *argp) {
    replay_thread_ctx_t *ctx = argp;
    region_t *r = ctx->r;
    uint32_t tid = ctx->tid, nt = ctx->n_threads;
    uint32_t n_chunks = r->n_chunks;
    uint32_t total_steps = ctx->n_passes * n_chunks;

    for (uint32_t s = 0; s < total_steps; s++) {
        uint32_t i = s % n_chunks; // chunk index; s alone (not i) is the gate key -- see lookahead_state_t's comment
        chunk_t *c = &r->chunks[i];

        lookahead_wait_to_start(ctx->la, (int64_t)s); // A-10: bounded lookahead, replaces the old hard barrier

        // "Consumed" signal (A-9) -- fired once per (pass, chunk) by the
        // coordinator only, same timing as before (right before the read).
        if (tid == 0) pager_notify_access(r, c);

        // Partition WITHIN this chunk's page range: thread `tid` takes
        // every nt-th page, starting at page tid. Covers every page
        // exactly once across all nt threads, no overlap, regardless of
        // n_threads or window -- this is what makes total bytes read
        // identical across both. Same per-page access pattern as
        // replay_cyclic() (one byte per 4096-byte page, into the volatile
        // sink) -- g_replay_sink is written by up to n_threads threads
        // concurrently here, so use an atomic add rather than the plain
        // `+=` the single-threaded path uses (a genuine data race under
        // real concurrency, not merely hypothetical).
        uint64_t page_idx = 0;
        for (uint64_t off = 0; off < c->len; off += RESIDCTL_ALIGN, page_idx++) {
            if ((page_idx % nt) == tid) {
                __sync_fetch_and_add(&g_replay_sink, r->map_a[c->region_off + off]);
            }
        }

        lookahead_mark_done(ctx->la, s);

        if (tid == 0) {
            lookahead_wait_full(ctx->la, s); // A-10: strict in-order emission, see lookahead_wait_full()'s comment
            ctx->bytes_touched += c->len;
            if (ctx->ref_trace)
                trace_record(ctx->ref_trace, ++ctx->ref_seq, i, TRACE_NA, TRACE_NA);
        }
    }
    return NULL;
}

replay_result_t replay_cyclic_mt(region_t *r, uint32_t n_passes, trace_t *ref_trace,
                                  uint32_t n_threads, uint32_t window) {
    uint64_t t_start = now_ns();

    uint32_t total_steps = n_passes * r->n_chunks;
    lookahead_state_t la;
    la.completed = calloc(total_steps, sizeof(uint32_t));
    if (!la.completed) { fprintf(stderr, "replay_cyclic_mt: calloc failed\n"); abort(); }
    pthread_mutex_init(&la.lock, NULL);
    pthread_cond_init(&la.cv, NULL);
    la.n_threads = n_threads;
    la.window = window;

    pthread_t *tids = calloc(n_threads, sizeof(pthread_t));
    replay_thread_ctx_t *ctxs = calloc(n_threads, sizeof(replay_thread_ctx_t));
    if (!tids || !ctxs) { fprintf(stderr, "replay_cyclic_mt: calloc failed\n"); abort(); }

    for (uint32_t t = 0; t < n_threads; t++) {
        ctxs[t].r = r;
        ctxs[t].tid = t;
        ctxs[t].n_threads = n_threads;
        ctxs[t].n_passes = n_passes;
        ctxs[t].ref_trace = ref_trace; // only tid 0 ever writes through this
        ctxs[t].la = &la;
        ctxs[t].bytes_touched = 0;
        ctxs[t].ref_seq = 0;
    }

    for (uint32_t t = 1; t < n_threads; t++)
        pthread_create(&tids[t], NULL, replay_thread_main, &ctxs[t]);
    replay_thread_main(&ctxs[0]); // run tid 0 on the calling thread itself
    for (uint32_t t = 1; t < n_threads; t++)
        pthread_join(tids[t], NULL);

    pthread_mutex_destroy(&la.lock);
    pthread_cond_destroy(&la.cv);
    free(la.completed);

    uint64_t t_end = now_ns();
    usleep(20 * 1000); // same settle wait as replay_cyclic()

    replay_result_t res;
    res.wall_ns = t_end - t_start;
    res.n_passes = n_passes;
    res.n_touches = n_passes * r->n_chunks; // one reference per (pass, chunk), same definition regardless of thread count/window
    res.bytes_touched = ctxs[0].bytes_touched;
    free(tids);
    free(ctxs);
    return res;
}
