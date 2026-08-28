// pager.c -- MECHANISM_SPEC.md §5 handler loop + state machine.
#define _GNU_SOURCE
#include "pager.h"
#include "fetch.h"
#include "budget.h"
#include "trace.h"
#include "metrics.h"
#include "policy.h"
#include "prefetch.h"
#include "prefetch_pool.h"
#include "fetch_trace.h"

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

// CLEANUP session (7th concurrency-class fix, part a). Caller holds c->lock.
void pager_abandon_fetch(region_t *r, chunk_t *c) {
    c->state = CHUNK_ABSENT;
    c->fetching_since_ns = 0;
    struct uffdio_range range;
    range.start = (unsigned long)(r->map_a + c->region_off);
    range.len = c->len;
    // UFFDIO_WAKE unblocks any thread that faulted on this range and whose
    // message the dispatcher dropped (handle_fault's CHUNK_FETCHING branch)
    // expecting a CONTINUE that will now never come. EAGAIN / harmless races
    // (the fault may have been resolved already) are ignored.
    if (ioctl(r->uffd, UFFDIO_WAKE, &range) != 0 && errno != EAGAIN)
        fprintf(stderr, "pager_abandon_fetch: UFFDIO_WAKE failed (region_off=%llu): %s\n",
                (unsigned long long)c->region_off, strerror(errno));
}

static void handle_absent(region_t *r, chunk_t *c, uint8_t trace_fault_type) {
    uint64_t t_start = now_ns();

    c->state = CHUNK_FETCHING;
    c->fetching_since_ns = t_start;
    // Task B: fault_seq is shared with prefetch_pool workers once depth>1,
    // so this must be a genuine atomic increment, not `++r->fault_seq`
    // (a plain non-atomic read-modify-write would race). Capture the
    // returned value rather than re-reading r->fault_seq afterward, so a
    // concurrent increment from another thread can't be attributed to
    // this chunk's trace record.
    uint64_t seq = __sync_add_and_fetch(&r->fault_seq, 1);
    c->last_fault_seq = seq;
    if (r->trace)
        trace_record(r->trace, seq, (uint32_t)(c - r->chunks), trace_fault_type, 0 /* was_prefetched, item 8 */);
    if (r->policy && r->policy->on_fault) {
        // Task B: on_fault is the only WRITER into a policy's internal
        // state (e.g. layer_order's successor table); predict_next is a
        // reader that prefetch_pool workers may call concurrently once
        // depth>1. budget_lock serializes all policy access uniformly
        // (see prefetch_pool.c) so this write can't race those reads.
        pthread_mutex_lock(&r->budget_lock);
        r->policy->on_fault(r, c);
        pthread_mutex_unlock(&r->budget_lock);
    }
    int budget_rc = ensure_budget(r, c->len);
    if (budget_rc != 0 && r->prefetch_depth > 1) {
        // Task B only: a REAL fault's ensure_budget() can go infeasible not
        // because the workload genuinely doesn't fit, but because prefetch
        // workers raced ahead and reserved the remaining headroom via
        // reserved_bytes before anything they're fetching became actually
        // resident (evictable). Unlike a dropped prefetch (harmless -- item
        // 8's design already treats that as fine), a REAL fault going
        // infeasible leaves the faulting thread blocked forever with
        // nothing that will ever wake it -- reproduced as a genuine
        // deadlock (driver thread stuck in handle_userfault, pager thread
        // idle, having already dropped the only message that could unblock
        // it) at --prefetch-depth 8. Bounded retry: reserved bytes are
        // always transient (every prefetch either commits -- becoming a
        // real, evictable resident -- or gets dropped -- releasing its
        // reservation -- within one fetch's duration), so a short wait
        // should let a subsequent ensure_budget() call succeed. Does not
        // change depth==1 behavior at all (this whole branch is
        // unreachable there).
        for (int attempt = 0; attempt < 200 && budget_rc != 0; attempt++) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 }; // 2ms
            nanosleep(&ts, NULL);
            budget_rc = ensure_budget(r, c->len);
        }
    }
    if (budget_rc != 0) {
        // Infeasible at this budget (§7/§11's censoring rule, not a bug --
        // reconcile() already ran inside ensure_budget and would have
        // aborted if OUR accounting disagreed with the kernel's). Revert to
        // ABSENT so a later fault can retry once budget frees up.
        // CLEANUP session (part a): also UFFDIO_WAKE -- under the async
        // handler a faulter that deduped against this FETCHING episode is
        // otherwise never woken (the old "the faulting thread stays blocked"
        // comment was correct only for the SYNC handler that returned the
        // fault so a later touch could re-trigger handle_absent; the async
        // dispatcher already read the message and there is no "later touch").
        pager_abandon_fetch(r, c);
        return;
    }
    fetch_timing_t timing;
    fetch_chunk(r, c, r->diag_fetch_trace ? &timing : NULL); // §6, lock held throughout (I-6)
    // Task B: commit BEFORE marking RESIDENT, not after. commit_reserved()
    // needs budget_lock; if state became RESIDENT first, another thread's
    // ensure_budget() could pick THIS chunk as a victim (it now looks
    // evictable) and block acquiring c->lock (via evict_chunk) while
    // holding budget_lock -- exactly while this thread, still holding
    // c->lock, blocks acquiring budget_lock via commit_reserved. Classic
    // circular wait. Committing first means the chunk isn't a viable
    // victim (still FETCHING) for the entire time this thread might want
    // budget_lock, breaking the cycle. Found by a genuine hang at
    // --prefetch-depth 8, not reasoned out in advance.
    // Task B: pin c atomically with the commit (commit_reserved_and_pin),
    // not as a separate later step -- see budget.c for why the two must be
    // one critical section. c stays pinned for the rest of this function
    // (which may still need budget_lock via prefetch_pool_top_up's
    // predict_next calls, while still holding c->lock) and is released
    // right before returning.
    commit_reserved_and_pin(r, c, c->len);
    c->state = CHUNK_RESIDENT;
    c->fetching_since_ns = 0; // CLEANUP session: left FETCHING
    r->stat_bytes_fetched += c->len; // Defect 3: pager's own byte accounting
    if (r->policy && r->policy->on_resident) r->policy->on_resident(r, c);
    if (r->prefetch_enabled) {
        if (r->prefetch_depth <= 1) maybe_prefetch(r, c);          // item 8's original inline path, unchanged
        else prefetch_pool_top_up(r, c);                            // Task B: worker pool
    }
    unpin_chunk(r, c);
    r->stat_absent_handled++;

    uint64_t t_exit = now_ns();
    if (r->metrics)
        latency_hist_record(&r->metrics->handler_latency, t_exit - t_start);
    if (r->diag_fetch_trace) {
        fetch_trace_record_t *rec = fetch_trace_reserve(r->diag_fetch_trace);
        rec->chunk_id = (uint32_t)(c - r->chunks);
        rec->was_prefetch = 0;
        rec->t_handler_entry_ns = t_start;
        rec->t_read_start_ns = timing.read_start_ns;
        rec->t_read_end_ns = timing.read_end_ns;
        rec->t_continue_end_ns = timing.continue_end_ns;
        rec->t_handler_exit_ns = t_exit;
        rec->bytes_read = c->len;
    }
}

