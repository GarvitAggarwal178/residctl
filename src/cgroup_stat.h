// cgroup_stat.h -- read a cgroup key-value file (memory.stat, memory.events)
// once into a single buffer and extract fields from that buffer.
//
// §9 is explicit about why: "Read memory.stat once per sample into a single
// buffer and extract all fields from it -- the spike logged two transient
// bare-vs-split mismatches from five separate fopen() calls racing live
// counters." Every field read for one sample must come from the same
// snapshot, not from separate reopens that can straddle a counter update.
#ifndef RESIDCTL_CGROUP_STAT_H
#define RESIDCTL_CGROUP_STAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CGROUP_STAT_BUF_SIZE 8192

typedef struct {
    char buf[CGROUP_STAT_BUF_SIZE];
    size_t len;
} cgroup_stat_snapshot_t;

// Reads the whole file (memory.stat, memory.events, ...) at cgroup_path/name
// into snap. Returns 0 on success, -1 on error (errno set). Truncates
// silently only if the file exceeds CGROUP_STAT_BUF_SIZE -- memory.stat on
// this kernel is well under that; if a future kernel adds enough fields to
// overflow it, field lookups past the truncation point will just report
// "not found," which is a safe failure mode for reconcile()'s caller to
// detect rather than a silent wrong value.
int cgroup_stat_snapshot_read(const char *cgroup_path, const char *filename, cgroup_stat_snapshot_t *snap);

// Exact-name match on the first whitespace-delimited token of each line
// (the lesson from SPIKE_ADDENDUM2.md's Step 1: confirm the key actually
// exists verbatim rather than assuming). Returns true and sets *out if
// found, false otherwise.
bool cgroup_stat_snapshot_field(const cgroup_stat_snapshot_t *snap, const char *key, uint64_t *out);

// Sums every field whose name starts with `prefix` (e.g. "pgscan" matches
// both a bare "pgscan" line, if present, and split variants like
// "pgscan_kswapd", "pgscan_direct" -- mirrors SPIKE_ADDENDUM2.md's
// cgroup_stat_split_sum()). Returns the count of matching fields via
// *n_matched (may be 0); the sum is always returned, 0 if nothing matched.
uint64_t cgroup_stat_snapshot_sum_prefix(const cgroup_stat_snapshot_t *snap, const char *prefix, int *n_matched);

#endif
