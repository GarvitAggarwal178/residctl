// pager.h -- MECHANISM_SPEC.md §5 handler loop.
#ifndef RESIDCTL_PAGER_H
#define RESIDCTL_PAGER_H

#include "region.h"
#include <signal.h>

// Runs the §5 handler loop until *stop becomes nonzero. Intended to be the
// body of the handler thread (§4 step 10, which region_startup deliberately
// does not perform). Single-threaded per MECHANISM_SPEC §4 step 10 ("one
// thread... multi-threading it is a cut item").
//
// poll_timeout_ms bounds how long each poll() waits before re-checking
// *stop; it does not affect correctness (I-2: EAGAIN is the only
// authoritative empty-queue signal, poll() is advisory only).
void pager_run(region_t *r, volatile sig_atomic_t *stop, int poll_timeout_ms);

// Binary search for the chunk covering region-relative offset `rel_off`.
// Exposed for testing. Returns NULL if out of range.
chunk_t *pager_lookup(region_t *r, uint64_t rel_off);

#endif
