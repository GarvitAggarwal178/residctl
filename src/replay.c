// replay.c -- see replay.h. Implements Defects 1 and 2's fixes.
#define _GNU_SOURCE
#include "replay.h"

#include <time.h>
#include <unistd.h>
#include <stdio.h>

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
