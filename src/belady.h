// belady.h -- MECHANISM_SPEC.md §10 offline optimal solver, core algorithm.
#ifndef RESIDCTL_BELADY_H
#define RESIDCTL_BELADY_H

#include <stdint.h>

typedef struct {
    uint64_t misses;
    uint64_t total_refs;
} belady_result_t;

// Standard Belady (MIN) simulation: given the reference string `ref`
// (length n_refs, values are chunk ids) and a cache capacity in chunk
// slots, returns the minimum achievable number of misses (fetches) any
// replacement policy could have made against this exact sequence -- the
// theoretical lower bound §11's arms are compared against (OPT).
//
// O(n log n): one reverse-ish pass to build per-chunk occurrence queues,
// one forward pass with a lazily-deleted max-heap keyed on each cached
// chunk's next-occurrence position (evict the chunk due furthest in the
// future, or never again).
belady_result_t belady_simulate(const uint32_t *ref, uint64_t n_refs, uint32_t capacity);

#endif
