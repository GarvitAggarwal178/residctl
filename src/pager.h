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

// item 10d Task C (A-9): the "consumed" signal. Call once, right before the
// workload reads chunk c (whether or not that read causes a real fault --
// a touch on an already-RESIDENT chunk generates no uffd event at all, so
// this is the ONLY way the pager can ever learn a prefetched chunk was
// actually used; see CLAUDE.md's item 8 "known measurement gap" note). If c
// is currently a retained prefetch target, releases its retention pin
// immediately; otherwise a no-op (the common case). Safe to call
// unconditionally on every reference regardless of --prefetch-retention
// mode -- under "none" nothing is ever added to the retention set, so this
// is permanently a no-op. This is not a mechanism-internal API: it's the
// same kind of "I am about to use this layer's weights" signal a real
// engine integration (item 11) would give before a compute pass, exactly
// mirroring how the synthetic replay driver (replay.c) already knows its
// own true reference sequence regardless of hit/miss.
void pager_notify_access(region_t *r, chunk_t *c);

// CLEANUP session (7th concurrency-class fix, part a): abandon an in-flight
// fetch of chunk c -- reset its state to CHUNK_ABSENT, clear fetching_since_ns,
// and issue UFFDIO_WAKE over the chunk's range so any faulter that deduped
// against this FETCHING episode (handle_fault's CHUNK_FETCHING branch drops
// the message assuming a future CONTINUE will wake it) refaults and retries
// instead of blocking forever. Caller MUST hold c->lock. Used by every
// budget-infeasible / prefetch-declined drop path in pager.c, prefetch_pool.c
// and prefetch.c, and by pager_run()'s FETCHING watchdog.
void pager_abandon_fetch(region_t *r, chunk_t *c);

#endif
