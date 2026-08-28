// policy.c -- see policy.h.
#define _GNU_SOURCE
#include "policy.h"

#include <stdlib.h>
#include <string.h>

static uint32_t chunk_index(region_t *r, chunk_t *c) {
    return (uint32_t)(c - r->chunks);
}

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
    p->on_access = NULL;         // no declared-sequence position
    p->declare_sequence = NULL;  // infers order online (it doesn't infer at all)
    p->state = NULL;
    return p;
}

// ---- layer_order_learned ----------------------------------------------
// (was `layer_order` through Campaign 13; renamed by WP1 / A-12, behaviour
// byte-for-byte unchanged -- every function body below is identical to the
// pre-A-12 `layer_order_*` original.)

typedef struct {
    uint32_t n_chunks;
    uint32_t last_fetched;   // CHUNK_NONE until the first fetch
    uint32_t *successor;     // successor[i] = chunk observed to follow i, or CHUNK_NONE
} lo_learned_state_t;

static void lo_learned_on_fault(region_t *r, chunk_t *c) {
    lo_learned_state_t *st = (lo_learned_state_t *)r->policy->state;
    uint32_t idx = chunk_index(r, c);
    if (st->last_fetched != CHUNK_NONE)
        st->successor[st->last_fetched] = idx;
    st->last_fetched = idx;
}

static void lo_learned_on_resident(region_t *r, chunk_t *c) { (void)r; (void)c; }

