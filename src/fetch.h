// fetch.h -- MECHANISM_SPEC.md §6.1-6.2 fetch path.
#ifndef RESIDCTL_FETCH_H
#define RESIDCTL_FETCH_H

#include "region.h"
#include <stdint.h>

// Item 10b Task A: optional timing capture, filled in by fetch_chunk() when
// out_timing is non-NULL. CLOCK_MONOTONIC throughout. Costs one branch and
// two clock_gettime() calls extra when active; zero overhead when NULL.
typedef struct {
    uint64_t read_start_ns;
    uint64_t read_end_ns;
    uint64_t continue_end_ns;
} fetch_timing_t;

// Populates chunk's bytes in the memfd via O_DIRECT (or buffered fallback)
// pread through map_b, looping until the full aligned range is read (short
// reads are legal), then resolves via UFFDIO_CONTINUE and asserts
// c.mapped == chunk->len (I-5). Caller must hold chunk->lock across the
// whole call (I-6). Aborts on any I/O or ioctl failure -- there is no
// partial-success return; a fetch either fully succeeds or the process
// stops, matching "never weaken a test to make it pass."
void fetch_chunk(region_t *r, chunk_t *c, fetch_timing_t *out_timing);

#endif
