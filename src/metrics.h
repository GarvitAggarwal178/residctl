// metrics.h -- MECHANISM_SPEC.md §9 treatment-arm-only metrics not already
// covered by region_t's bare fault/dedup/eviction counters (items 2-4):
// handler service latency and queue depth high-water.
#ifndef RESIDCTL_METRICS_H
#define RESIDCTL_METRICS_H

#include <stdint.h>
#include <pthread.h>

// Log2(nanoseconds)-bucketed latency histogram. NOT a real HDR histogram
// library -- no sub-bucket linear interpolation, no configurable
// significant-figure precision. It is a plain power-of-two bucketing,
// documented as an approximation because that's what it is. 48 buckets
// covers 1ns..2^48ns (~3.2 days), far beyond any plausible fetch latency;
// this is enough for the p50/p99/max reporting §9 asks for.
#define METRICS_LATENCY_BUCKETS 48

typedef struct {
    uint64_t bucket_counts[METRICS_LATENCY_BUCKETS];
    uint64_t count;
    uint64_t sum_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    // item 10c: latency_hist_record() can now be called concurrently by
    // multiple fetch-pool workers (was always single-threaded before --
    // the sync handler is the only caller in the old design). Internal
    // lock, same reasoning as fetch_trace_t's (item 10b Task A): callers
    // shouldn't have to know or care that this became concurrent.
    pthread_mutex_t lock;
} latency_hist_t;

void latency_hist_init(latency_hist_t *h);
void latency_hist_record(latency_hist_t *h, uint64_t ns); // internally thread-safe (item 10c)
// Approximate: returns the upper edge of the bucket containing the p-th
// percentile (0.0 < p <= 1.0), not a true interpolated value.
uint64_t latency_hist_percentile_ns(const latency_hist_t *h, double p);

// Tag must be `metrics` to match region.h's forward declaration
// (`typedef struct metrics metrics_t;`).
typedef struct metrics {
    latency_hist_t handler_latency; // full handle_absent(): ensure_budget + fetch + continue
    uint32_t queue_depth_high_water; // max uffd messages drained between two poll() wakeups
} metrics_t;

void metrics_init(metrics_t *m);

#endif
