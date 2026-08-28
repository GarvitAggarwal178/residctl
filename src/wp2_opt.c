// wp2_opt.c -- WP2 offline optimum for UNEQUAL chunk sizes. Standalone; does
// NOT touch belady.c (per WP2.md's "write it separately and cross-check").
//
// Greedy Belady/MIN generalised to a byte budget: process the declared
// reference string (chunks 0..n-1, repeated n_passes times); on a miss, add
// the missed chunk's byte size to the total and, while it does not fit,
// evict the resident chunk whose next use is furthest in the future (or
// never again). This is the same rule the live pager's select_victim +
// ensure_budget implement; it is optimal for the equal-size case (checked
// below) and the standard approximation for unequal sizes.
//
// usage: wp2_opt <chunk_bytes_file> <budget_bytes> <n_passes> [seq_file]
//   chunk_bytes_file: one chunk length (bytes) per line, in chunk-id order.
//   seq_file (optional): one chunk id per line = ONE pass of the declared
//     access sequence. If omitted, the pass is 0,1,...,n_chunks-1.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s <chunk_bytes_file> <budget_bytes> <n_passes> [seq_file]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    uint64_t cap = 64, n = 0;
    uint64_t *len = malloc(cap * sizeof *len);
    char line[64];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '\n' || line[0] == '#') continue;
        if (n == cap) { cap *= 2; len = realloc(len, cap * sizeof *len); }
        len[n++] = strtoull(line, NULL, 10);
    }
    fclose(f);
    uint64_t budget = strtoull(argv[2], NULL, 10);
    uint32_t passes = (uint32_t)strtoul(argv[3], NULL, 10);
    if (n == 0 || passes == 0) { fprintf(stderr, "empty chunk table or 0 passes\n"); return 1; }

    // one pass of the declared sequence
    uint32_t *pass = NULL; uint64_t pass_len = 0;
    if (argc == 5) {
        FILE *sf = fopen(argv[4], "r");
        if (!sf) { fprintf(stderr, "cannot open seq %s\n", argv[4]); return 1; }
        uint64_t pc = 64; pass = malloc(pc * sizeof *pass);
        char sl[64];
        while (fgets(sl, sizeof sl, sf)) {
            if (sl[0] == '\n' || sl[0] == '#') continue;
            if (pass_len == pc) { pc *= 2; pass = realloc(pass, pc * sizeof *pass); }
            pass[pass_len++] = (uint32_t)strtoul(sl, NULL, 10);
        }
        fclose(sf);
    } else {
        pass_len = n; pass = malloc(n * sizeof *pass);
        for (uint64_t i = 0; i < n; i++) pass[i] = (uint32_t)i;
    }
    if (pass_len == 0) { fprintf(stderr, "empty sequence\n"); return 1; }

    uint64_t total_refs = pass_len * (uint64_t)passes;
    uint32_t *ref = malloc(total_refs * sizeof *ref);
    for (uint64_t i = 0; i < total_refs; i++) ref[i] = pass[i % pass_len];

    // next-occurrence index: next_use[i] = smallest j>i with ref[j]==ref[i], or UINT64_MAX
    uint64_t *next_use = malloc(total_refs * sizeof *next_use);
    uint64_t *last_seen = malloc(n * sizeof *last_seen);
    for (uint64_t c = 0; c < n; c++) last_seen[c] = UINT64_MAX;
    for (uint64_t i = total_refs; i-- > 0; ) {
        next_use[i] = last_seen[ref[i]];
        last_seen[ref[i]] = i;
    }

    uint8_t  *resident = calloc(n, 1);
    uint64_t *cur_nextuse = malloc(n * sizeof *cur_nextuse);
    uint64_t resident_bytes = 0, missed_bytes = 0, misses = 0;

    for (uint64_t i = 0; i < total_refs; i++) {
        uint32_t c = ref[i];
        if (resident[c]) { cur_nextuse[c] = next_use[i]; continue; }
        misses++;
        missed_bytes += len[c];
        if (len[c] > budget) {
            // cannot ever fit -- still counts as a miss every time; nothing to evict for it
            cur_nextuse[c] = next_use[i];
            continue;
        }
        while (resident_bytes + len[c] > budget) {
            // evict furthest-future resident
            uint32_t victim = UINT32_MAX; uint64_t best = 0;
            for (uint32_t k = 0; k < n; k++) {
                if (!resident[k]) continue;
                uint64_t d = cur_nextuse[k]; // UINT64_MAX = never again = best victim
                if (victim == UINT32_MAX || d > best) { victim = k; best = d; }
            }
            if (victim == UINT32_MAX) break; // nothing resident (shouldn't happen if len[c] <= budget)
            resident[victim] = 0;
            resident_bytes -= len[victim];
        }
        resident[c] = 1;
        resident_bytes += len[c];
        cur_nextuse[c] = next_use[i];
    }

    // sanity floors (adapted from belady_main's A-2 check for the unequal case):
    // compulsory = every DISTINCT chunk in the sequence must miss at least once.
    uint8_t *seen = calloc(n, 1);
    uint64_t distinct = 0, sum_distinct = 0;
    for (uint64_t i = 0; i < pass_len; i++) if (!seen[pass[i]]) { seen[pass[i]] = 1; distinct++; sum_distinct += len[pass[i]]; }
    int below = 0;
    if (misses < distinct) { fprintf(stderr, "WP2_OPT FAIL: misses=%llu < distinct chunks=%llu (compulsory)\n",
                              (unsigned long long)misses, (unsigned long long)distinct); below = 1; }
    if (missed_bytes < sum_distinct) { fprintf(stderr, "WP2_OPT FAIL: missed_bytes=%llu < sum(distinct chunk sizes)=%llu\n",
                                  (unsigned long long)missed_bytes, (unsigned long long)sum_distinct); below = 1; }
    uint64_t sum_all = sum_distinct;

    // equal-size cross-check over the sequence's distinct chunks
    int all_equal = 1;
    { uint64_t first = 0; int have = 0;
      for (uint64_t c = 0; c < n; c++) if (seen[c]) { if (!have) { first = len[c]; have = 1; } else if (len[c] != first) all_equal = 0; }
    }
    free(seen);
    if (all_equal && missed_bytes != misses * len[0]) {
        fprintf(stderr, "WP2_OPT FAIL: equal-size cross-check: missed_bytes=%llu != misses*len=%llu\n",
                (unsigned long long)missed_bytes, (unsigned long long)(misses * len[0]));
        below = 1;
    }

    printf("WP2_OPT,n_chunks=%llu,passes=%u,total_refs=%llu,budget_bytes=%llu,"
           "opt_misses=%llu,opt_missed_bytes=%llu,sum_chunk_bytes=%llu,equal_size=%d\n",
           (unsigned long long)n, passes, (unsigned long long)total_refs, (unsigned long long)budget,
           (unsigned long long)misses, (unsigned long long)missed_bytes,
           (unsigned long long)sum_all, all_equal);
    return below ? 1 : 0;
}
