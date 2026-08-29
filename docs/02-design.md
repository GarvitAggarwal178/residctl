# Design — the residctl mechanism, as it stands

`residctl` gives an application authority over which pages of a large read-only
working set stay resident, using kernel primitives (`userfaultfd`, a
double-mapped `memfd`, `FALLOC_FL_PUNCH_HOLE`, cgroup v2 `memory.swap.max = 0`)
rather than a custom kernel module.

This document describes the mechanism **as it is now**. Every design decision
below was, at some point, made or changed by a measurement; that history — the
14 spec amendments and the seven latent concurrency bugs found along the way —
is in [`design-history.md`](design-history.md), keyed to the experiment record
that established each one. The problem this solves is in
[`01-problem.md`](01-problem.md); how it is measured is in
[`03-methodology.md`](03-methodology.md).

---

## 1. Invariants

Violating any of these produces silently wrong results, not a crash. Each is
enforced by an assertion in code.

- **I-1 — The handler never touches the registered mapping.** All population
  writes go through mapping B (unregistered). A handler read/write through
  mapping A deadlocks the handler against itself.
- **I-2 — The uffd is `O_NONBLOCK`; `EAGAIN` is the only authoritative
  empty-queue signal.** `poll()` readiness is advisory — one `UFFDIO_CONTINUE`
  resolving many waiters dequeues messages a `poll()`-trusting handler was about
  to read. Drain: `read()` until `-1/EAGAIN`.
- **I-3 — `memory.swap.max` must read exactly `0` at startup or init aborts.**
  With swap available the kernel evicts ~236 MiB of the region per run to swap
  while our accounting believes it resident (spike S3e). Abort, do not warn.
- **I-4 — A chunk is never punched while `FETCHING`, and never punched while it
  is the current or a prefetch-target chunk.** The policy respects a pin set;
  the mechanism asserts `pin == 0` in `evict()`. The only exception is the
  demand-fetch pin-break override (§4), which releases the pin itself before
  the assertion runs.
- **I-5 — `UFFDIO_CONTINUE` is issued only after every byte of the chunk exists
  in the memfd.** Loop the read to full length, then assert
  `uffdio_continue.mapped == chunk_len`.
- **I-6 — Chunk state transitions happen only under that chunk's lock; the lock
  is held across the entire fetch (read + `CONTINUE` + state update).**
- **I-7 — Resident-bytes accounting is reconciled against `memory.stat[shmem]`
  on every eviction, and otherwise every `reconcile_interval` fetches (default
  16).** Divergence beyond one chunk aborts. `--eager-reconcile` forces
  `reconcile_interval = 1`; the correctness harness always runs with it forced.
- **I-8 — Fault handling dispatches on chunk *state*, never on fault *type*.**
  Both `MISSING` and `MINOR` occur normally. Record the type as a metric; do
  not branch on it.
- **I-9 — THP is disabled on both mappings via `MADV_NOHUGEPAGE`.** Large-folio
  splitting under hole-punching would make eviction non-deterministic.
- **I-10 — No policy decision is made from data a teammate produced.** The
  harness validates its own inputs.

## 2. Measured constants (from the feasibility spike)

| quantity | measured | consequence |
|---|---|---|
| `O_DIRECT pread`, 150 MiB | 61 ms (2450 MiB/s) | the fetch cost; dominates |
| `UFFDIO_CONTINUE`, 150 MiB | 2.9 ms (50,900 MiB/s) | 4.5 % of a fetch; not a cost centre |
| `UFFDIO_COPY`, 150 MiB | 29.0 ms | +48 % per fetch — rejected |
| buffered `pread`, 150 MiB | 128 ms, 6× spread | rejected; also pollutes page cache |
| hole punch, 400 MiB | exact, no delay | eviction is synchronous |
| max `O_DIRECT` bandwidth | 3396 MiB/s | the "host-cache contamination" threshold |

Optimise the read path and the policy; do not micro-optimise `CONTINUE`.

---

## 3. Architecture

### Startup (`region.c`)

1. Verify the cgroup: `memory.swap.max == "0"` (I-3); record `memory.max`.
2. Verify uffd features: `UFFD_FEATURE_MISSING_SHMEM` and `…_MINOR_SHMEM`.
3. `memfd_create` + `ftruncate(region_len)` — **not** `fallocate` (that would
   make the whole model resident at startup).
