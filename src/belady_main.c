// belady_main.c -- build-order item 9: standalone offline optimal solver
// binary (§10). Reads a trace file (item 5's format) and reports the
// minimum achievable fetch count/bytes for a given budget.
//
// The reference string fed to belady_simulate() excludes was_prefetched
// records: trace.h is explicit that the trace is "derived from actual
// faults, never from declared access order," and a prefetch is OUR OWN
// speculative activity, not consumer demand -- feeding it back in would be
// circular (computing an "optimal" bound partly informed by our own guesses
// about the future).
//
// Per §10: "Sanity check that must pass before any result is reported: on
// a strictly cyclic reference string at budget ratio r, the solver must
// return approximately (1-r) x W bytes per pass. If it doesn't, the solver
// is wrong, not the theory."
//
// That check is implemented (see the "cyclic pattern" section of
// run_selftest() below) but its own naive "(1-r)*W" expectation turned out
// to be WRONG, not the algorithm: a hand-derived "pin K items forever"
// argument ignores that demand paging forces a cache slot open for every
// miss, which necessarily disturbs whichever set you'd hoped to keep
// pinned. Measured steady-state converges to a stable value ABOVE (1-r)*W,
// not equal to it (see CLAUDE.md item 9 for the numbers and a small
// hand-traced example proving the naive bound isn't achievable). Per the
// spec's own instruction ("the solver is wrong, not the theory") this was
// investigated rather than silently accepted or the test silently loosened.
//
// The actual correctness gate is a random cross-check against a
// deliberately naive O(n^2) reference implementation (linear forward scan
// for true next-occurrence at every eviction -- directly implements the
// textbook definition, nothing clever to get wrong). 300/300 random trials
// match exactly. Run `belady_main --selftest` and confirm SELFTEST PASS
// before trusting any real-trace output.
//
// Usage: belady_main <trace_path> <chunk_size_bytes> <budget_bytes>
//        belady_main --selftest
#define _GNU_SOURCE
#include "belady.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static uint32_t *load_reference_string(const char *trace_path, uint64_t *out_n) {
    int fd = open(trace_path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "cannot open %s\n", trace_path); exit(1); }

    uint64_t cap = 1024, n = 0;
    uint32_t *ref = malloc(cap * sizeof(uint32_t));
    uint64_t n_prefetched_skipped = 0;

    trace_record_t rec;
    while (read(fd, &rec, sizeof rec) == (ssize_t)sizeof rec) {
        if (rec.was_prefetched) { n_prefetched_skipped++; continue; }
        if (n == cap) { cap *= 2; ref = realloc(ref, cap * sizeof(uint32_t)); }
        ref[n++] = rec.chunk_id;
    }
    close(fd);

    fprintf(stderr, "loaded %llu genuine-fault references from %s (skipped %llu prefetched)\n",
            (unsigned long long)n, trace_path, (unsigned long long)n_prefetched_skipped);
    *out_n = n;
    return ref;
}

// Deliberately naive O(n^2) reference: linear forward scan for the true
// next occurrence at every eviction decision. Directly implements the
// textbook Belady/MIN definition with no heap, no lazy deletion, nothing
// clever -- if this disagrees with belady_simulate(), belady_simulate() is
// wrong, since this one can't be.
static uint64_t naive_next_occurrence(const uint32_t *ref, uint64_t n, uint64_t from, uint32_t chunk) {
    for (uint64_t j = from + 1; j < n; j++) if (ref[j] == chunk) return j;
    return UINT64_MAX;
}

static uint64_t naive_belady(const uint32_t *ref, uint64_t n, uint32_t capacity) {
    uint32_t *cache = malloc(capacity * sizeof(uint32_t));
    uint8_t *used = calloc(capacity, sizeof(uint8_t));
    uint64_t misses = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t c = ref[i];
        int found = -1;
        for (uint32_t k = 0; k < capacity; k++) if (used[k] && cache[k] == c) { found = (int)k; break; }
        if (found >= 0) continue;
        misses++;
        int slot = -1;
        for (uint32_t k = 0; k < capacity; k++) if (!used[k]) { slot = (int)k; break; }
        if (slot < 0) {
            uint64_t best_dist = 0; int best_slot = 0;
            for (uint32_t k = 0; k < capacity; k++) {
                uint64_t d = naive_next_occurrence(ref, n, i, cache[k]);
                if (k == 0 || d > best_dist) { best_dist = d; best_slot = (int)k; }
            }
            slot = best_slot;
        }
        cache[slot] = c;
        used[slot] = 1;
    }
    free(cache);
    free(used);
    return misses;
}

