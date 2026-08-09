// region.c -- MECHANISM_SPEC.md §4 startup sequence, steps 1-9 and 11.
// Step 10 (start the handler thread) belongs to build-order item 2 and is
// not performed here.
#define _GNU_SOURCE
#include "region.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <linux/userfaultfd.h>
#include <linux/memfd.h>

static void fail(const char *step, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "STARTUP FAILED at step [%s]: ", step);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

// Reads a small file into buf, strips trailing newline. Returns 0 on success.
static int read_small_file(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    int saved_errno = errno;
    close(fd);
    if (n < 0) { errno = saved_errno; return -1; }
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = '\0';
    return 0;
}

static uint64_t parse_cgroup_uint64_or_max(const char *s) {
    if (strcmp(s, "max") == 0) return UINT64_MAX;
    return strtoull(s, NULL, 10);
}

// §4 step 1: verify cgroup.
static void verify_cgroup(const region_config_t *cfg, run_manifest_t *m) {
    char path[300], val[64];

    snprintf(path, sizeof path, "%s/memory.swap.max", cfg->cgroup_path);
    if (read_small_file(path, val, sizeof val) != 0)
        fail("verify_cgroup", "cannot read %s: %s", path, strerror(errno));
    snprintf(m->memory_swap_max_raw, sizeof m->memory_swap_max_raw, "%s", val);
    if (strcmp(val, "0") != 0)
        fail("verify_cgroup",
             "%s reads '%s', not '0' (I-3). Without this the kernel can swap "
             "out weight pages while our accounting believes them resident "
             "(S3e measured 236 MiB swapped per run when this was left at "
             "default). Fix the cgroup config and rerun; this program will "
             "not proceed with a non-zero swap ceiling.", path, val);

    snprintf(path, sizeof path, "%s/memory.max", cfg->cgroup_path);
    if (read_small_file(path, val, sizeof val) != 0)
        fail("verify_cgroup", "cannot read %s: %s", path, strerror(errno));
    m->memory_max = parse_cgroup_uint64_or_max(val);

    snprintf(path, sizeof path, "%s/memory.high", cfg->cgroup_path);
    if (read_small_file(path, val, sizeof val) == 0 && strcmp(val, "max") != 0) {
        m->memory_high_set = true;
        m->memory_high = parse_cgroup_uint64_or_max(val);
    } else {
        m->memory_high_set = false;
        m->memory_high = 0;
    }
}

// §4 step 2: verify uffd features.
static void verify_uffd_features(region_t *r) {
    long fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        fail("verify_uffd_features", "userfaultfd() failed: %s", strerror(errno));

    struct uffdio_api api;
    memset(&api, 0, sizeof api);
    api.api = UFFD_API;
    api.features = 0;
    if (ioctl(fd, UFFDIO_API, &api) != 0)
        fail("verify_uffd_features", "UFFDIO_API ioctl failed: %s", strerror(errno));

    if (!(api.features & UFFD_FEATURE_MISSING_SHMEM))
        fail("verify_uffd_features",
             "UFFD_FEATURE_MISSING_SHMEM not present (features=0x%llx)",
             (unsigned long long)api.features);
    if (!(api.features & UFFD_FEATURE_MINOR_SHMEM))
        fail("verify_uffd_features",
             "UFFD_FEATURE_MINOR_SHMEM not present (features=0x%llx)",
             (unsigned long long)api.features);

    r->uffd = (int)fd; // O_NONBLOCK per I-2; EAGAIN is the only authoritative empty-queue signal
}

// §4 step 3: create backing store. Deliberately does NOT fallocate the
// whole region -- that would make the entire model resident at startup.
static void create_backing_store(region_t *r, uint64_t region_len) {
    int fd = memfd_create("residctl", MFD_CLOEXEC);
    if (fd < 0)
        fail("create_backing_store", "memfd_create failed: %s", strerror(errno));
    if (ftruncate(fd, (off_t)region_len) != 0)
        fail("create_backing_store", "ftruncate(%llu) failed: %s",
             (unsigned long long)region_len, strerror(errno));
    r->memfd = fd;
    r->region_len = region_len;
}