4. `mmap` the memfd twice: `map_b` (population), `map_a` (registered). Two
   virtual addresses, one backing object.
5. `madvise(MADV_NOHUGEPAGE)` on both (I-9).
6. `UFFDIO_REGISTER` mapping A with `MISSING | MINOR`.
7. Open the model `O_RDONLY|O_DIRECT` (buffered + `POSIX_FADV_DONTNEED`
   fallback, recorded in the manifest). A second plain buffered fd handles the
   sub-4096 tail of the final chunk.
8. Build the chunk table from the model's tensor metadata. Chunk boundaries are
   aligned to 4096 at construction so `region_off ≡ file_off (mod 4096)` and
   the `O_DIRECT` alignment problem never reaches the hot path.
9. Compute the budget: `budget = memory.max − N_peak − margin`, `margin =
   max(128 MiB, 0.2 · N_peak)`. Pre-allocate all handler scratch now.
10. Start the handler: **a dispatcher thread plus a shared fetch-worker pool.**
    One thread drains the uffd queue and makes state decisions; it never does
    I/O. `--fetch-workers N` (default 4) sizes the pool. `--sync-handler`
    restores the original single-threaded blocking handler for A/B comparison.
11. Write the run manifest (kernel version, `shmem_enabled`, `memory.max`,
    `swap.max`, chunk size, policy, `O_DIRECT` y/n, model hash, git SHA).

### Handler loop (`pager.c`)

```
read message (O_NONBLOCK; EAGAIN = empty, I-2)
chunk = lookup(address)              # binary search on region_off
lock(chunk)
switch chunk->state:
  RESIDENT:  UFFDIO_WAKE; stat_dedup_resident++
  FETCHING:  stat_dedup_fetching++            # drop; the in-flight CONTINUE wakes it
  ABSENT:    chunk->state = FETCHING; fetching_since_ns = now
             chunk->last_fault_seq = atomic_incr(fault_seq)
             fault_trace.record(...)          # METRICS ONLY (never the reference string)
             policy->on_fault(chunk)          # bookkeeping, not I/O
             enqueue(chunk) to the fetch pool # no wait
unlock(chunk)
```

The dispatch loop also **watchdogs the `FETCHING` state**: any chunk `FETCHING`
longer than `fetching_timeout_ms` (default 30 000) with no worker holding its
lock (checked via `trylock`) is an orphaned slot — reset to `ABSENT`, wake
waiters, `stat_fetching_timeout++`. Every `CHUNK_FETCHING → ABSENT` drop path
(infeasible, retry-exhausted, prefetch declined) pairs the transition with
`UFFDIO_WAKE` via `pager_abandon_fetch()`.

Fetch workers dequeue a job, lock the chunk, run `ensure_budget()`, `fetch()`
(read + `CONTINUE` + state update to `RESIDENT`, all under the lock — I-6),
commit the budget reservation **before** marking `RESIDENT`
(`commit_reserved_and_pin()` — the reverse order is a real deadlock),
`policy->on_resident()`, prefetch top-up, unlock. **Demand fetches take strict
priority** — a worker drains the demand queue fully before looking at the
prefetch queue.

### Fetch path (`fetch.c`)

`O_DIRECT` read in a loop into `map_b` until the aligned range is full (short
reads are legal), then `UFFDIO_CONTINUE` over the `map_a` range with `mode = 0`
(wakes waiters), asserting `c.mapped == chunk->len` (I-5).

---

## 4. Eviction and budget (`budget.c`)

`ensure_budget(need)`:

```
reconcile()                                       # I-7
while resident_bytes + need > budget_bytes:
    victim = policy->select_victim()              # chunk_id or NONE
    if victim == NONE:
        if retention == pinned and the pinned-retention FIFO is non-empty:
            break the coldest pinned prefetch target's pin, evict it, stat_pin_broken++
        else:
            stat_infeasible++; return E_INFEASIBLE   # caller records a censored point
    evict(victim)
```

`evict(chunk)`: assert `RESIDENT` and `pin == 0` (I-4), `fallocate(PUNCH_HOLE |
KEEP_SIZE)`, `state = ABSENT`, `resident_bytes -= len`.

