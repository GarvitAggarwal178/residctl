// budget.c -- MECHANISM_SPEC.md §7.
#define _GNU_SOURCE
#include "budget.h"
#include "cgroup_stat.h"
#include "policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static void budget_fail(const char *step, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "BUDGET FAILED at step [%s]: ", step);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

// Single-buffer read (§9's lesson: separate fopen()s per field can race
// live counters -- see SPIKE_ADDENDUM2.md Step 1). reconcile() only needs
// one field today, but goes through the shared snapshot reader so there is
// exactly one implementation of "read memory.stat safely" in this codebase.
static uint64_t read_memory_stat_shmem(const char *cgroup_path) {
    cgroup_stat_snapshot_t snap;
    if (cgroup_stat_snapshot_read(cgroup_path, "memory.stat", &snap) != 0)
        budget_fail("reconcile", "cannot read %s/memory.stat: %s", cgroup_path, strerror(errno));
    uint64_t val;
    if (!cgroup_stat_snapshot_field(&snap, "shmem", &val))
        budget_fail("reconcile", "'shmem' field not found in %s/memory.stat", cgroup_path);
    return val;
}

// Caller must hold r->budget_lock (see budget.h).
void reconcile(region_t *r) {
    uint64_t shmem = read_memory_stat_shmem(r->cgroup_path);
    uint64_t threshold = r->n_chunks > 0 ? r->chunks[0].len : RESIDCTL_ALIGN;

    // Item 10b Task B: with prefetch_depth>1, a fetch's pwrite() into map_b
    // populates real shmem pages incrementally WHILE the fetch is still
    // in flight -- i.e. before commit_reserved() moves its bytes from
    // reserved_bytes into resident_bytes. If another thread's reconcile()
    // lands in that window, the kernel's memory.stat[shmem] can already be
    // partway (or all the way) toward reflecting an in-flight fetch that
    // our resident_bytes alone doesn't know about yet. That's not I-7's
    // "eviction that silently didn't happen" failure mode -- it's a
    // legitimate timing window. The true value of shmem at any instant is
    // somewhere in [resident_bytes, resident_bytes+reserved_bytes]; only
    // a value OUTSIDE that range (by more than one chunk of slack on
    // either side) is a real divergence. At depth==1, reserved_bytes is
    // always 0 whenever reconcile() runs (the single thread that reserves
    // it is the same one that commits it, sequentially), so this reduces
    // to exactly the original single-sided check.
    uint64_t lower = r->resident_bytes + r->known_overhead_bytes;
    uint64_t upper = lower + r->reserved_bytes;

    if (shmem + threshold < lower) {
        fprintf(stderr,
                "RECONCILE FAILED (I-7): memory.stat[shmem]=%llu is BELOW our confirmed-resident "
                "floor (resident_bytes=%llu + known_overhead=%llu = %llu) by more than one chunk "
                "(threshold=%llu). This is the only defence against an eviction that silently "
                "didn't happen -- treating as fatal, not a warning.\n",
                (unsigned long long)shmem, (unsigned long long)r->resident_bytes,
                (unsigned long long)r->known_overhead_bytes, (unsigned long long)lower,
                (unsigned long long)threshold);
        abort();
    }
    if (shmem > upper + threshold) {
        fprintf(stderr,
                "RECONCILE FAILED (I-7): memory.stat[shmem]=%llu exceeds even our upper bound "
                "(resident_bytes+reserved_bytes+known_overhead=%llu) by more than one chunk "
                "(threshold=%llu) -- something is resident that neither our confirmed nor our "
                "in-flight accounting knows about.\n",
                (unsigned long long)shmem, (unsigned long long)upper, (unsigned long long)threshold);
        abort();
    }
}

