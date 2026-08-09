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
replay_result_t replay_cyclic(region_t *r, uint32_t n_passes, trace_t *ref_trace);

// Elision-guard accumulator (Defect 2): callers should print this and
// confirm it's nonzero and varies run to run, as one piece of evidence the
// full-chunk read loop actually executed and wasn't optimized away.
uint64_t replay_sink_value(void);

#endif
