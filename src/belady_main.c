// belady_main.c -- build-order item 9: standalone offline optimal solver
// binary (§10). Reads a GROUND-TRUTH REFERENCE trace (item 5's format,
// amended by item 10's correction, A-1) and reports the minimum achievable
// fetch count/bytes for a given budget.
//
// ITEM 10 CORRECTION, DEFECT 1: this binary used to load a pager-authored
// FAULT trace and run Belady over it. That is circular: a pager only ever
// observes misses (a hit generates no uffd event at all), so a fault trace
// is a function of whichever policy produced it, not the workload's true
// reference string. Running Belady over "the misses one particular policy
// happened to produce" computes "the best you could do given those misses,"
// which is not a valid lower bound on anything. Proof this was happening:
// V1's reported OPT values were BELOW the mathematically provable cyclic
// floor (see detect_cyclic_scan_floor() below) at every single budget
// ratio tested -- an impossible result, meaning the solver's INPUT was
// wrong, not (necessarily) the algorithm.
//
// Fix: this binary now REQUIRES a TRACE_TYPE_REFERENCE file (written by the
// WORKLOAD -- replay_cyclic(), see replay.c -- which knows the true access
// sequence regardless of hit/miss) and ABORTS if handed a TRACE_TYPE_FAULT
// file. Fault traces remain useful (metrics, dedup/prefetch accounting)
// but are never solver input; see trace.h.
//
// A second, independent gate: if the loaded reference string is a strict
// cyclic scan (the same permutation of chunk ids repeated verbatim), the
// result is checked against a provable lower bound (Defect 1's floor
// formula) and this binary ABORTS if the solver reports fewer misses than
// that floor -- an exact, provable check, replacing §10's original "must
// return approximately (1-r)*W" sanity check, which item 9 already found
// to rest on a flawed derivation (see CLAUDE.md item 9's note) and was too
// weak to catch this defect in the first place.
//
// Usage: belady_main <reference_trace_path> <chunk_size_bytes> <budget_bytes>
//        belady_main --selftest
#define _GNU_SOURCE
#include "belady.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// Loads a TRACE_TYPE_REFERENCE file. Aborts (not just returns an error) if
// the header is missing/malformed, or -- the critical check -- if the file
// is a TRACE_TYPE_FAULT trace. This must be impossible to get wrong by
// accident, per the correction's explicit instruction.
static uint32_t *load_reference_string(const char *trace_path, uint64_t *out_n) {
    int fd = open(trace_path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "cannot open %s\n", trace_path); exit(1); }

    uint8_t trace_type;
    if (trace_read_header(fd, &trace_type) != 0) {
        fprintf(stderr, "SOLVER FAILED: %s has no valid trace header (bad magic/version) -- "
                        "refusing to guess the format\n", trace_path);
        abort();
    }
    if (trace_type == TRACE_TYPE_FAULT) {
        fprintf(stderr,
                "SOLVER FAILED: %s is a TRACE_TYPE_FAULT trace (pager-authored, misses only). "
                "This is Defect 1 from the item 10 correction: a fault trace is a function of "
                "whichever policy produced it, not the workload's true reference string, and "
                "feeding it to Belady computes a circular, invalid bound. This solver requires "
                "a TRACE_TYPE_REFERENCE trace, written by the WORKLOAD (replay_cyclic with a "
                "ref_trace argument), not the pager. Aborting.\n", trace_path);
        abort();
    }
    if (trace_type != TRACE_TYPE_REFERENCE) {
        fprintf(stderr, "SOLVER FAILED: %s has unrecognized trace_type=%u\n", trace_path, trace_type);
        abort();
    }

    uint64_t cap = 1024, n = 0;
    uint32_t *ref = malloc(cap * sizeof(uint32_t));
    trace_record_t rec;
    while (read(fd, &rec, sizeof rec) == (ssize_t)sizeof rec) {
        if (n == cap) { cap *= 2; ref = realloc(ref, cap * sizeof(uint32_t)); }
        ref[n++] = rec.chunk_id;
    }
    close(fd);

    fprintf(stderr, "loaded %llu ground-truth references from %s (TRACE_TYPE_REFERENCE)\n",
            (unsigned long long)n, trace_path);
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