// §4 step 4: map twice.
static void map_twice(region_t *r) {
    void *b = mmap(NULL, r->region_len, PROT_READ | PROT_WRITE, MAP_SHARED, r->memfd, 0);
    if (b == MAP_FAILED)
        fail("map_twice", "mmap map_b failed: %s", strerror(errno));
    void *a = mmap(NULL, r->region_len, PROT_READ | PROT_WRITE, MAP_SHARED, r->memfd, 0);
    if (a == MAP_FAILED)
        fail("map_twice", "mmap map_a failed: %s", strerror(errno));
    r->map_b = b;
    r->map_a = a;
}

// §4 step 5: THP disabled on both mappings (I-9). Asserted, not assumed --
// this kernel has shmem_enabled=[never] but that's not guaranteed elsewhere.
static void disable_thp(region_t *r) {
    if (madvise(r->map_a, r->region_len, MADV_NOHUGEPAGE) != 0)
        fail("disable_thp", "madvise(MADV_NOHUGEPAGE) on map_a failed: %s", strerror(errno));
    if (madvise(r->map_b, r->region_len, MADV_NOHUGEPAGE) != 0)
        fail("disable_thp", "madvise(MADV_NOHUGEPAGE) on map_b failed: %s", strerror(errno));
}

// §4 step 6: register mapping A only (I-1 depends on map_b staying unregistered).
static void register_mapping_a(region_t *r) {
    struct uffdio_register reg;
    memset(&reg, 0, sizeof reg);
    reg.range.start = (unsigned long)r->map_a;
    reg.range.len = r->region_len;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_MINOR;
    if (ioctl(r->uffd, UFFDIO_REGISTER, &reg) != 0)
        fail("register_mapping_a", "UFFDIO_REGISTER failed: %s", strerror(errno));
    if (!(reg.ioctls & (1ULL << _UFFDIO_CONTINUE)))
        fail("register_mapping_a",
             "UFFDIO_CONTINUE not in the returned ioctls mask (0x%llx) -- "
             "the fetch path (§6.2) depends on this ioctl being available",
             (unsigned long long)reg.ioctls);
}

// §4 step 7: open model file O_DIRECT, falling back to buffered + FADV_DONTNEED.
static void open_model_file(region_t *r, const char *model_path) {
    int fd = open(model_path, O_RDONLY | O_DIRECT);
    if (fd >= 0) {
        r->model_fd = fd;
        r->used_o_direct = true;
        return;
    }
    int direct_errno = errno;
    fd = open(model_path, O_RDONLY);
    if (fd < 0)
        fail("open_model_file",
             "O_DIRECT open failed (%s) and buffered fallback also failed (%s) for %s",
             strerror(direct_errno), strerror(errno), model_path);
    fprintf(stderr,
            "WARNING [open_model_file]: O_DIRECT unavailable for %s (%s); "
            "falling back to buffered I/O + POSIX_FADV_DONTNEED. This changes "
            "the copy count and MUST appear in the run manifest.\n",
            model_path, strerror(direct_errno));
    r->model_fd = fd;
    r->used_o_direct = false;
}

