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
// compute_ns_per_mib (Campaign 11 Phase 2): 0 (default) => unchanged from
// every prior item. Non-zero => after reading each chunk's pages, runs a
// real busy computation proportional to bytes just read -- see
// replay_calibrate_compute() above.
//
// item 10d Task A (A-8): this is the N=1 path, UNCHANGED AT compute_ns_per_mib==0, kept as its own
// function specifically so --driver-threads 1 (the default) preserves
// current behaviour byte-for-byte -- not reimplemented as a trivial case of
// replay_cyclic_mt() below, which spawns threads and uses a barrier even at
// N=1 would need special-casing to behave identically.
replay_result_t replay_cyclic(region_t *r, uint32_t n_passes, trace_t *ref_trace, uint64_t compute_ns_per_mib);

// Campaign 11 Phase 2: adds a compute phase to the driver. Default 0 (no
// compute) reproduces prior behaviour byte-for-byte in every existing
// caller. When non-zero, after a thread finishes reading its assigned
// pages for a chunk, it performs a REAL arithmetic loop (not nanosleep --
// a sleeping thread yields the CPU and does not model compute-bound work)
// over the bytes it just read, proportional to compute_ns_per_mib
// nanoseconds per MiB touched. The loop is calibrated: replay_calibrate_compute()
// measures the loop's actual ns/unit-of-work once, and the requested
// ns_per_mib is converted to a unit count via that calibration, not
// assumed. Call once, before the first replay_cyclic()/replay_cyclic_mt()
// call in a process (idempotent -- a second call re-calibrates and
// replaces the stored constant; harmless, just wasted work).
void replay_calibrate_compute(void);

// After a run that used a non-zero compute_ns_per_mib, returns the
// ACHIEVED ns/MiB (measured actual compute wall-time / bytes computed over,
// not the requested value) -- this is what verifies the parameter rather
// than assuming the calibration was accurate. Returns 0.0 if no compute
// was ever performed (compute_ns_per_mib was 0 throughout, or this is
// called before any run).
double replay_compute_achieved_ns_per_mib(void);

// item 10d Task A (A-8), superseded by item 10e Task A (A-10): multi-threaded
// cyclic replay. n_threads threads collectively execute the SAME reference
// sequence as replay_cyclic() above -- partitioned WITHIN each chunk's page
// range (a stride of pages per thread), never across chunks. Not n_threads
// independent workers running ahead on different chunks: this models
// parallel compute over one layer's weights, then advancing to later layers
// -- e.g. llama.cpp's default multi-threaded matmul over one tensor at a
// time, with pipelined overlap into the next layer's weights.
//
// item 10e (A-10): A-8's original hard barrier (window==0 conceptually, but
// literally a pthread_barrier_t -- "all N threads finish chunk i before any
// starts chunk i+1") is REPLACED by a bounded lookahead window: thread t may
// begin chunk i+1 once ALL threads have completed chunk (i - window). At
// most window+1 chunks may be in flight at any moment (see the proof in
// replay.c's comment above lookahead_wait_to_start()). window==0 reproduces
// the old hard-barrier semantic exactly (data-identical: same reference
// trace content, same bytes read, same pager-observable fault/eviction
// counts -- see the item 10e Task A verification gate; NOT claimed to be
// identical in wall-clock scheduling, the same "byte-identical" standard
// item 10d already established when comparing traces across thread counts).
//
// Implemented with a counting mechanism (a per-GLOBAL-STEP completion
// counter, condition-variable-gated), not a weakened/racy approximation of
// the barrier -- see lookahead_wait_to_start()/lookahead_mark_done() in
// replay.c. "Global step" = pass*n_chunks + chunk_index, so the gate is
// well-defined across pass boundaries too (chunk ids alone repeat every
// pass and would be ambiguous).
//
// The reference trace is still emitted ONCE per (pass, chunk) by the
// coordinating thread (tid 0) only, in the same chunk-by-chunk, pass-by-pass
// order as replay_cyclic() -- tid 0 waits for a step's FULL completion
// (all n_threads) before emitting that step's record, and per-thread
// progression is strictly sequential by step, so this is provably in order
// regardless of window size (see replay.c). Total bytes read is identical
// regardless of window: the stride partition covers every page exactly
// once, by exactly one thread, with no overlap -- window only changes how
// much chunk-to-chunk overlap is PERMITTED, never what gets read or the
// order chunks are visited in.
//
// Also fires pager_notify_access() once per (pass, chunk) reference (item
// 10d Task C, A-9's "consumed" signal). consumption_signal_all == 0 (default):
// tid 0 fires it before that step's read, exactly as replay_cyclic() above.
// consumption_signal_all == 1 (FINAL SESSION Phase 2, --consumption-signal
// all-threads): fired instead when the step reaches full completion (all
// n_threads finished it), from whichever thread does the completing
// increment -- the exact "consumed by everyone" signal. Still exactly once
// per (pass,chunk) (asserted).
//
// n_threads must be >= 2 (replay_main.c routes n_threads<=1 to
// replay_cyclic() instead, where "lookahead window" has no meaning -- there
// is only one thread, nothing to overlap with; the two consumption-signal
// modes are trivially identical there since replay_cyclic() is unchanged).
// Caller must have already run region_startup() and started the pager thread.
replay_result_t replay_cyclic_mt(region_t *r, uint32_t n_passes, trace_t *ref_trace,
                                  uint32_t n_threads, uint32_t window, uint64_t compute_ns_per_mib,
                                  int consumption_signal_all);

// Elision-guard accumulator (Defect 2): callers should print this and
// confirm it's nonzero and varies run to run, as one piece of evidence the
// full-chunk read loop actually executed and wasn't optimized away. Shared
// by both replay_cyclic() and replay_cyclic_mt() (the latter accumulates
// into it from multiple threads -- `volatile`, not atomic, since exact
// interleaving doesn't matter here, only "was this loop provably executed,"
// which a nonzero, run-varying value already demonstrates).
uint64_t replay_sink_value(void);

#endif
