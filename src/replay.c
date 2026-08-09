// replay.c -- see replay.h.
#define _GNU_SOURCE
#include "replay.h"

#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

replay_result_t replay_cyclic(region_t *r, uint32_t n_passes) {
    uint64_t t_start = now_ns();

    for (uint32_t pass = 0; pass < n_passes; pass++) {
        for (uint32_t i = 0; i < r->n_chunks; i++) {
            chunk_t *c = &r->chunks[i];
            // A MISSING/MINOR fault here blocks this thread at the hardware
            // level until the pager thread's UFFDIO_CONTINUE/WAKE resolves
            // it -- no polling or sleep needed for the read itself to be
            // correct. (Our own chunk->state bookkeeping update in
            // handle_absent happens a few instructions AFTER the CONTINUE
            // that wakes us, so it can lag this read by a small amount;
            // see the settle wait below.)
            volatile uint8_t x = r->map_a[c->region_off];
            (void)x;
        }
    }

    uint64_t t_end = now_ns(); // the actual replay wall time, excludes the settle wait below

    // Settle wait: chunk->state / resident_bytes / trace records for the
    // LAST touch are updated by the pager thread a short time after the
    // CONTINUE that unblocked this thread's read -- give it a moment before
    // returning so callers inspecting region_t state right after this call
    // see it fully caught up. Does not affect the wall_ns measurement above.
    usleep(20 * 1000);

    replay_result_t res;
    res.wall_ns = t_end - t_start;
    res.n_passes = n_passes;
    res.n_touches = n_passes * r->n_chunks;
    return res;
}
