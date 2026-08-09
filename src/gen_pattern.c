// gen_pattern.c -- writes a position-derived byte pattern to a file, used by
// test_pager.c (and later the §13 T-1 data-integrity test) to verify fetched
// data instead of just checking "did it crash."
// Usage: gen_pattern <path> <size_bytes>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <path> <size_bytes>\n", argv[0]); return 2; }
    uint64_t size = strtoull(argv[2], NULL, 10);
    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror("fopen"); return 1; }
    const uint64_t BUFN = 1 << 20;
    uint8_t *buf = malloc(BUFN);
    uint64_t written = 0;
    while (written < size) {
        uint64_t n = (size - written) < BUFN ? (size - written) : BUFN;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t off = written + i;
            buf[i] = (uint8_t)(off ^ (off >> 8) ^ (off >> 16));
        }
        fwrite(buf, 1, n, f);
        written += n;
    }
    free(buf);
    fclose(f);
    return 0;
}
