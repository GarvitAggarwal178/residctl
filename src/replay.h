// replay.h -- build-order item 6: the trace-replay driver.
//
// SPEC CORRECTION (item 10, Defects 1 and 2):
//  - Defect 1: replay_cyclic() now optionally writes the GROUND-TRUTH
//    reference trace (TRACE_TYPE_REFERENCE) itself, since it is the
//    workload generating the access sequence. The pager's own fault trace
//    (region_t.trace, TRACE_TYPE_FAULT) is unaffected and still records
//    misses only -- it is metrics-only now, never solver input. See
//    trace.h and belady_main.c.
//  - Defect 2: every reference now touches every 4096-byte page of the
//    chunk (full consumption), not just its first byte, matching what a
//    real consumer (e.g. a transformer reading a layer's weights) actually
//    does. See replay.c for the elision-guard implementation.
#ifndef RESIDCTL_REPLAY_H
#define RESIDCTL_REPLAY_H

#include "region.h"
#include "trace.h"

typedef struct {
    uint64_t wall_ns;
    uint32_t n_passes;
    uint32_t n_touches;    // n_passes * n_chunks (references, not bytes)
    uint64_t bytes_touched; // total bytes actually read (sum of c->len per reference)
} replay_result_t;

// Cyclic layer-order replay: n_passes full sweeps over chunks
// 0..n_chunks-1. Each reference reads every page of the chunk (Defect 2)
// and, if ref_trace is non-NULL, appends one TRACE_TYPE_REFERENCE record
// per reference (Defect 1) -- ref_trace must have been opened with
// trace_open(path, TRACE_TYPE_REFERENCE).
//
// This is the "known cyclic order" access pattern §8's layer_order policy
// and §9's Belady solver are built around -- LLM inference traverses
// layers in the same fixed order every token/pass. Until build-order item
// 11 (llama.cpp integration) exists, this synthetic pattern IS the
// trace-replay driver's primary workload, per §12.
//
// Caller must have already run region_startup() and started the pager
// thread; this function only issues the touches and times them.
//
// item 10d Task A (A-8): this is the N=1 path, UNCHANGED, kept as its own
// function specifically so --driver-threads 1 (the default) preserves
// current behaviour byte-for-byte -- not reimplemented as a trivial case of
// replay_cyclic_mt() below, which spawns threads and uses a barrier even at
// N=1 would need special-casing to behave identically.
replay_result_t replay_cyclic(region_t *r, uint32_t n_passes, trace_t *ref_trace);

// item 10d Task A (A-8): multi-threaded cyclic replay. n_threads threads
// collectively execute the SAME reference sequence as replay_cyclic() above
// -- partitioned WITHIN each chunk's page range (a stride of pages per
// thread), never across chunks, with a barrier at every chunk boundary so
// all n_threads threads finish chunk i before any of them starts chunk i+1.
// This models parallel compute over one layer's weights, then advancing to
// the next layer together -- e.g. llama.cpp's default multi-threaded matmul
// over one tensor at a time -- not n_threads independent workers running
// ahead on different chunks.
//
// The reference trace is emitted ONCE per (pass, chunk) by the coordinating
// thread (tid 0) only, in the same chunk-by-chunk order as replay_cyclic(),
// so the emitted TRACE_TYPE_REFERENCE sequence (chunk_id per record, in
// order) is IDENTICAL regardless of n_threads -- verified by the item 10d
// Task A gate (trace/byte/OPT identity at n_threads in {1,8}). Total bytes
// read is also identical: the stride partition covers every page exactly
// once, by exactly one thread, with no overlap.
//
// Also fires pager_notify_access() once per (pass, chunk) reference (Task
// C, A-9's "consumed" signal) -- same call, same point in the loop, as
// replay_cyclic() above, so this doesn't change Task A's identity gate
// (notify_access only touches internal pin/retention bookkeeping, never
// g_replay_sink, bytes_touched, or the reference trace).
//
// n_threads must be >= 2 (replay_main.c routes n_threads<=1 to
// replay_cyclic() instead). Caller must have already run region_startup()
// and started the pager thread.
replay_result_t replay_cyclic_mt(region_t *r, uint32_t n_passes, trace_t *ref_trace, uint32_t n_threads);

// Elision-guard accumulator (Defect 2): callers should print this and
// confirm it's nonzero and varies run to run, as one piece of evidence the
// full-chunk read loop actually executed and wasn't optimized away. Shared
// by both replay_cyclic() and replay_cyclic_mt() (the latter accumulates
// into it from multiple threads -- `volatile`, not atomic, since exact
// interleaving doesn't matter here, only "was this loop provably executed,"
// which a nonzero, run-varying value already demonstrates).
uint64_t replay_sink_value(void);

#endif