// Caller must hold r->budget_lock (see budget.h).
void evict_chunk(region_t *r, chunk_t *c) {
    pthread_mutex_lock(&c->lock);
    if (c->state != CHUNK_RESIDENT)
        budget_fail("evict_chunk", "chunk region_off=%llu not RESIDENT (state=%d), I-4 violation",
                   (unsigned long long)c->region_off, (int)c->state);
    if (c->pin != 0)
        budget_fail("evict_chunk", "chunk region_off=%llu is pinned, I-4 violation",
                   (unsigned long long)c->region_off);

    if (fallocate(r->memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                  (off_t)c->region_off, (off_t)c->len) != 0)
        budget_fail("evict_chunk", "fallocate(PUNCH_HOLE, off=%llu, len=%llu) failed: %s",
                   (unsigned long long)c->region_off, (unsigned long long)c->len, strerror(errno));

    c->state = CHUNK_ABSENT;
    r->resident_bytes -= c->len;
    r->stat_evictions++;
    r->stat_bytes_punched += c->len;
    pthread_mutex_unlock(&c->lock);
}

// Default victim selector when r->policy is NULL: lowest layer_id /
// region_off among RESIDENT+unpinned. This used to be the only option
// (documented as "temporary until item 7"); now that item 7 exists, it's
// kept as the no-policy-configured default so items 4-6's tests (which
// never set region_t.policy) keep working unchanged, and it's still a
// legitimate deterministic fallback, not a hack.
static uint32_t default_select_victim(region_t *r) {
    for (uint32_t i = 0; i < r->n_chunks; i++) {
        if (r->chunks[i].state == CHUNK_RESIDENT && r->chunks[i].pin == 0)
            return i;
    }
    return CHUNK_NONE;
}

// Spec amendment A-3 (item 10 correction): reconcile() used to run
// unconditionally here, on EVERY fetch -- a real, measured, un-amortized
// cost (a fresh open/read/close of memory.stat per fetch). Now it runs
// unconditionally on every eviction (I-7's core safety property --
// "the only defence against an eviction that silently didn't happen" -- is
// unchanged, since we still check immediately after every single punch)
// and otherwise only every reconcile_interval fetches.
// reconcile_interval==1 (region_config_t.reconcile_interval=1, or
// --eager-reconcile in the CLI binaries) reproduces the old eager behaviour
// exactly; the §13 correctness harness runs with that set.
//
// Item 10b Task B: the whole reconcile+evict+reserve sequence runs under
// r->budget_lock, so it's safe to call this concurrently from the handler
// thread and any prefetch_pool workers (depth>1). At depth==1 there is
// never any contention on this lock (only one thread ever calls it), so
// behavior and performance are unchanged from before Task B.
int ensure_budget(region_t *r, uint64_t need) {
    pthread_mutex_lock(&r->budget_lock);

    r->fetches_since_reconcile++;
    if (r->reconcile_interval <= 1 || r->fetches_since_reconcile >= r->reconcile_interval) {
        reconcile(r);
        r->fetches_since_reconcile = 0;
    }

    while (r->resident_bytes + r->reserved_bytes + need > r->budget_bytes) {
        uint32_t victim_idx = r->policy ? r->policy->select_victim(r) : default_select_victim(r);
        if (victim_idx == CHUNK_NONE) {
            // item 10d Task C (A-9): before giving up, this is a DEMAND
            // fetch (ensure_budget_prefetch() has its own, separate gate and
            // never reaches this function) -- "a speculative chunk must
            // never starve a real one." If everything RESIDENT is pinned
            // because it's a retained prefetch target, break the coldest
            // one's pin and evict it instead of failing.
            if (r->prefetch_retention_pinned) {
                uint32_t pin_victim = pin_break_select_victim(r);
                if (pin_victim != CHUNK_NONE) {
                    prefetch_retain_release(r, pin_victim); // drops the retention pin; I-4's pin==0 still holds by the time evict_chunk() checks it
                    r->stat_pin_broken++;
                    evict_chunk(r, &r->chunks[pin_victim]);
                    reconcile(r);
                    r->fetches_since_reconcile = 0;
                    continue;
                }
            }
            // Infeasible: no evictable victim (e.g. everything left is
            // pinned). Not forced -- per Task B's explicit instruction, a
            // prefetch that would need to evict a pinned chunk is dropped,
            // not forced; this is exactly the mechanism that drops it
            // (the caller treats -1 as "abandon this prefetch").
            r->stat_infeasible++;
            pthread_mutex_unlock(&r->budget_lock);
            return -1;
        }
        evict_chunk(r, &r->chunks[victim_idx]);
        reconcile(r); // A-3: unconditionally on every eviction
        r->fetches_since_reconcile = 0; // an eviction just gave us a fresh check
    }

    r->reserved_bytes += need; // reserve the space; caller must commit_reserved() after the real fetch
    pthread_mutex_unlock(&r->budget_lock);
    return 0;
}

