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

void reconcile(region_t *r) {
    uint64_t shmem = read_memory_stat_shmem(r->cgroup_path);
    uint64_t expected = r->resident_bytes + r->known_overhead_bytes;
    uint64_t threshold = r->n_chunks > 0 ? r->chunks[0].len : RESIDCTL_ALIGN;
    uint64_t diff = shmem > expected ? shmem - expected : expected - shmem;
    if (diff > threshold) {
        fprintf(stderr,
                "RECONCILE FAILED (I-7): memory.stat[shmem]=%llu vs our "
                "accounting (resident_bytes=%llu + known_overhead=%llu) = "
                "%llu, diff=%llu exceeds one-chunk threshold=%llu. This is "
                "the only defence against an eviction that silently didn't "
                "happen -- treating as fatal, not a warning.\n",
                (unsigned long long)shmem, (unsigned long long)r->resident_bytes,
                (unsigned long long)r->known_overhead_bytes, (unsigned long long)expected,
                (unsigned long long)diff, (unsigned long long)threshold);
        abort();
    }
}

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

int ensure_budget(region_t *r, uint64_t need) {
    reconcile(r); // I-7, every policy decision point

    while (r->resident_bytes + need > r->budget_bytes) {
        uint32_t victim_idx = r->policy ? r->policy->select_victim(r) : default_select_victim(r);
        if (victim_idx == CHUNK_NONE) {
            r->stat_infeasible++;
            return -1;
        }
        evict_chunk(r, &r->chunks[victim_idx]);
    }
    return 0;
}