// Detects whether ref[] is a strict cyclic scan: the same permutation of
// n_candidate distinct chunk ids, repeated verbatim for the whole length.
// Returns 1 and sets *out_n/*out_passes if so, 0 otherwise (not periodic,
// not a clean permutation, or n_refs isn't an exact multiple of the
// period).
static int detect_cyclic_scan(const uint32_t *ref, uint64_t n_refs, uint32_t *out_n, uint32_t *out_passes) {
    if (n_refs == 0) return 0;
    uint32_t max_id = 0;
    for (uint64_t i = 0; i < n_refs; i++) if (ref[i] > max_id) max_id = ref[i];
    uint32_t n_candidate = max_id + 1;
    if (n_candidate == 0 || n_refs % n_candidate != 0) return 0;
    uint32_t passes = (uint32_t)(n_refs / n_candidate);
    if (passes < 1) return 0;

    for (uint64_t i = 0; i < n_refs; i++)
        if (ref[i] != ref[i % n_candidate]) return 0;

    uint8_t *seen = calloc(n_candidate, 1);
    int ok = 1;
    for (uint32_t i = 0; i < n_candidate; i++) {
        if (ref[i] >= n_candidate || seen[ref[i]]) { ok = 0; break; }
        seen[ref[i]] = 1;
    }
    free(seen);
    if (!ok) return 0;

    *out_n = n_candidate;
    *out_passes = passes;
    return 1;
}

// Exact, provable lower bound for a strict cyclic scan of n distinct items
// visited `passes` times with cache capacity k: the first pass is n
// compulsory misses (nothing is cached yet); after that, going into any
// subsequent pass at most k items can possibly be resident, so at least
// (n-k) of that pass's n references must miss. This is a LOWER BOUND, not
// necessarily tight -- item 9's own measurements found the true optimum can
// exceed it (see CLAUDE.md) -- but any result below it is a mathematical
// impossibility and proves the solver (or, as in V1, its input) is wrong.
static uint64_t cyclic_floor(uint32_t n, uint32_t passes, uint32_t k) {
    uint64_t extra_per_pass = (k < n) ? (uint64_t)(n - k) : 0;
    return (uint64_t)n + (uint64_t)(passes - 1) * extra_per_pass;
}

// Runs the exact-floor check unconditionally, aborting on failure. Returns
// 1 if the cyclic-scan case applied (and passed), 0 if the trace wasn't a
// detectable cyclic scan (no check performed, not a failure).
static int check_cyclic_floor_or_abort(const uint32_t *ref, uint64_t n_refs, uint32_t capacity, uint64_t misses) {
    uint32_t n, passes;
    if (!detect_cyclic_scan(ref, n_refs, &n, &passes)) return 0;
    uint64_t floor = cyclic_floor(n, passes, capacity);
    printf("cyclic-scan floor check: n=%u passes=%u capacity=%u floor=%llu actual_misses=%llu -- %s\n",
           n, passes, capacity, (unsigned long long)floor, (unsigned long long)misses,
           misses >= floor ? "OK" : "BELOW FLOOR, IMPOSSIBLE");
    if (misses < floor) {
        fprintf(stderr,
                "SOLVER FAILED (A-2 exact floor check): detected a strict cyclic scan "
                "(n=%u distinct chunks, %u passes, capacity=%u), computed provable floor=%llu "
                "misses, but the solver reported %llu misses, which is BELOW the floor -- "
                "mathematically impossible. Per the spec's own instruction, this means the "
                "solver is wrong (or was fed a corrupted/inconsistent input) -- aborting rather "
                "than reporting an impossible number.\n",
                n, passes, capacity, (unsigned long long)floor, (unsigned long long)misses);
        abort();
    }
    return 1;
}