`reconcile()`: read `memory.stat[shmem]`, compare to `resident_bytes + known
overhead`. `MAP_SHARED|MAP_ANONYMOUS` allocations are shmem-backed and must be
counted in the overhead term or `reconcile()` false-positives.

**Evicting a chunk a compute thread is reading is *correct*** — the read
faults, the handler refetches, the thread gets right data. It is a performance
and liveness problem, not a correctness one; the pin set exists to prevent the
pathology, not to preserve correctness.

## 5. Prefetch (`prefetch.c`, `prefetch_pool.c`)

On chunk *N* → `RESIDENT`, if the policy predicts a successor, the pool
top-up enqueues the near successors (`--prefetch-depth`, default 2). A
prefetched chunk reaches `RESIDENT` without ever faulting — **this is where the
latency win comes from, and it is counted separately** (it is the D→E delta).

- **Admission** (`--prefetch-admission {always,guarded}`, default `guarded`): a
  prefetch may force an eviction only if the victim's `next_use_distance` is
  strictly greater than the target's. Recorded as a **negative result** — under
  `layer_order` the victim selector already picks the coldest resident chunk,
  so this guard declined 0/36 cells in its sweep; it is close to redundant.
- **Retention** (`--prefetch-retention {none,pinned}`, default `pinned`): a
  prefetch target stays pinned from arrival until the workload signals it was
  consumed (`pager_notify_access()`) or `prefetch_depth` newer targets have
  been pinned ahead of it (a bounded FIFO). `stat_pin_broken > 0` at budget
  ratio *r* is the load-bearing diagnostic that retention is contending with
  demand for budget; the boundary ratio is depth-dependent.
- **Decline backoff**: a prefetch `ensure_budget_prefetch()` declines sets
  `decline_until_ns = now + 100 ms`; the top-up skips candidates still in
  backoff, so a declined prefetch is not re-enqueued to be declined again.

Prefetch is a narrow tool: it trades a few percent more bytes for ~2× fewer
demand faults at a loose budget, and has no byte advantage at a tight one — see
[`findings.md`](../results/findings.md) step 7 and experiments
[`16`](../experiments/16-consumption-signal.md)–[`21`](../experiments/21-livelock-real-model.md).

---

## 6. Policy interface (`policy.c`)

```c
typedef struct {
    const char *name;
    void      (*on_fault)(region_t*, chunk_t*);
    void      (*on_resident)(region_t*, chunk_t*);
    void      (*on_access)(region_t*, chunk_t*);            // workload consumption signal
    void      (*declare_sequence)(region_t*, const uint32_t*, uint32_t);
    uint32_t  (*select_victim)(region_t*);                  // CHUNK_NONE if none evictable
    int32_t   (*predict_next)(region_t*, chunk_t*);         // -1 if no prediction
    int64_t   (*next_use_distance)(region_t*, chunk_t*);    // INT64_MAX if never reached
} policy_t;
```

| policy | victim selection | purpose |
|---|---|---|
| `lru` | max age by `last_fault_seq`; `next_use_distance` always `INT64_MAX` | **control** — the kernel's own rule, run through the pager. Isolates *authority* from *policy*. |
| `layer_order_learned` | furthest next use, from a successor chain built from **fault-dispatch order** (`on_fault`) | the *inferred*-order arm. Retained to measure what declared knowledge is worth. |
| `layer_order_declared` | **(default)** furthest next use, from a **workload-declared sequence** and a consumption position advanced only by `on_access()` — never `on_fault` | the informed policy. `§1` argues the application knows its order in advance; this is the implementation of that claim. |
| `belady` | — | offline only (`belady_main`, `wp2_opt`); never runs live. |

`layer_order_declared` details:

