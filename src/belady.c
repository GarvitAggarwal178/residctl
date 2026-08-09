// belady.c -- see belady.h.
#define _GNU_SOURCE
#include "belady.h"

#include <stdlib.h>
#include <string.h>

// ---- tiny array-based max-heap of (priority, chunk_id), lazy deletion ----

typedef struct { uint64_t priority; uint32_t chunk_id; } heap_entry_t;

typedef struct {
    heap_entry_t *data;
    uint64_t size;
    uint64_t cap;
} heap_t;

static void heap_init(heap_t *h, uint64_t cap) {
    h->data = malloc(cap * sizeof(heap_entry_t));
    h->size = 0;
    h->cap = cap;
}

static void heap_swap(heap_entry_t *a, heap_entry_t *b) { heap_entry_t t = *a; *a = *b; *b = t; }

static void heap_push(heap_t *h, uint64_t priority, uint32_t chunk_id) {
    uint64_t i = h->size++;
    h->data[i].priority = priority;
    h->data[i].chunk_id = chunk_id;
    while (i > 0) {
        uint64_t parent = (i - 1) / 2;
        if (h->data[parent].priority >= h->data[i].priority) break;
        heap_swap(&h->data[parent], &h->data[i]);
        i = parent;
    }
}

// Returns 1 and fills *out if non-empty, 0 if empty.
static int heap_pop_max(heap_t *h, heap_entry_t *out) {
    if (h->size == 0) return 0;
    *out = h->data[0];
    h->data[0] = h->data[--h->size];
    uint64_t i = 0;
    for (;;) {
        uint64_t l = 2 * i + 1, r = 2 * i + 2, largest = i;
        if (l < h->size && h->data[l].priority > h->data[largest].priority) largest = l;
        if (r < h->size && h->data[r].priority > h->data[largest].priority) largest = r;
        if (largest == i) break;
        heap_swap(&h->data[i], &h->data[largest]);
        i = largest;
    }
    return 1;
}

// ---- Belady simulation ---------------------------------------------------

belady_result_t belady_simulate(const uint32_t *ref, uint64_t n_refs, uint32_t capacity) {
    belady_result_t result = { .misses = 0, .total_refs = n_refs };
    if (n_refs == 0) return result;

    uint32_t n_chunks = 0;
    for (uint64_t i = 0; i < n_refs; i++) if (ref[i] + 1 > n_chunks) n_chunks = ref[i] + 1;

    uint64_t *occ_count = calloc(n_chunks, sizeof(uint64_t));
    for (uint64_t i = 0; i < n_refs; i++) occ_count[ref[i]]++;

    uint64_t **occurrence = malloc(n_chunks * sizeof(uint64_t *));
    uint64_t *fill_idx = calloc(n_chunks, sizeof(uint64_t));
    for (uint32_t c = 0; c < n_chunks; c++)
        occurrence[c] = occ_count[c] ? malloc(occ_count[c] * sizeof(uint64_t)) : NULL;
    for (uint64_t i = 0; i < n_refs; i++) {
        uint32_t c = ref[i];
        occurrence[c][fill_idx[c]++] = i;
    }

    uint64_t *read_ptr = calloc(n_chunks, sizeof(uint64_t)); // next unconsumed index into occurrence[c]
    uint64_t *current_priority = malloc(n_chunks * sizeof(uint64_t));
    for (uint32_t c = 0; c < n_chunks; c++) current_priority[c] = 0;
    uint8_t *in_cache = calloc(n_chunks, sizeof(uint8_t));

    heap_t heap;
    heap_init(&heap, n_refs + 1);

    uint32_t cache_size = 0;
    const uint64_t INF = UINT64_MAX;

    for (uint64_t i = 0; i < n_refs; i++) {
        uint32_t c = ref[i];
        read_ptr[c]++; // consume this occurrence (the one at position i)
        uint64_t next_pos = (read_ptr[c] < occ_count[c]) ? occurrence[c][read_ptr[c]] : INF;

        if (in_cache[c]) {
            current_priority[c] = next_pos;
            // Always push, even for next_pos == INF ("never used again").
            // INF naturally sorts as the best possible victim (max-heap),
            // which is correct -- but skipping the push here would make
            // this chunk invisible to eviction entirely (no heap entry
            // means it can never be found and evicted again), letting it
            // occupy a cache slot forever. Every live cached chunk must
            // always have exactly one live heap entry.
            heap_push(&heap, next_pos, c);
        } else {
            result.misses++;
            if (cache_size == capacity) {
                for (;;) {
                    heap_entry_t top;
                    if (!heap_pop_max(&heap, &top)) {
                        // Unreachable when capacity > 0 and cache_size ==
                        // capacity: every live cached chunk always has
                        // exactly one live heap entry (pushed above,
                        // including INF-priority ones), so there's always
                        // at least one valid victim to find.
                        break;
                    }
                    if (in_cache[top.chunk_id] && current_priority[top.chunk_id] == top.priority) {
                        in_cache[top.chunk_id] = 0;
                        cache_size--;
                        break;
                    }
                    // stale entry (chunk since evicted or priority changed
                    // by a later hit): discard, keep popping.
                }
            }
            if (cache_size < capacity) {
                in_cache[c] = 1;
                cache_size++;
                current_priority[c] = next_pos;
                heap_push(&heap, next_pos, c); // see note above: always push
            }
            // if cache_size == capacity here, capacity is 0 (nothing ever
            // cached) -- every reference is a miss, which is correct.
        }
    }

    free(heap.data);
    free(in_cache);
    free(current_priority);
    free(read_ptr);
    free(fill_idx);
    for (uint32_t c = 0; c < n_chunks; c++) free(occurrence[c]);
    free(occurrence);
    free(occ_count);

    return result;
}
