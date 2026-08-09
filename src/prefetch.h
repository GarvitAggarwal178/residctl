// prefetch.h -- MECHANISM_SPEC.md §6.3.
//
// DESIGN NOTE, disclosed rather than silently decided: §6.3 says "enqueue
// chunk N+1 for asynchronous fetch," which could mean a dedicated prefetch
// OS thread. This implementation does NOT add one. §4 step 10 is explicit
// that the handler stays single-threaded and that multi-threading "must be
// justified by measured handler queue depth, not assumed" -- no such
// measurement exists. A real prefetch thread would also need a new
// region-wide accounting lock (resident_bytes/ensure_budget/evict_chunk
// currently assume a single caller, safe only because nothing else has
// ever called them concurrently), which is real new SMP-correctness surface
// this project has no measured justification to take on yet.
//
// Instead, prefetch runs INLINE in the single handler thread, immediately
// after the triggering fetch completes. This still satisfies §6.3's
// observable requirement -- a correctly predicted chunk generates zero
// fault for whoever touches it later, "which is where the performance
// comes from" -- without adding concurrency. "One outstanding prefetch
// maximum" holds trivially since prefetch is synchronous by construction.
#ifndef RESIDCTL_PREFETCH_H
#define RESIDCTL_PREFETCH_H

#include "region.h"

// Called after `just_resident` transitions to CHUNK_RESIDENT (after
// on_resident, per §5's ordering). If the policy predicts a successor and
// that chunk is CHUNK_ABSENT, pins it (I-4), fetches it through the normal
// budget/fetch path, and marks it RESIDENT -- all before returning. A
// second prediction from the prefetched chunk is NOT chased (no chaining;
// "one outstanding prefetch maximum").
void maybe_prefetch(region_t *r, chunk_t *just_resident);

#endif