// §4 step 8: build the chunk table.
//
// MECHANISM_SPEC §4 step 8 says "from the model's tensor metadata" -- that
// source doesn't exist yet (llama.cpp integration is build-order item 11,
// out of scope for now). Until then this builds a generic table: fixed-size,
// 4096-aligned chunks with file_off == region_off (identity layout). This
// trivially satisfies the §6.1 alignment requirement (region_off ≡ file_off
// mod 4096) and is a legitimate stand-in for every build-order item up to
// and including the harness (§11) and correctness suite (§13), all of which
// operate on a region + a source file, not on tensor semantics.
static void build_chunk_table(region_t *r, uint64_t requested_chunk_size) {
    uint64_t chunk_size = requested_chunk_size;
    if (chunk_size % RESIDCTL_ALIGN != 0)
        chunk_size = ((chunk_size / RESIDCTL_ALIGN) + 1) * RESIDCTL_ALIGN;
    if (chunk_size == 0)
        fail("build_chunk_table", "chunk_size rounds to 0");

    uint64_t n = (r->region_len + chunk_size - 1) / chunk_size;
    if (n == 0 || n > UINT32_MAX)
        fail("build_chunk_table", "computed chunk count %llu out of range", (unsigned long long)n);

    chunk_t *chunks = calloc(n, sizeof(chunk_t));
    if (!chunks)
        fail("build_chunk_table", "calloc(%llu chunks) failed", (unsigned long long)n);

    for (uint64_t i = 0; i < n; i++) {
        uint64_t off = i * chunk_size;
        uint64_t len = chunk_size;
        if (off + len > r->region_len) len = r->region_len - off;
        chunks[i].file_off = off;
        chunks[i].region_off = off;
        chunks[i].len = len;
        chunks[i].layer_id = (uint32_t)i;
        chunks[i].state = CHUNK_ABSENT;
        chunks[i].pin = 0;
        chunks[i].last_fault_seq = 0;
        if (pthread_mutex_init(&chunks[i].lock, NULL) != 0)
            fail("build_chunk_table", "pthread_mutex_init failed at chunk %llu", (unsigned long long)i);
        if (pthread_cond_init(&chunks[i].cv, NULL) != 0)
            fail("build_chunk_table", "pthread_cond_init failed at chunk %llu", (unsigned long long)i);
    }

    r->chunks = chunks;
    r->n_chunks = (uint32_t)n;
}

// §4 step 9: compute the budget.
//
// The spec's auto-compute formula (budget = memory.max - N_peak - margin)
// requires N_peak from a pilot run, which does not exist yet -- no workload
// has been built (that's items 2-6). Rather than fabricate an N_peak, this
// requires an explicit budget_bytes from the caller and aborts otherwise.
// The auto-compute path is deferred to whichever later item first has a
// pilot run to measure N_peak from; do not guess it here.
static void compute_budget(region_t *r, const region_config_t *cfg, const run_manifest_t *m) {
    if (cfg->budget_bytes == 0)
        fail("compute_budget",
             "auto-budget (memory.max - N_peak - margin) is not implemented: "
             "N_peak requires a pilot run that doesn't exist yet at build-order "
             "item 1. Pass an explicit region_config_t.budget_bytes.");
    if (m->memory_max != UINT64_MAX && cfg->budget_bytes > m->memory_max)
        fprintf(stderr,
                "WARNING [compute_budget]: requested budget %llu exceeds "
                "cgroup memory.max %llu; ensure_budget will still enforce "
                "this budget, but OOM is possible if the pager's own "
                "footprint isn't accounted for elsewhere.\n",
                (unsigned long long)cfg->budget_bytes, (unsigned long long)m->memory_max);
    r->budget_bytes = cfg->budget_bytes;
    r->resident_bytes = 0;
    r->known_overhead_bytes = 0; // no pager-owned shmem allocations yet at item 1
    r->reconcile_interval = cfg->reconcile_interval ? cfg->reconcile_interval : 16; // A-3
    r->fetches_since_reconcile = 0;
}

static void read_shmem_enabled(char *out, size_t outsz) {
    if (read_small_file("/sys/kernel/mm/transparent_hugepage/shmem_enabled", out, outsz) != 0)
        snprintf(out, outsz, "unknown");
}

