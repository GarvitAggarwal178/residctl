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

// ---- item 10d Task A (A-8): multi-threaded replay ------------------------

typedef struct {
    region_t *r;
    uint32_t tid;
    uint32_t n_threads;
    uint32_t n_passes;
    trace_t *ref_trace;
    pthread_barrier_t *barrier;
    uint64_t bytes_touched; // written only by tid 0
    uint64_t ref_seq;       // written only by tid 0
} replay_thread_ctx_t;

static void *replay_thread_main(void *argp) {
    replay_thread_ctx_t *ctx = argp;
    region_t *r = ctx->r;
    uint32_t tid = ctx->tid, nt = ctx->n_threads;

    for (uint32_t pass = 0; pass < ctx->n_passes; pass++) {
        for (uint32_t i = 0; i < r->n_chunks; i++) {
            chunk_t *c = &r->chunks[i];

            // "Consumed" signal (A-9) -- fired once per (pass, chunk) by the
            // coordinator only, same as replay_cyclic() above. Independent
            // of the read below; doesn't need to happen-before it (it only
            // touches internal pin bookkeeping, not the data being read).
            if (tid == 0) pager_notify_access(r, c);

            // Partition WITHIN this chunk's page range: thread `tid` takes
            // every nt-th page, starting at page tid. Covers every page
            // exactly once across all nt threads, no overlap, regardless of
            // n_threads -- this is what makes total bytes read identical to
            // the N=1 path. Same per-page access pattern as replay_cyclic()
            // (one byte per 4096-byte page, into the volatile sink) --
            // g_replay_sink is written by up to n_threads threads
            // concurrently here, so use an atomic add rather than the
            // plain `+=` the single-threaded path uses (which would be a
            // genuine data race under real concurrency, not merely
            // hypothetical -- see CLAUDE.md's item 10c pattern for why this
            // project fixes this class of bug on sight rather than
            // deferring it).
            uint64_t page_idx = 0;
            for (uint64_t off = 0; off < c->len; off += RESIDCTL_ALIGN, page_idx++) {
                if ((page_idx % nt) == tid) {
                    __sync_fetch_and_add(&g_replay_sink, r->map_a[c->region_off + off]);
                }
            }

            // Barrier: all nt threads have finished THIS chunk's reads
            // before any of them starts the next one ("then all advance to
            // the next chunk together via a barrier" -- models parallel
            // compute over one layer, then moving to the next layer
            // together).
            pthread_barrier_wait(ctx->barrier);

            if (tid == 0) {
                ctx->bytes_touched += c->len;
                if (ctx->ref_trace)
                    trace_record(ctx->ref_trace, ++ctx->ref_seq, i, TRACE_NA, TRACE_NA);
            }

            // Second barrier: keeps the coordinator's bookkeeping (byte
            // count, reference-trace record) for chunk i strictly ordered
            // before any thread's reads for chunk i+1 begin -- removes any
            // ambiguity about ordering, even though nothing above actually
            // depends on it for correctness (only tid 0 ever touches
            // bytes_touched/ref_seq/ref_trace).
            pthread_barrier_wait(ctx->barrier);
        }
    }
    return NULL;
}

replay_result_t replay_cyclic_mt(region_t *r, uint32_t n_passes, trace_t *ref_trace, uint32_t n_threads) {
    uint64_t t_start = now_ns();

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, n_threads);

    pthread_t *tids = calloc(n_threads, sizeof(pthread_t));
    replay_thread_ctx_t *ctxs = calloc(n_threads, sizeof(replay_thread_ctx_t));
    if (!tids || !ctxs) { fprintf(stderr, "replay_cyclic_mt: calloc failed\n"); abort(); }

    for (uint32_t t = 0; t < n_threads; t++) {
        ctxs[t].r = r;
        ctxs[t].tid = t;
        ctxs[t].n_threads = n_threads;
        ctxs[t].n_passes = n_passes;
        ctxs[t].ref_trace = ref_trace; // only tid 0 ever writes through this
        ctxs[t].barrier = &barrier;
        ctxs[t].bytes_touched = 0;
        ctxs[t].ref_seq = 0;
    }

    for (uint32_t t = 1; t < n_threads; t++)
        pthread_create(&tids[t], NULL, replay_thread_main, &ctxs[t]);
    replay_thread_main(&ctxs[0]); // run tid 0 on the calling thread itself
    for (uint32_t t = 1; t < n_threads; t++)
        pthread_join(tids[t], NULL);

    pthread_barrier_destroy(&barrier);

    uint64_t t_end = now_ns();
    usleep(20 * 1000); // same settle wait as replay_cyclic()

    replay_result_t res;
    res.wall_ns = t_end - t_start;
    res.n_passes = n_passes;
    res.n_touches = n_passes * r->n_chunks; // one reference per (pass, chunk), same definition regardless of thread count
    res.bytes_touched = ctxs[0].bytes_touched;
    free(tids);
    free(ctxs);
    return res;
}