// item 10c Task B (A-6): see budget.h. Mirrors ensure_budget()'s
// reconcile/evict/reserve loop exactly, except each candidate eviction is
// gated by prefetch_admit()'s distance check before it's allowed to happen.
int ensure_budget_prefetch(region_t *r, chunk_t *target) {
    pthread_mutex_lock(&r->budget_lock);

    r->fetches_since_reconcile++;
    if (r->reconcile_interval <= 1 || r->fetches_since_reconcile >= r->reconcile_interval) {
        reconcile(r);
        r->fetches_since_reconcile = 0;
    }

    uint64_t need = target->len;
    while (r->resident_bytes + r->reserved_bytes + need > r->budget_bytes) {
        uint32_t victim_idx = r->policy ? r->policy->select_victim(r) : default_select_victim(r);
        if (victim_idx == CHUNK_NONE) {
            r->stat_infeasible++;
            pthread_mutex_unlock(&r->budget_lock);
            return -1;
        }

        if (!r->prefetch_admission_always && r->policy && r->policy->next_use_distance) {
            // prefetch_admit(): "a prefetch may not force an eviction unless
            // it is strictly justified" -- the victim must be needed
            // STRICTLY later than the prefetch's own target, or this
            // eviction (and therefore the whole prefetch) is declined
            // rather than forced. Demand fetches never go through this
            // function, so they're unaffected.
            int64_t victim_dist = r->policy->next_use_distance(r, &r->chunks[victim_idx]);
            int64_t target_dist = r->policy->next_use_distance(r, target);
            if (!(victim_dist > target_dist)) {
                r->stat_prefetch_declined++;
                pthread_mutex_unlock(&r->budget_lock);
                return -1; // same "abandon this prefetch, not fatal" contract as ensure_budget()
            }
        }

        evict_chunk(r, &r->chunks[victim_idx]);
        reconcile(r); // A-3: unconditionally on every eviction
        r->fetches_since_reconcile = 0;
    }

    r->reserved_bytes += need;
    pthread_mutex_unlock(&r->budget_lock);
    return 0;
}

void commit_reserved(region_t *r, uint64_t len) {
    pthread_mutex_lock(&r->budget_lock);
    r->reserved_bytes -= len;
    r->resident_bytes += len;
    pthread_mutex_unlock(&r->budget_lock);
}

// Task B: like commit_reserved(), but also pins `c` in the SAME budget_lock
// critical section. Needed specifically by handle_absent(): it still holds
// c->lock and may need budget_lock again afterward (prefetch_pool_top_up's
// predict_next calls) before it's done with this chunk. If state became
// RESIDENT and pin were applied as two separate steps, there's a real
// window between them where a concurrent worker's select_victim() can pick
// this chunk (RESIDENT, still unpinned) and block on c->lock, while this
// thread blocks acquiring budget_lock for the (not-yet-applied) pin --
// circular wait. Doing both under one lock acquisition closes the window
// entirely. Reproduced as a flaky (timing-dependent) hang at
// --prefetch-depth 8 before this fix.
void commit_reserved_and_pin(region_t *r, chunk_t *c, uint64_t len) {
    pthread_mutex_lock(&r->budget_lock);
    r->reserved_bytes -= len;
    r->resident_bytes += len;
    c->pin++;
    pthread_mutex_unlock(&r->budget_lock);
}