// item 10c (A-5): the ASYNC handler's ABSENT case. Does the minimum work
// needed to make the chunk's state correct and observable -- mark
// CHUNK_FETCHING, stamp the fault sequence, record the fault trace, run the
// policy's on_fault hook (a bookkeeping write, not I/O) -- then enqueues the
// actual fetch on the shared pool and returns immediately. NO ensure_budget,
// NO fetch_chunk, NO ioctl: nothing in this function can block on I/O. This
// is what makes CHUNK_FETCHING actually observable by concurrent faulters
// (item 10b Task C proved it was NOT observable under the old synchronous
// design) and lets pager_run() read the next uffd message immediately
// instead of sitting behind one blocking pread.
static void handle_absent_dispatch(region_t *r, chunk_t *c, uint8_t trace_fault_type) {
    uint64_t t_entry = now_ns(); // item 10d Task B: dispatch latency, entry to enqueue (diagnostic)
    c->state = CHUNK_FETCHING;
    c->fetching_since_ns = t_entry; // CLEANUP session: FETCHING watchdog clock
    uint64_t seq = __sync_add_and_fetch(&r->fault_seq, 1); // shared with fetch-pool workers; see budget.c note
    c->last_fault_seq = seq;
    if (r->trace)
        trace_record(r->trace, seq, (uint32_t)(c - r->chunks), trace_fault_type, 0 /* was_prefetched */);
    if (r->policy && r->policy->on_fault) {
        // on_fault is the only WRITER into a policy's internal state (e.g.
        // layer_order's successor table); predict_next is a reader that
        // fetch-pool workers call concurrently. budget_lock serializes all
        // policy access uniformly (same reasoning as item 10b's Task B
        // note, now the common case rather than the depth>1-only case).
        pthread_mutex_lock(&r->budget_lock);
        r->policy->on_fault(r, c);
        pthread_mutex_unlock(&r->budget_lock);
    }
    // Enqueue and return -- do NOT wait for the fetch. The faulting
    // thread(s) stay blocked in the kernel on this address until whichever
    // worker picks this job up issues UFFDIO_CONTINUE; that's the kernel's
    // job, not this dispatcher's.
    prefetch_pool_enqueue_demand(r->prefetch_pool_handle, (uint32_t)(c - r->chunks));
    // item 10d Task B: stamped AFTER enqueue, still under c->lock (held by
    // handle_fault throughout) -- the worker that eventually picks this
    // chunk up re-locks c->lock before reading these, so there's no race
    // despite the lock being released and reacquired in between.
    c->diag_dispatch_entry_ns = t_entry;
    c->diag_dispatch_enqueue_ns = now_ns();
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
            // item 10c (A-5): async_handler (the default) dispatches only
            // and never blocks on I/O here; --sync-handler keeps calling
            // handle_absent() inline, exactly as items 2-10b always did,
            // for A/B comparison.
            if (r->async_handler) handle_absent_dispatch(r, c, trace_fault_type);
            else handle_absent(r, c, trace_fault_type);
            break;
    }
    pthread_mutex_unlock(&c->lock);
}

