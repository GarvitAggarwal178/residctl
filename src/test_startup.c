// test_startup.c -- build-order item 1 verification.
//
// Exercises region_startup() end to end (happy path) and confirms that the
// I-3 and step-9 assertions actually fire when violated, rather than just
// trusting the code reads correctly. Run via test/run_item1_tests.sh, which
// sets up the cgroup state and joins this process to it before exec.
//
// Usage: test_startup <happy|bad_budget> <cgroup_path> <model_path> <manifest_out>
#include "region.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <happy|bad_budget> <cgroup_path> <model_path> <manifest_out>\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    const char *cgroup_path = argv[2];
    const char *model_path = argv[3];
    const char *manifest_out = argv[4];

    region_config_t cfg = {
        .region_len = 16ULL * 1024 * 1024,
        .chunk_size = 4ULL * 1024 * 1024,
        .model_path = model_path,
        .cgroup_path = cgroup_path,
        .budget_bytes = 8ULL * 1024 * 1024,
    };

    if (strcmp(mode, "bad_budget") == 0) {
        cfg.budget_bytes = 0; // expect compute_budget() to abort
    } else if (strcmp(mode, "happy") != 0) {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }

    region_t r;
    run_manifest_t m;
    printf("test_startup mode=%s: calling region_startup...\n", mode);
    fflush(stdout);
    region_startup(&r, &cfg, &m); // aborts on any assertion failure -- that IS the test for bad_* modes

    printf("region_startup returned successfully.\n");
    printf("n_chunks=%u chunk[0].len=%llu used_o_direct=%d resident_bytes=%llu budget_bytes=%llu\n",
           r.n_chunks,
           r.n_chunks ? (unsigned long long)r.chunks[0].len : 0,
           r.used_o_direct,
           (unsigned long long)r.resident_bytes,
           (unsigned long long)r.budget_bytes);
    printf("map_a=%p map_b=%p (must differ: I-1) memfd=%d uffd=%d\n",
           (void *)r.map_a, (void *)r.map_b, r.memfd, r.uffd);

    if (r.map_a == r.map_b) {
        fprintf(stderr, "FAIL: map_a == map_b, violates I-1 precondition\n");
        return 1;
    }
    uint64_t sum = 0;
    for (uint32_t i = 0; i < r.n_chunks; i++) sum += r.chunks[i].len;
    if (sum != r.region_len) {
        fprintf(stderr, "FAIL: chunk lens sum to %llu, expected region_len %llu\n",
                (unsigned long long)sum, (unsigned long long)r.region_len);
        return 1;
    }
    for (uint32_t i = 0; i < r.n_chunks; i++) {
        if (r.chunks[i].region_off % RESIDCTL_ALIGN != 0 || r.chunks[i].file_off % RESIDCTL_ALIGN != 0) {
            fprintf(stderr, "FAIL: chunk %u not 4096-aligned\n", i);
            return 1;
        }
        if (r.chunks[i].state != CHUNK_ABSENT) {
            fprintf(stderr, "FAIL: chunk %u not CHUNK_ABSENT at startup\n", i);
            return 1;
        }
    }

    region_write_manifest(&m, manifest_out);
    printf("manifest written to %s\n", manifest_out);

    region_teardown(&r);
    printf("PASS\n");
    return 0;
}