void unpin_chunk(region_t *r, chunk_t *c) {
    pthread_mutex_lock(&r->budget_lock);
    c->pin--;
    pthread_mutex_unlock(&r->budget_lock);
}

void pin_chunk(region_t *r, chunk_t *c) {
    pthread_mutex_lock(&r->budget_lock);
    c->pin++;
    pthread_mutex_unlock(&r->budget_lock);
}

// item 10d Task C (A-9). Caller holds budget_lock.
void prefetch_retain_on_resident(region_t *r, chunk_t *target) {
    uint32_t idx = (uint32_t)(target - r->chunks);
    if (r->pinned_prefetch_len >= r->pinned_prefetch_cap) {
        // Cap reached: unpin+drop the OLDEST entry first ("it has waited
        // longest and is most likely stale," per §6.3 Amendment A-9).
        uint32_t oldest_idx = r->pinned_prefetch_queue[r->pinned_prefetch_head];
        r->pinned_prefetch_head = (r->pinned_prefetch_head + 1) % r->pinned_prefetch_cap;
        r->pinned_prefetch_len--;
        r->chunks[oldest_idx].pin--;
    }
    r->pinned_prefetch_queue[(r->pinned_prefetch_head + r->pinned_prefetch_len) % r->pinned_prefetch_cap] = idx;
    r->pinned_prefetch_len++;
    // target->pin is NOT incremented here -- the pre-fetch pin++ the caller
    // already did (I-4, protects the in-flight fetch itself) is repurposed
    // as this chunk's retention pin. The caller must not also unpin.
}

// item 10d Task C (A-9). Caller holds budget_lock.
void prefetch_retain_release(region_t *r, uint32_t chunk_idx) {
    for (uint32_t i = 0; i < r->pinned_prefetch_len; i++) {
        uint32_t pos = (r->pinned_prefetch_head + i) % r->pinned_prefetch_cap;
        if (r->pinned_prefetch_queue[pos] != chunk_idx) continue;
        // Remove by shifting the tail down -- the queue is bounded at
        // prefetch_depth entries (small), so this is cheap.
        for (uint32_t j = i; j + 1 < r->pinned_prefetch_len; j++) {
            uint32_t from = (r->pinned_prefetch_head + j + 1) % r->pinned_prefetch_cap;
            uint32_t to = (r->pinned_prefetch_head + j) % r->pinned_prefetch_cap;
            r->pinned_prefetch_queue[to] = r->pinned_prefetch_queue[from];
        }
        r->pinned_prefetch_len--;
        r->chunks[chunk_idx].pin--;
        return;
    }
    // Not in the pinned set -- expected the vast majority of the time (most
    // references aren't to a currently-retained prefetch target). Not an
    // error.
}

// item 10d Task C (A-9). Caller holds budget_lock.
uint32_t pin_break_select_victim(region_t *r) {
    if (r->pinned_prefetch_len == 0) return CHUNK_NONE;
    uint32_t best = CHUNK_NONE;
    int64_t best_dist = -1;
    for (uint32_t i = 0; i < r->pinned_prefetch_len; i++) {
        uint32_t pos = (r->pinned_prefetch_head + i) % r->pinned_prefetch_cap;
        uint32_t idx = r->pinned_prefetch_queue[pos];
        chunk_t *c = &r->chunks[idx];
        if (c->state != CHUNK_RESIDENT) continue; // defensive; shouldn't happen
        int64_t d = (r->policy && r->policy->next_use_distance) ? r->policy->next_use_distance(r, c) : INT64_MAX;
        if (best == CHUNK_NONE || d > best_dist) { best = idx; best_dist = d; }
        // ties (including "no policy distance, everything INT64_MAX") keep
        // the FIRST (oldest, per FIFO order) entry found -- a reasonable
        // fallback to "oldest" per the same "most likely stale" reasoning
        // the cap-eviction rule uses.
    }
    return best;
}