// CLEANUP session (7th concurrency-class fix, part b): the FETCHING-state
// watchdog. Runs on every dispatch-loop iteration (never blocks -- A-5). A
// chunk that has been CHUNK_FETCHING for longer than r->fetching_timeout_ms
// with NO worker in its fetch critical section (c->lock free -- checked via
// trylock, so this itself never blocks) is an orphaned slot: some path set it
// FETCHING and neither completed the fetch nor reset the state. Reclaim it --
// reset to ABSENT, wake any deduped faulter, count it. Part (a) should make
// this never fire; if stat_fetching_timeout > 0 in a run, part (a) missed a
// path and the offending chunk id is logged here.
static void pager_fetching_watchdog(region_t *r) {
    uint32_t timeout_ms = r->fetching_timeout_ms ? r->fetching_timeout_ms : 30000;
    uint64_t now = now_ns();
    uint64_t timeout_ns = (uint64_t)timeout_ms * 1000000ULL;
    for (uint32_t i = 0; i < r->n_chunks; i++) {
        chunk_t *c = &r->chunks[i];
        // Cheap unlocked pre-check -- both fields are plain scalars; a stale
        // read only costs us one extra loop iteration before we act.
        if (c->state != CHUNK_FETCHING) continue;
        uint64_t since = c->fetching_since_ns;
        if (since == 0 || now - since < timeout_ns) continue;
        if (pthread_mutex_trylock(&c->lock) != 0)
            continue; // a worker owns c->lock => a real fetch is in progress => not orphaned
        if (c->state == CHUNK_FETCHING && c->fetching_since_ns != 0 &&
            now - c->fetching_since_ns >= timeout_ns) {
            fprintf(stderr, "pager: FETCHING watchdog reclaimed orphaned chunk %u "
                    "(region_off=%llu len=%llu, stuck %.1fs) -- part (a) missed a reset path\n",
                    i, (unsigned long long)c->region_off, (unsigned long long)c->len,
                    (double)(now - c->fetching_since_ns) / 1e9);
            pager_abandon_fetch(r, c);          // -> ABSENT, fetching_since_ns=0, UFFDIO_WAKE
            __sync_fetch_and_add(&r->stat_fetching_timeout, 1);
        }
        pthread_mutex_unlock(&c->lock);
    }
}

void pager_run(region_t *r, volatile sig_atomic_t *stop, int poll_timeout_ms) {
    struct pollfd pfd = { .fd = r->uffd, .events = POLLIN };

    // item 10c (A-5/A-7): the async handler's shared fetch pool is owned by
    // this function's lifetime -- demand fetches need it unconditionally
    // (not just when prefetch is enabled), so "run the handler" now means
    // "run the dispatcher loop plus its worker pool," started/stopped
    // together. The --sync-handler path (async_handler==false) leaves
    // r->prefetch_pool_handle exactly as item 10b left it: NULL unless the
    // caller (e.g. replay_main.c) started one itself for prefetch_depth>1,
    // and this function never touches its lifecycle.
    if (r->async_handler)
        prefetch_pool_start(r, r->fetch_workers);

    while (!*stop) {
        // CLEANUP session (part b): FETCHING watchdog on every iteration --
        // before poll (catches an orphan while the queue is quiet) and, via
        // the loop, after every drain batch too. Never blocks (trylock).
        if (r->async_handler) pager_fetching_watchdog(r);

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

    if (r->async_handler) {
        // Stop AFTER the dispatch loop exits (no more callers will enqueue
        // anything), and BEFORE this function returns -- callers (e.g.
        // replay_main.c) join this thread and then flush the fetch trace,
        // assuming every worker that could call fetch_trace_reserve() has
        // already been joined by the time pager_run() returns.
        prefetch_pool_stop(r->prefetch_pool_handle);
        r->prefetch_pool_handle = NULL;
    }
}

void pager_notify_access(region_t *r, chunk_t *c) {
    // WP1 (A-12): the workload's consumption signal also advances
    // layer_order_declared's position in the declared sequence. Fired for
    // every reference regardless of --prefetch-retention mode -- the
    // position must track the workload whether or not retention is on.
    if (r->policy && r->policy->on_access) {
        pthread_mutex_lock(&r->budget_lock);
        r->policy->on_access(r, c);
        pthread_mutex_unlock(&r->budget_lock);
    }
    if (!r->prefetch_retention_pinned) return; // --prefetch-retention none: nothing is ever retained
    pthread_mutex_lock(&r->budget_lock);
    prefetch_retain_release(r, (uint32_t)(c - r->chunks));
    pthread_mutex_unlock(&r->budget_lock);
}