static int run_selftest(void) {
    int all_ok = 1;

    printf("=== belady self-test 1/3: random cross-check vs naive O(n^2) reference ===\n");
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

    printf("=== belady self-test 2/3: exact cyclic floor (A-2), replaces the old weak check ===\n");
    uint32_t W = 20, P = 10;
    double ratios[] = { 0.25, 0.5, 0.75 };
    for (size_t ri = 0; ri < sizeof(ratios) / sizeof(ratios[0]); ri++) {
        uint32_t K = (uint32_t)(ratios[ri] * W);
        uint64_t n_refs = (uint64_t)W * P;
        uint32_t *ref = malloc(n_refs * sizeof(uint32_t));
        for (uint64_t i = 0; i < n_refs; i++) ref[i] = (uint32_t)(i % W);
        belady_result_t res = belady_simulate(ref, n_refs, K);
        int checked = check_cyclic_floor_or_abort(ref, n_refs, K, res.misses);
        if (!checked) { fprintf(stderr, "  FAIL: expected a detectable cyclic scan\n"); all_ok = 0; }
        free(ref);
    }

    printf("=== belady self-test 3/3: reference-vs-fault trace header enforcement ===\n");
    {
        const char *ref_path = "/tmp/belady_selftest_ref.trace";
        const char *fault_path = "/tmp/belady_selftest_fault.trace";
        trace_t *rt = trace_open(ref_path, TRACE_TYPE_REFERENCE);
        for (int i = 0; i < 8; i++) trace_record(rt, (uint64_t)i + 1, (uint32_t)i, TRACE_NA, TRACE_NA);
        trace_close(rt);
        trace_t *ft = trace_open(fault_path, TRACE_TYPE_FAULT);
        for (int i = 0; i < 8; i++) trace_record(ft, (uint64_t)i + 1, (uint32_t)i, TRACE_FAULT_MISSING, 0);
        trace_close(ft);

        int rfd = open(ref_path, O_RDONLY);
        uint8_t t1;
        int ok1 = (trace_read_header(rfd, &t1) == 0 && t1 == TRACE_TYPE_REFERENCE);
        close(rfd);
        int ffd = open(fault_path, O_RDONLY);
        uint8_t t2;
        int ok2 = (trace_read_header(ffd, &t2) == 0 && t2 == TRACE_TYPE_FAULT);
        close(ffd);

        printf("  reference file correctly identified: %s\n", ok1 ? "OK" : "FAIL");
        printf("  fault file correctly identified: %s\n", ok2 ? "OK" : "FAIL");
        if (!ok1 || !ok2) all_ok = 0;
        unlink(ref_path);
        unlink(fault_path);
        // load_reference_string() aborting on a FAULT file is exercised by
        // run_correctness_harness.sh's negative test (a subprocess whose
        // expected exit is SIGABRT), not here -- self-test must not abort.
    }

    printf(all_ok ? "SELFTEST PASS\n" : "SELFTEST FAIL\n");
    return all_ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
        return run_selftest();
    }
    if (argc != 4) {
        fprintf(stderr, "usage: %s <reference_trace_path> <chunk_size_bytes> <budget_bytes>\n"
                        "       %s --selftest\n", argv[0], argv[0]);
        return 2;
    }
    const char *trace_path = argv[1];
    uint64_t chunk_size = strtoull(argv[2], NULL, 10);
    uint64_t budget_bytes = strtoull(argv[3], NULL, 10);
    if (chunk_size == 0) { fprintf(stderr, "chunk_size must be nonzero\n"); return 2; }
    uint32_t capacity = (uint32_t)(budget_bytes / chunk_size);

    uint64_t n_refs;
    uint32_t *ref = load_reference_string(trace_path, &n_refs); // aborts on TRACE_TYPE_FAULT
    if (n_refs == 0) {
        fprintf(stderr, "no references found in trace -- nothing to solve\n");
        return 1;
    }

    belady_result_t res = belady_simulate(ref, n_refs, capacity);

    // A-2: unconditional exact floor check on every real run, not just selftest.
    check_cyclic_floor_or_abort(ref, n_refs, capacity, res.misses);
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
