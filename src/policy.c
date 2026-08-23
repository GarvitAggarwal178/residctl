// policy.c -- see policy.h.
#define _GNU_SOURCE
#include "policy.h"

#include <stdlib.h>
#include <string.h>

// ---- lru --------------------------------------------------------------

static uint32_t lru_select_victim(region_t *r) {
    uint32_t best = CHUNK_NONE;
    uint64_t best_seq = 0;
    for (uint32_t i = 0; i < r->n_chunks; i++) {
        chunk_t *c = &r->chunks[i];
        if (c->state != CHUNK_RESIDENT || c->pin != 0) continue;
        if (best == CHUNK_NONE || c->last_fault_seq < best_seq) {
            best = i;
            best_seq = c->last_fault_seq;
        }
    }
    return best;
}

static int32_t lru_predict_next(region_t *r, chunk_t *c) {
    (void)r; (void)c;
    return -1;
}

// item 10c Task B (A-6): lru has no predict_next (always -1), so it has no
// basis to claim any chunk is "closer" than any other -- always INT64_MAX,
// per the spec's explicit requirement. In practice this is moot: lru's
// predict_next==-1 already means prefetch_pool_top_up() never enqueues
// anything under lru, so prefetch_admit() is never even called for it.
// Implemented anyway for interface completeness and to fail safe if that
// ever changes.
static int64_t lru_next_use_distance(region_t *r, chunk_t *c) {
    (void)r; (void)c;
    return INT64_MAX;
}

static void lru_noop(region_t *r, chunk_t *c) { (void)r; (void)c; }

// Campaign 13 Phase A.3: lru has no ordering cursor concept.
static uint32_t lru_trace_cursor(region_t *r) { (void)r; return CHUNK_NONE; }

policy_t *policy_lru_create(void) {
    policy_t *p = calloc(1, sizeof *p);
    p->name = "lru";
    p->on_fault = lru_noop;
    p->on_resident = lru_noop;
    p->select_victim = lru_select_victim;
    p->predict_next = lru_predict_next;
    p->next_use_distance = lru_next_use_distance;
    p->trace_cursor = lru_trace_cursor;
    p->state = NULL;
    return p;
}

// ---- layer_order --------------------------------------------------------

typedef struct {
    uint32_t n_chunks;
    uint32_t last_fetched;   // CHUNK_NONE until the first fetch
    uint32_t *successor;     // successor[i] = chunk observed to follow i, or CHUNK_NONE
} layer_order_state_t;

static uint32_t chunk_index(region_t *r, chunk_t *c) {
    return (uint32_t)(c - r->chunks);
}

static void layer_order_on_fault(region_t *r, chunk_t *c) {
    layer_order_state_t *st = (layer_order_state_t *)r->policy->state;
    uint32_t idx = chunk_index(r, c);
    if (st->last_fetched != CHUNK_NONE)
        st->successor[st->last_fetched] = idx;
    st->last_fetched = idx;
}

static void layer_order_on_resident(region_t *r, chunk_t *c) { (void)r; (void)c; }

static int32_t layer_order_predict_next(region_t *r, chunk_t *c) {
    layer_order_state_t *st = (layer_order_state_t *)r->policy->state;
    uint32_t idx = chunk_index(r, c);
    uint32_t next = st->successor[idx];
    return (next == CHUNK_NONE) ? -1 : (int32_t)next;
}

// item 10c Task B (A-6): extracted so next_use_distance() can reuse the
// EXACT same walk select_victim() already used internally -- "next use
// distance" must mean the same thing in both places, or a prefetch's
// admission decision could disagree with what the policy itself believes
// about eviction order. Fills dist[i] for every chunk (not just RESIDENT
// ones); UINT32_MAX = never reached by the walk = infinitely far/unknown.
static void layer_order_compute_dist(layer_order_state_t *st, uint32_t *dist) {
    for (uint32_t i = 0; i < st->n_chunks; i++) dist[i] = UINT32_MAX; // unknown = infinitely far

    if (st->last_fetched != CHUNK_NONE) {
        uint32_t node = st->last_fetched;
        uint32_t d = 1;
        for (uint32_t hops = 0; hops < st->n_chunks; hops++) {
            uint32_t next = st->successor[node];
            if (next == CHUNK_NONE || dist[next] != UINT32_MAX) break; // unknown or cycle closed
            dist[next] = d++;
            node = next;
        }
    }
}

static uint32_t layer_order_select_victim(region_t *r) {
    layer_order_state_t *st = (layer_order_state_t *)r->policy->state;

    uint32_t *dist = malloc(st->n_chunks * sizeof(uint32_t));
    layer_order_compute_dist(st, dist);

    uint32_t best = CHUNK_NONE;
    uint32_t best_dist = 0;
    for (uint32_t i = 0; i < r->n_chunks; i++) {
        chunk_t *c = &r->chunks[i];
        if (c->state != CHUNK_RESIDENT || c->pin != 0) continue;
        uint32_t d = dist[i]; // UINT32_MAX (unknown) naturally wins any unsigned comparison
        if (best == CHUNK_NONE || d > best_dist) {
            best = i;
            best_dist = d;
        }
    }
    free(dist);
    return best;
}

static int64_t layer_order_next_use_distance(region_t *r, chunk_t *c) {
    layer_order_state_t *st = (layer_order_state_t *)r->policy->state;
    uint32_t idx = chunk_index(r, c);

    uint32_t *dist = malloc(st->n_chunks * sizeof(uint32_t));
    layer_order_compute_dist(st, dist);
    uint32_t d = dist[idx];
    free(dist);

    return (d == UINT32_MAX) ? INT64_MAX : (int64_t)d;
}

// Campaign 13 Phase A.3: the successor-chain walk's starting point --
// exactly the same `last_fetched` layer_order_compute_dist() itself reads.
// Not a new decision point, just exposing the existing one for tracing.
static uint32_t layer_order_trace_cursor(region_t *r) {
    layer_order_state_t *st = (layer_order_state_t *)r->policy->state;
    return st->last_fetched;
}

policy_t *policy_layer_order_create(uint32_t n_chunks) {
    policy_t *p = calloc(1, sizeof *p);
    layer_order_state_t *st = calloc(1, sizeof *st);
    st->n_chunks = n_chunks;
    st->last_fetched = CHUNK_NONE;
    st->successor = malloc(n_chunks * sizeof(uint32_t));
    for (uint32_t i = 0; i < n_chunks; i++) st->successor[i] = CHUNK_NONE;

    p->name = "layer_order";
    p->on_fault = layer_order_on_fault;
    p->on_resident = layer_order_on_resident;
    p->select_victim = layer_order_select_victim;
    p->predict_next = layer_order_predict_next;
    p->next_use_distance = layer_order_next_use_distance;
    p->trace_cursor = layer_order_trace_cursor;
    p->state = st;
    return p;
}

void policy_destroy(policy_t *p) {
    if (!p) return;
    if (strcmp(p->name, "layer_order") == 0 && p->state) {
        layer_order_state_t *st = (layer_order_state_t *)p->state;
        free(st->successor);
        free(st);
    }
    free(p);
}