- **`next_use_distance` origin is signal-mode-aware** (`policy_set_signal_mode`).
  On the **post-consumption** path (synthetic `--consumption-signal all-threads`,
  the driver advances the cursor only after every thread finishes a chunk) the
  scan starts at `d = 1`: `seq[pos]` was just consumed, its next use is a full
  cycle away. On the **pre-consumption** path (the real-model eval callback,
  which fires before the layer's weights are read) it starts at `d = 0`:
  `seq[pos]`'s use is imminent, distance 0.
- **`--protect-current`** (`policy_set_protect_current`, **default off on both
  paths**) forces `seq[pos]` and `seq[pos-1]` to distance 0 regardless of mode.
  Retained and unit-tested for a caller whose consumption signal is genuinely
  inexact; redundant when the signal is accurate.
- Chunks absent from the declared sequence fall through to `INT64_MAX`.

## 7. Trace and the offline optimum

Two **non-interchangeable** trace kinds, distinguished by a file header:

- **`TRACE_TYPE_REFERENCE`** — written by the *workload* (replay driver or the
  llama.cpp integration), one record per access, hit or miss, in true order.
  **The only legitimate input to the Belady solver.**
- **`TRACE_TYPE_FAULT`** — what the handler's `ABSENT` branch writes. A record
  of *which references this policy missed on* — circular if fed to the solver.
  Metrics and dedup accounting only.

The solver (`belady_main`, `wp2_opt` for unequal chunk sizes) reads the header
and **aborts on a fault trace**. It builds a next-use index over the reference
string (one reverse pass), simulates with a max-heap on next-use distance
(O(n log k)), and on every invocation checks the result against the provable
cyclic-scan floor:

```
floor(n, k, passes) = n + (passes − 1) · max(n − k, 0)
```

Pass 1 is `n` compulsory misses; each later pass has at most `k` items resident,
so at least `n − k` of its `n` references miss. Any solver result *below* this
is a mathematical impossibility — it is what catches a circular OPT input. The
solver is also cross-checked against a naive O(n²) next-occurrence scan over
many random cases.

## 8. Arms

| arm | configuration |
|---|---|
| **A** | kernel `mmap` + `madvise` — "just let the kernel do it" |
| **B** | A + `MADV_WILLNEED` / `MADV_PAGEOUT` hints — informed kernel |
| **C** | pager + `lru`, prefetch off — isolates authority from policy |
| **D** | pager + `layer_order_declared`, prefetch off — the informed policy |
| **E** | D + prefetch |
| **OPT** | offline Belady over the workload-authored reference trace |

Every arm consumes **every byte of every referenced chunk** (not a sentinel
byte) with an identical read pattern; the only difference between arms is who
owns the replacement decision. Every arm runs in a cgroup at an equal
weight-residency budget. Budget ratios are swept, not tuned to one point. An
`E_INFEASIBLE` / OOM at budget *B* is a **censored data point** (policy
infeasible at *B*), plotted, never discarded — unless the pager's accounting
said it was under budget and `memory.stat[shmem]` disagreed, which is a bug.

## 9. Correctness harness (T-1…T-7)

Runs with `--eager-reconcile`, after every code change. All pass,
`mismatches = 0`.

| test | what it catches |
|---|---|
| T-1 data integrity | full region read under a 25 % budget, every sample checked, all policies — eviction or fetch serving stale data |
| T-2 punch-refetch | 1000 evict/refetch cycles, contents identical — a broken eviction path |
| T-3 concurrent storm | 8 threads, tight budget, 60 s — hangs, corruption, budget overrun, `reconcile()` trips |
| T-4 accounting | `resident_bytes` matches `memory.stat[shmem]` − overhead exactly |
| T-5 Belady sanity | the cyclic-floor + naive-cross-check self-test |
| T-6 dedup fires | the `RESIDENT` / `FETCHING` dedup branches are exercised under load (`stat_dedup_fetching > 0`) |
| T-7 no lost fault | 60 s storm + 120 s watchdog — every storm thread joins, no fault is ever lost |

The real-model integration additionally runs a **startup audit**: after the
first decode completes, if any workload-declared chunk received zero
consumption signals, it prints the chunk id and name and aborts — a silent
name mismatch must not degrade to unnoticed thrash.

## 10. Scope

**In:** the pager mechanism, the three live policies, the trace recorder, the
offline solver, the replay driver, the measurement harness, and the llama.cpp
integration (`residctl_llama.c` + `wp2_gen.cpp` + a ~35-line `dlsym` hook in
`llama-mmap.cpp`).

**Not built:** multi-region support, huge pages, write support, network-backed
fetch, non-root operation, GPU offload.
