// metrics.c -- see metrics.h for the honesty caveat on the histogram.
#include "metrics.h"
#include <string.h>

static int bucket_for(uint64_t ns) {
    if (ns == 0) return 0;
    int b = 0;
    uint64_t v = ns;
    while (v > 1) { v >>= 1; b++; }
    if (b >= METRICS_LATENCY_BUCKETS) b = METRICS_LATENCY_BUCKETS - 1;
    return b;
}

void latency_hist_init(latency_hist_t *h) {
    memset(h, 0, sizeof *h);
    h->min_ns = UINT64_MAX;
    pthread_mutex_init(&h->lock, NULL); // after the memset, not before (item 10c)
}

void latency_hist_record(latency_hist_t *h, uint64_t ns) {
    pthread_mutex_lock(&h->lock); // item 10c: multiple fetch-pool workers may call this concurrently
    h->bucket_counts[bucket_for(ns)]++;
    h->count++;
    h->sum_ns += ns;
    if (ns < h->min_ns) h->min_ns = ns;
    if (ns > h->max_ns) h->max_ns = ns;
    pthread_mutex_unlock(&h->lock);
}

uint64_t latency_hist_percentile_ns(const latency_hist_t *h, double p) {
    if (h->count == 0) return 0;
    uint64_t target = (uint64_t)(p * (double)h->count);
    if (target == 0) target = 1;
    uint64_t cum = 0;
    for (int b = 0; b < METRICS_LATENCY_BUCKETS; b++) {
        cum += h->bucket_counts[b];
        if (cum >= target) {
            return (b == 0) ? 1 : (1ULL << (b + 1)) - 1; // upper edge of bucket b
        }
    }
    return h->max_ns;
}

void metrics_init(metrics_t *m) {
    latency_hist_init(&m->handler_latency);
    m->queue_depth_high_water = 0;
}