static int32_t lo_learned_predict_next(region_t *r, chunk_t *c) {
    lo_learned_state_t *st = (lo_learned_state_t *)r->policy->state;
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
static void lo_learned_compute_dist(lo_learned_state_t *st, uint32_t *dist) {
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

static uint32_t lo_learned_select_victim(region_t *r) {
    lo_learned_state_t *st = (lo_learned_state_t *)r->policy->state;

    uint32_t *dist = malloc(st->n_chunks * sizeof(uint32_t));
    lo_learned_compute_dist(st, dist);

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

static int64_t lo_learned_next_use_distance(region_t *r, chunk_t *c) {
    lo_learned_state_t *st = (lo_learned_state_t *)r->policy->state;
    uint32_t idx = chunk_index(r, c);

    uint32_t *dist = malloc(st->n_chunks * sizeof(uint32_t));
    lo_learned_compute_dist(st, dist);
    uint32_t d = dist[idx];
    free(dist);

    return (d == UINT32_MAX) ? INT64_MAX : (int64_t)d;
}

// Campaign 13 Phase A.3: the successor-chain walk's starting point --
// exactly the same `last_fetched` lo_learned_compute_dist() itself reads.
static uint32_t lo_learned_trace_cursor(region_t *r) {
    lo_learned_state_t *st = (lo_learned_state_t *)r->policy->state;
    return st->last_fetched;
}

policy_t *policy_layer_order_learned_create(uint32_t n_chunks) {
    policy_t *p = calloc(1, sizeof *p);
    lo_learned_state_t *st = calloc(1, sizeof *st);
    st->n_chunks = n_chunks;
    st->last_fetched = CHUNK_NONE;
    st->successor = malloc(n_chunks * sizeof(uint32_t));
    for (uint32_t i = 0; i < n_chunks; i++) st->successor[i] = CHUNK_NONE;

    p->name = "layer_order_learned";
    p->on_fault = lo_learned_on_fault;
    p->on_resident = lo_learned_on_resident;
    p->select_victim = lo_learned_select_victim;
    p->predict_next = lo_learned_predict_next;
    p->next_use_distance = lo_learned_next_use_distance;
    p->trace_cursor = lo_learned_trace_cursor;
    p->on_access = NULL;         // learns from fault-dispatch order, not a declared signal
    p->declare_sequence = NULL;
    p->state = st;
    return p;
}

// ---- layer_order_declared (WP1 / Amendment A-12) ---------------------
//
// The application declares its access sequence up front. Next-use distance
// is a lookup into that declared sequence from the current consumption
// position, NOT an online-learned chain walk -- structurally different from
// the kernel (and from layer_order_learned), which must infer the future
// from the past. The consumption position advances ONLY in on_access()
// (fired by pager_notify_access(), once per reference, in the workload's
// own order), never from on_fault -- so a declared static sequence cannot
// be perturbed by fault-dispatch order.

typedef struct {
    uint32_t n_chunks;
    uint32_t *seq;          // the declared reference string for one pass
    uint32_t  seq_len;
    int64_t   pos;          // index into seq of the most recently consumed
                            // reference; -1 before the first on_access()
    uint32_t *next_in_seq;  // next_in_seq[x] = chunk that follows x's last
                            // occurrence in one cyclic pass; CHUNK_NONE if x
                            // does not appear in the declared sequence
} lo_declared_state_t;

// Cyclic distance from the consumption position to x's next use in the
// declared sequence. INT64_MAX if x never appears in the declared sequence.
// This is the ONE shared notion of "next use distance" -- select_victim and
// next_use_distance both go through it, so a prefetch's admission decision
// can never silently disagree with the policy's own eviction ranking (A-6).
//
// WP0 consumption-signal fix (session 2): the chunk the consumption signal
// currently points at (seq[pos]) is the one being consumed RIGHT NOW -- its
// next use is imminent, not one full cycle away. Return 0 for it so
// select_victim never evicts the actively-consumed chunk. Without this, the
// `for d = 1..seq_len` loop matched seq[pos] only on the full wrap (d ==
// seq_len), making the actively-consumed chunk the *furthest*-future and
// therefore the top eviction victim -- WP2's real-inference measurement
// showed layer_order_declared then re-faulting its own working set roughly
// 1.8x per pass (arm D read 1.5x more bytes than "refetch everything").
// This does not change the WP1 synthetic path: there the consumption signal
// fires just before the read, so seq[pos] is pinned throughout the window
// this could matter (verified: WP1 §1.2 gate + §1.3 determinism grid
// unchanged after this fix).
static int64_t lo_declared_dist(lo_declared_state_t *st, uint32_t x) {
    if (st->seq_len == 0) return INT64_MAX;
    uint64_t base = (st->pos < 0) ? 0 : (uint64_t)st->pos;
    // Protect the chunk being consumed now (seq[pos]) and the one consumed
    // immediately before it (seq[pos-1]). The one-step lookback covers two
    // real cases: (1) the consumption signal fires slightly after the fault
    // that actually needs the chunk (WP2's per-layer eval callback fires
    // post-compute), so "current" per the policy lags reality by ~1; (2) a
    // single transformer layer whose weights the GGUF split across two
    // non-contiguous file chunks -- both are live while that layer computes.
    if (st->seq[base % st->seq_len] == x) return 0;
    if (base > 0 && st->seq[(base - 1) % st->seq_len] == x) return 0;
    for (uint32_t d = 1; d <= st->seq_len; d++) {
        if (st->seq[(base + d) % st->seq_len] == x) return (int64_t)d;
    }
    return INT64_MAX;
}

// Advance the consumption position to this chunk's next slot in the
// declared sequence (cyclically, scanning forward from just past the
// current position). For a workload that consumes chunks in exactly the
// declared order this is a plain +1 each call; the forward search also
// keeps the position correct if a caller ever skips or reorders a
// reference relative to what it declared.
static void lo_declared_on_access(region_t *r, chunk_t *c) {
    lo_declared_state_t *st = (lo_declared_state_t *)r->policy->state;
    if (st->seq_len == 0) return;
    uint32_t idx = chunk_index(r, c);
    uint64_t from = (st->pos < 0) ? 0 : (uint64_t)(st->pos + 1);
    for (uint32_t k = 0; k < st->seq_len; k++) {
        uint64_t q = (from + k) % st->seq_len;
        if (st->seq[q] == idx) { st->pos = (int64_t)q; return; }
    }
    // idx not in the declared sequence -- leave pos where it was (this
    // chunk contributes nothing to the declared ordering).
}

static void lo_declared_on_fault(region_t *r, chunk_t *c) { (void)r; (void)c; }
static void lo_declared_on_resident(region_t *r, chunk_t *c) { (void)r; (void)c; }

static int32_t lo_declared_predict_next(region_t *r, chunk_t *c) {
    lo_declared_state_t *st = (lo_declared_state_t *)r->policy->state;
    uint32_t idx = chunk_index(r, c);
    if (idx >= st->n_chunks) return -1;
    uint32_t nxt = st->next_in_seq[idx];
    return (nxt == CHUNK_NONE) ? -1 : (int32_t)nxt;
}

static uint32_t lo_declared_select_victim(region_t *r) {
    lo_declared_state_t *st = (lo_declared_state_t *)r->policy->state;
    uint32_t best = CHUNK_NONE;
    int64_t best_dist = -1;
    for (uint32_t i = 0; i < r->n_chunks; i++) {
        chunk_t *c = &r->chunks[i];
        if (c->state != CHUNK_RESIDENT || c->pin != 0) continue;
        int64_t d = lo_declared_dist(st, i);
        if (best == CHUNK_NONE || d > best_dist) {
            best = i;
            best_dist = d;
        }
    }
    return best;
}

static int64_t lo_declared_next_use_distance(region_t *r, chunk_t *c) {
    lo_declared_state_t *st = (lo_declared_state_t *)r->policy->state;
    return lo_declared_dist(st, chunk_index(r, c));
}

// Campaign 13 Phase A.3: the chunk currently at the consumption position --
// the declared-sequence analogue of layer_order_learned's `last_fetched`
// cursor. CHUNK_NONE before the first on_access().
static uint32_t lo_declared_trace_cursor(region_t *r) {
    lo_declared_state_t *st = (lo_declared_state_t *)r->policy->state;
    if (st->pos < 0 || st->seq_len == 0) return CHUNK_NONE;
    return st->seq[st->pos];
}

static void lo_declared_declare_sequence(region_t *r, const uint32_t *ids, uint32_t n) {
    lo_declared_state_t *st = (lo_declared_state_t *)r->policy->state;
    free(st->seq);
    st->seq = malloc(n * sizeof(uint32_t));
    memcpy(st->seq, ids, n * sizeof(uint32_t));
    st->seq_len = n;
    st->pos = -1;
    for (uint32_t i = 0; i < st->n_chunks; i++) st->next_in_seq[i] = CHUNK_NONE;
    // next_in_seq[x] from x's LAST occurrence in the declared pass -- a
    // single forward pass (the reverse-pass equivalent for this "successor
    // of the final occurrence" definition); the sequence is assumed to
    // repeat cyclically so position n wraps to position 0.
    for (uint32_t p = 0; p < n; p++) {
        uint32_t x = ids[p];
        if (x < st->n_chunks) st->next_in_seq[x] = ids[(p + 1) % n];
    }
}

policy_t *policy_layer_order_declared_create(uint32_t n_chunks) {
    policy_t *p = calloc(1, sizeof *p);
    lo_declared_state_t *st = calloc(1, sizeof *st);
    st->n_chunks = n_chunks;
    st->seq = NULL;
    st->seq_len = 0;
    st->pos = -1;
    st->next_in_seq = malloc(n_chunks * sizeof(uint32_t));
    for (uint32_t i = 0; i < n_chunks; i++) st->next_in_seq[i] = CHUNK_NONE;

    p->name = "layer_order_declared";
    p->on_fault = lo_declared_on_fault;
    p->on_resident = lo_declared_on_resident;
    p->select_victim = lo_declared_select_victim;
    p->predict_next = lo_declared_predict_next;
    p->next_use_distance = lo_declared_next_use_distance;
    p->trace_cursor = lo_declared_trace_cursor;
    p->on_access = lo_declared_on_access;
    p->declare_sequence = lo_declared_declare_sequence;
    p->state = st;
    return p;
}

void policy_declare_sequence(region_t *r, const uint32_t *chunk_ids, uint32_t n) {
    if (r->policy && r->policy->declare_sequence)
        r->policy->declare_sequence(r, chunk_ids, n);
}

void policy_destroy(policy_t *p) {
    if (!p) return;
    if (strcmp(p->name, "layer_order_learned") == 0 && p->state) {
        lo_learned_state_t *st = (lo_learned_state_t *)p->state;
        free(st->successor);
        free(st);
    } else if (strcmp(p->name, "layer_order_declared") == 0 && p->state) {
        lo_declared_state_t *st = (lo_declared_state_t *)p->state;
        free(st->seq);
        free(st->next_in_seq);
        free(st);
    }
    free(p);
}