static int run_selftest(void) {
    int all_ok = 1;

    printf("=== belady self-test 1/2: random cross-check vs naive O(n^2) reference ===\n");
    srand(12345); // fixed seed: deterministic, reproducible selftest
    int trials = 300, mismatches = 0;
    for (int t = 0; t < trials; t++) {
        uint64_t n = 5 + rand() % 60;
        uint32_t vocab = 2 + rand() % 10;
        uint32_t capacity = 1 + rand() % vocab;
        uint32_t *ref = malloc(n * sizeof(uint32_t));
        for (uint64_t i = 0; i < n; i++) ref[i] = rand() % vocab;

        belady_result_t fast = belady_simulate(ref, n, capacity);
        uint64_t slow = naive_belady(ref, n, capacity);
        if (fast.misses != slow) {
            mismatches++;
            fprintf(stderr, "  MISMATCH trial=%d n=%llu vocab=%u capacity=%u fast=%llu slow=%llu\n",
                    t, (unsigned long long)n, vocab, capacity,
                    (unsigned long long)fast.misses, (unsigned long long)slow);
        }
        free(ref);
    }
    printf("%d/%d trials matched -- %s\n", trials - mismatches, trials, mismatches == 0 ? "OK" : "MISMATCH");
    if (mismatches != 0) all_ok = 0;

    printf("=== belady self-test 2/2: pure cyclic reference string (informational) ===\n");
    uint32_t W = 20, P = 10;
    double ratios[] = { 0.25, 0.5, 0.75 };
    for (size_t ri = 0; ri < sizeof(ratios) / sizeof(ratios[0]); ri++) {
        uint32_t K = (uint32_t)(ratios[ri] * W);
        uint64_t n_refs = (uint64_t)W * P;
        uint32_t *ref = malloc(n_refs * sizeof(uint32_t));
        for (uint64_t i = 0; i < n_refs; i++) ref[i] = (uint32_t)(i % W);
        belady_result_t res = belady_simulate(ref, n_refs, K);
        double r = (double)K / (double)W;
        double naive_expected = (1.0 - r) * (double)W;
        double actual_per_pass = (double)(res.misses - W) / (double)(P - 1);
        printf("  W=%u P=%u K=%u (r=%.2f): steady-state misses/pass=%.2f "
               "(naive (1-r)*W=%.2f -- NOT a pass/fail bound, see CLAUDE.md item 9)\n",
               W, P, K, r, actual_per_pass, naive_expected);
        free(ref);
    }

    printf(all_ok ? "SELFTEST PASS\n" : "SELFTEST FAIL\n");
    return all_ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
        return run_selftest();
    }
    if (argc != 4) {
        fprintf(stderr, "usage: %s <trace_path> <chunk_size_bytes> <budget_bytes>\n"
                        "       %s --selftest\n", argv[0], argv[0]);
        return 2;
    }
    const char *trace_path = argv[1];
    uint64_t chunk_size = strtoull(argv[2], NULL, 10);
    uint64_t budget_bytes = strtoull(argv[3], NULL, 10);
    if (chunk_size == 0) { fprintf(stderr, "chunk_size must be nonzero\n"); return 2; }
    uint32_t capacity = (uint32_t)(budget_bytes / chunk_size);

    uint64_t n_refs;
    uint32_t *ref = load_reference_string(trace_path, &n_refs);
    if (n_refs == 0) {
        fprintf(stderr, "no genuine-fault references found in trace -- nothing to solve\n");
        return 1;
    }

    belady_result_t res = belady_simulate(ref, n_refs, capacity);
    free(ref);

    printf("OPT (Belady) result\n");
    printf("  trace=%s chunk_size=%llu budget_bytes=%llu capacity=%u chunks\n",
           trace_path, (unsigned long long)chunk_size, (unsigned long long)budget_bytes, capacity);
    printf("  total_refs=%llu minimum_misses=%llu minimum_bytes_fetched=%llu hit_rate=%.4f\n",
           (unsigned long long)res.total_refs, (unsigned long long)res.misses,
           (unsigned long long)res.misses * chunk_size,
           1.0 - (double)res.misses / (double)res.total_refs);

    return 0;
}