int region_startup(region_t *r, const region_config_t *cfg, run_manifest_t *manifest_out) {
    memset(r, 0, sizeof *r);
    run_manifest_t m;
    memset(&m, 0, sizeof m);

    if (cfg->region_len == 0)
        fail("region_startup", "region_len must be nonzero");
    if (!cfg->model_path || !cfg->cgroup_path)
        fail("region_startup", "model_path and cgroup_path are required");

    snprintf(r->cgroup_path, sizeof r->cgroup_path, "%s", cfg->cgroup_path);
    snprintf(m.cgroup_path, sizeof m.cgroup_path, "%s", cfg->cgroup_path);
    snprintf(m.model_path, sizeof m.model_path, "%s", cfg->model_path);

    struct utsname uts;
    if (uname(&uts) == 0)
        snprintf(m.kernel_release, sizeof m.kernel_release, "%s", uts.release);
    else
        snprintf(m.kernel_release, sizeof m.kernel_release, "unknown");
    read_shmem_enabled(m.shmem_enabled, sizeof m.shmem_enabled);

    verify_cgroup(cfg, &m);                 // step 1
    verify_uffd_features(r);                // step 2
    create_backing_store(r, cfg->region_len); // step 3
    map_twice(r);                            // step 4
    disable_thp(r);                          // step 5
    register_mapping_a(r);                   // step 6
    open_model_file(r, cfg->model_path);     // step 7
    build_chunk_table(r, cfg->chunk_size);   // step 8
    compute_budget(r, cfg, &m);              // step 9
    // step 10 (start handler thread) intentionally not done here -- item 2.

    m.chunk_size = r->n_chunks > 0 ? r->chunks[0].len : cfg->chunk_size;
    // use the actual rounded size, not chunk 0's possibly-truncated len, for
    // the manifest field: recompute the same rounding build_chunk_table did.
    {
        uint64_t cs = cfg->chunk_size;
        if (cs % RESIDCTL_ALIGN != 0) cs = ((cs / RESIDCTL_ALIGN) + 1) * RESIDCTL_ALIGN;
        m.chunk_size = cs;
    }
    m.used_o_direct = r->used_o_direct;
    m.region_len = r->region_len;
    m.n_chunks = r->n_chunks;

    if (manifest_out) *manifest_out = m;     // step 11 data; region_write_manifest() does the I/O
    return 0;
}

void region_write_manifest(const run_manifest_t *m, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "region_write_manifest: cannot open %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "kernel_release=%s\n", m->kernel_release);
    fprintf(f, "shmem_enabled=%s\n", m->shmem_enabled);
    if (m->memory_max == UINT64_MAX) fprintf(f, "memory_max=max\n");
    else fprintf(f, "memory_max=%llu\n", (unsigned long long)m->memory_max);
    fprintf(f, "memory_swap_max=%s\n", m->memory_swap_max_raw);
    if (m->memory_high_set) fprintf(f, "memory_high=%llu\n", (unsigned long long)m->memory_high);
    else fprintf(f, "memory_high=max\n");
    fprintf(f, "chunk_size=%llu\n", (unsigned long long)m->chunk_size);
    fprintf(f, "used_o_direct=%s\n", m->used_o_direct ? "yes" : "no");
    fprintf(f, "region_len=%llu\n", (unsigned long long)m->region_len);
    fprintf(f, "n_chunks=%u\n", m->n_chunks);
    fprintf(f, "model_path=%s\n", m->model_path);
    fprintf(f, "cgroup_path=%s\n", m->cgroup_path);
    fclose(f);
}

void region_teardown(region_t *r) {
    if (!r) return;
    if (r->chunks) {
        for (uint32_t i = 0; i < r->n_chunks; i++) {
            pthread_mutex_destroy(&r->chunks[i].lock);
            pthread_cond_destroy(&r->chunks[i].cv);
        }
        free(r->chunks);
        r->chunks = NULL;
    }
    if (r->map_a && r->map_a != MAP_FAILED) munmap(r->map_a, r->region_len);
    if (r->map_b && r->map_b != MAP_FAILED) munmap(r->map_b, r->region_len);
    if (r->model_fd > 0) close(r->model_fd);
    if (r->memfd > 0) close(r->memfd);
    if (r->uffd > 0) close(r->uffd);
    memset(r, 0, sizeof *r);
}
