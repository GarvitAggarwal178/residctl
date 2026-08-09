// pager.c -- MECHANISM_SPEC.md §5 handler loop + state machine.
#define _GNU_SOURCE
#include "pager.h"
#include "fetch.h"
#include "budget.h"
#include "trace.h"
#include "metrics.h"
#include "policy.h"
#include "prefetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/userfaultfd.h>

chunk_t *pager_lookup(region_t *r, uint64_t rel_off) {
    if (r->n_chunks == 0) return NULL;
    uint32_t lo = 0, hi = r->n_chunks; // [lo, hi)
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        chunk_t *c = &r->chunks[mid];
        if (rel_off < c->region_off) {
            hi = mid;
        } else if (rel_off >= c->region_off + c->len) {
            lo = mid + 1;
        } else {
            return c;
        }
    }
    return NULL;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void handle_absent(region_t *r, chunk_t *c, uint8_t trace_fault_type) {
    uint64_t t_start = now_ns();

    c->state = CHUNK_FETCHING;
    c->last_fault_seq = ++r->fault_seq;
    if (r->trace)
        trace_record(r->trace, r->fault_seq, (uint32_t)(c - r->chunks), trace_fault_type, 0 /* was_prefetched, item 8 */);
    if (r->policy && r->policy->on_fault) r->policy->on_fault(r, c);
    if (ensure_budget(r, c->len) != 0) {
        // Infeasible at this budget (§7/§11's censoring rule, not a bug --
        // reconcile() already ran inside ensure_budget and would have
        // aborted if OUR accounting disagreed with the kernel's). Revert to
        // ABSENT so a later fault can retry once budget frees up; drop this
        // fault message the same way the FETCHING branch does (don't wait,
        // don't issue I/O). The faulting thread stays blocked -- that's the
        // correct, honest behaviour when the workload doesn't fit budget B.
        c->state = CHUNK_ABSENT;
        return;
    }
    fetch_chunk(r, c);            // §6, lock held throughout (I-6)
    c->state = CHUNK_RESIDENT;
    r->resident_bytes += c->len;
    r->stat_bytes_fetched += c->len; // Defect 3: pager's own byte accounting
    if (r->policy && r->policy->on_resident) r->policy->on_resident(r, c);
    if (r->prefetch_enabled) maybe_prefetch(r, c);
    r->stat_absent_handled++;

    if (r->metrics)
        latency_hist_record(&r->metrics->handler_latency, now_ns() - t_start);
}

static void wake_range(region_t *r, chunk_t *c) {
    struct uffdio_range range;
    range.start = (unsigned long)(r->map_a + c->region_off);
    range.len = c->len;
    if (ioctl(r->uffd, UFFDIO_WAKE, &range) != 0 && errno != EAGAIN) {
        // Harmless in practice (the fault may have already been resolved by
        // the CONTINUE that made this chunk RESIDENT), but record loudly if
        // it's an error we don't expect.
        fprintf(stderr, "pager: UFFDIO_WAKE failed (chunk region_off=%llu): %s\n",
                (unsigned long long)c->region_off, strerror(errno));
    }
}

// handle() from §5's pseudocode. Dispatches on chunk STATE, never on fault
// type (I-8) -- the fault type is only ever recorded as a metric.
static void handle_fault(region_t *r, uint64_t fault_addr, bool was_minor) {
    uint8_t trace_fault_type = was_minor ? TRACE_FAULT_MINOR : TRACE_FAULT_MISSING;
    if (was_minor) r->stat_fault_minor++;
    else r->stat_fault_missing++;

    uint64_t rel_off = fault_addr - (uint64_t)(uintptr_t)r->map_a;
    chunk_t *c = pager_lookup(r, rel_off);
    if (!c) {
        fprintf(stderr, "PAGER FAILED: fault at rel_off=%llu has no covering chunk\n",
                (unsigned long long)rel_off);
        abort();
    }

    pthread_mutex_lock(&c->lock);
    switch (c->state) {
        case CHUNK_RESIDENT:
            wake_range(r, c);
            r->stat_dedup_resident++;
            break;
        case CHUNK_FETCHING:
            // Another fetch is in flight; its CONTINUE will wake this
            // thread too. Drop the message: do NOT wait, do NOT issue I/O.
            r->stat_dedup_fetching++;
            break;
        case CHUNK_ABSENT:
            handle_absent(r, c, trace_fault_type);
            break;
    }
    pthread_mutex_unlock(&c->lock);
}

void pager_run(region_t *r, volatile sig_atomic_t *stop, int poll_timeout_ms) {
    struct pollfd pfd = { .fd = r->uffd, .events = POLLIN };

    while (!*stop) {
        int pr = poll(&pfd, 1, poll_timeout_ms); // advisory only (I-2)
        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "PAGER FAILED: poll() failed: %s\n", strerror(errno));
            abort();
        }
        if (pr == 0) continue; // timeout, re-check *stop

        uint32_t drained_this_batch = 0;
        for (;;) {
            struct uffd_msg msg;
            ssize_t n = read(r->uffd, &msg, sizeof msg);
            if (n < 0) {
                if (errno == EAGAIN) break; // THE authoritative empty-queue signal (I-2)
                if (errno == EINTR) continue;
                fprintf(stderr, "PAGER FAILED: read(uffd) failed: %s\n", strerror(errno));
                abort();
            }
            if (n != (ssize_t)sizeof msg) {
                fprintf(stderr, "PAGER FAILED: short read on uffd (%zd bytes)\n", n);
                abort();
            }
            if (msg.event != UFFD_EVENT_PAGEFAULT) {
                fprintf(stderr, "PAGER FAILED: unexpected uffd event 0x%x\n", msg.event);
                abort();
            }
            bool was_minor = (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_MINOR) != 0;
            handle_fault(r, msg.arg.pagefault.address, was_minor);
            drained_this_batch++;
        }
        if (r->metrics && drained_this_batch > r->metrics->queue_depth_high_water)
            r->metrics->queue_depth_high_water = drained_this_batch;
    }
}
