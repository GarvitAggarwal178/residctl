// fetch.h -- MECHANISM_SPEC.md §6.1-6.2 fetch path.
#ifndef RESIDCTL_FETCH_H
#define RESIDCTL_FETCH_H

#include "region.h"

// Populates chunk's bytes in the memfd via O_DIRECT (or buffered fallback)
// pread through map_b, looping until the full aligned range is read (short
// reads are legal), then resolves via UFFDIO_CONTINUE and asserts
// c.mapped == chunk->len (I-5). Caller must hold chunk->lock across the
// whole call (I-6). Aborts on any I/O or ioctl failure -- there is no
// partial-success return; a fetch either fully succeeds or the process
// stops, matching "never weaken a test to make it pass."
void fetch_chunk(region_t *r, chunk_t *c);

#endif
