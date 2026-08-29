# Phase 0 — Analysis Over Existing Data

No new runs this phase. Machine state note: at the start of this campaign
(before any new measurement), `ps aux` showed a foreign workload (`cc1plus`
compiling `/root/souffle-master`, 98% CPU, load average 6.89) already
running. This does not affect Phase 0 (no new timed measurements), but is
recorded here and re-checked before Phase 1, which does require exclusivity.

## 0.1 Fetch overlap on arm E

Item 10e Task C's `--fetch-trace` files (`scratch/sweepec_*.fetchtrace`,
75 files, all retained) were re-analyzed with the same overlap method
Sweep B used (`scratch/analyze_sweep_e_b.py`'s
`median_concurrent_outstanding`), extended per this phase's request to
break out demand-only vs. prefetch-only medians and count demand↔prefetch
overlapping pairs directly. Script: `scratch/analyze_phase0_overlap.py`.

### Overlap statistics (aggregated over 3 reps per cell)

| Ratio | Depth | Retention | n fetches | Median (all) | Max | Frac. ≥1 overlap | Median (demand-only) | Median (prefetch-only) | Demand↔prefetch pairs |
|---|---|---|---|---|---|---|---|---|---|
| 0.25 | 2 | none | 286 | 2.00 | 4 | 0.649 | 1.00 | 2.00 | 31 |
| 0.25 | 2 | pinned | 280 | 2.00 | 5 | 0.684 | 1.00 | 3.00 | 38 |
| 0.25 | 4 | none | 345 | 3.00 | 7 | 0.752 | 1.00 | 4.00 | 27 |
| 0.25 | 4 | pinned | 323 | 3.00 | 5 | 0.805 | 2.00 | 3.00 | 57 |
| 0.375 | 2 | none | 225 | 1.00 | 4 | 0.493 | 1.00 | 2.00 | 14 |
| 0.375 | 2 | pinned | 255 | 2.00 | 4 | 0.578 | 1.00 | 2.00 | 26 |
| 0.375 | 4 | none | 262 | 3.00 | 6 | 0.655 | 1.00 | 4.00 | 13 |
| 0.375 | 4 | pinned | 294 | 3.00 | 5 | 0.742 | 2.00 | 3.00 | 43 |
| 0.5 | 2 | none | 208 | 1.00 | 4 | 0.479 | 1.00 | 2.00 | 16 |
| 0.5 | 2 | pinned | 236 | 2.00 | 5 | 0.582 | 1.00 | 2.00 | 23 |
| 0.5 | 4 | none | 233 | 2.00 | 6 | 0.584 | 1.00 | 3.00 | 13 |
| 0.5 | 4 | pinned | 263 | 3.00 | 6 | 0.659 | 1.00 | 3.00 | 32 |
| 0.625 | 2 | none | 168 | 1.00 | 4 | 0.283 | 1.00 | 2.00 | 5 |
| 0.625 | 2 | pinned | 192 | 1.00 | 4 | 0.418 | 1.00 | 2.00 | 10 |
| 0.625 | 4 | none | 175 | 1.00 | 6 | 0.483 | 1.00 | 3.00 | 6 |
| 0.625 | 4 | pinned | 202 | 2.00 | 5 | 0.529 | 1.00 | 3.00 | 14 |
| 0.75 | 2 | none | 136 | 1.00 | 4 | 0.370 | 1.00 | 2.00 | 7 |
| 0.75 | 2 | pinned | 162 | 1.00 | 4 | 0.315 | 1.00 | 2.00 | 4 |
| 0.75 | 4 | none | 135 | 1.00 | 4 | 0.409 | 1.00 | 2.00 | 6 |
| 0.75 | 4 | pinned | 131 | 1.00 | 3 | 0.422 | 1.00 | 2.00 | 3 |

**Arm E (prefetch on) shows real overlap.** Median concurrently-outstanding
is 1.00–3.00 depending on cell, max reaches 7, and 28-81% of individual
fetches overlap at least one other fetch. Demand-only fetches rarely
overlap other fetches (median 1.00-2.00) — demand faults on this workload
still mostly serialize against each other, consistent with item 10e's
finding on arm D. Prefetch-only fetches overlap far more (median 2.00-4.00)
— prefetches are what actually produces concurrent I/O here. Demand↔
prefetch overlapping pairs range from 3 to 57 per (ratio,depth,retention)
cell (aggregated over 3 reps), concentrated at tighter ratios and deeper
prefetch depth (more prefetches issued, more chances to overlap demand).

This measures overlapping WALL-CLOCK READ WINDOWS as recorded by
`--fetch-trace`, not confirmed hardware-level concurrent I/O service — the
distinction matters for interpreting against Phase 1's platform-level
result, which tests raw device concurrency directly. Item 10e's "exactly
1.00" finding is not contradicted: that finding was specific to arm D
(prefetch off), where demand fetches structurally can't put a second fetch
in flight on this workload (converging driver threads all fault the same
chunk; the dedup path absorbs everyone else). Arm E's prefetch fetches are
the only mechanism in this project that has ever produced overlapping
fetch windows, and they clearly do.

## 0.2 Lookahead window wall-clock effect

`results/data/historical/task-e-sweep-b.csv`, arm D, `layer_order` policy — every cell
Sweep B actually ran (unlike the Task A gate, whose dramatic 5.177s→
3.379s→3.227s observation was run under `policy=default`, NOT
`layer_order` — see below, this is a genuine confound, not a
reproduction).

### Wall-clock (median of n=3, seconds)

| Ratio | Threads | Window=0 | Window=1 | Window=2 |
|---|---|---|---|---|
| 0.25 | 1 | sync 4.475 / async 4.279 | sync 4.092 / async 4.117 | sync 4.150 / async 4.153 |
| 0.25 | 8 | sync 4.228 / async 4.213 | sync 4.481 / async 4.415 | sync 4.642 / async 4.556 |
| 0.50 | 1 | sync 4.041 / async 3.763 | sync 4.267 / async 3.646 | sync 3.672 / async 3.640 |
| 0.50 | 8 | sync 3.765 / async 3.751 | sync 3.664 / async 3.669 | sync 4.048 / async 3.931 |
| 0.75 | 1 | sync 2.761 / async 2.573 | sync 2.633 / async 3.085 | sync 2.710 / async 2.934 |
| 0.75 | 8 | sync 2.556 / async 2.818 | sync 2.709 / async 2.598 | sync 2.713 / async 2.732 |

**The improvement does not hold in Sweep B's own data, at any ratio, at
either thread count.** There is no consistent monotonic fall from W=0 to
W=2 anywhere in this table — at threads=8/ratio=0.5 (the exact cell the
gate used), wall-clock is roughly flat-to-slightly-worse (sync:
3.765→3.664→4.048; async: 3.751→3.669→3.931), nothing resembling the
gate's 38% drop.

**Root cause of the discrepancy: a policy confound, not a measurement
error.** `scripts/historical/run-task-e-gate.sh` (item 10e's own Task A verification
gate script) invoked `replay_main` with policy argument `default`
(`region_t.policy == NULL`, the lowest-region-off FIFO fallback selector
in `budget.c`'s `default_select_victim()`), not `layer_order`. Every cell
of Sweep B, and every other sweep in this project since item 7, uses
`layer_order`. The dramatic wall-clock effect was measured under a policy
that has never been used anywhere else in this project's reported sweeps.
Confirmed by direct inspection of the gate script's command line, not
inferred. Whether the `default` policy's own eviction behavior (a static
lowest-index rule rather than distance-based) interacts differently with
overlapping fetches than `layer_order` does was not tested further this
phase — flagged as a real, disclosed methodological gap in item 10e's own
incidental observation, not resolved here.

## 0.3 Source review — locks on the fetch path

Independently re-derived from the current source (not assumed from the
prior review). For each function, the lock held at the exact moment
`fetch_read()`'s `pread()` executes (`fetch.c:54`, inside `fetch_chunk()`,
called from `fetch.c` with no locking of its own anywhere in `fetch_chunk`/
`fetch_read`/`fetch_resolve` — confirmed by reading the whole file).

**`do_one_demand()` (`prefetch_pool.c:58-154`), the async demand-fetch
path (default handler):**
- `c->lock` acquired at `prefetch_pool.c:63`, held continuously through
  `prefetch_pool.c:153` (unlock) — `pread()` (via `fetch_chunk()` called
  at `prefetch_pool.c:105`) executes under this lock.
- `budget_lock` is acquired and released INSIDE `ensure_budget()`
  (`budget.c`), called at `prefetch_pool.c:76`/`92`, which returns before
  `fetch_chunk()` is called at line 105 — **not held** at the `pread()`
  call site.
- `pool->lock` was released by the caller (`worker_main`,
  `prefetch_pool.c:266`) before `do_one_demand()` was ever invoked — **not
  held**.
- **Only one lock held at `pread()`: `c->lock`, a per-chunk lock.**

**`do_one_prefetch()` (`prefetch_pool.c:161-247`), the async prefetch path:**
- `target->lock` acquired at `prefetch_pool.c:165`, held continuously
  through `prefetch_pool.c:246` (unlock) — `pread()` (via `fetch_chunk()`
  called at `prefetch_pool.c:210`) executes under this lock.
- `budget_lock` acquired/released inside `ensure_budget_prefetch()`
  (`budget.c`), called at `prefetch_pool.c:195`, returns before
  `fetch_chunk()` — **not held**.
- `pool->lock` released by `worker_main` (`prefetch_pool.c:273`) before
  `do_one_prefetch()` was invoked — **not held**.
- **Only one lock held at `pread()`: `target->lock`, a per-chunk lock.**

**Confirms the prior review for the async path (`do_one_demand`,
`do_one_prefetch` — the pair explicitly named): the only lock held across
`pread()` is a per-chunk lock, never a global one.** Since two concurrent
fetch jobs are always on two DIFFERENT chunks (the `FETCHING`-state dedup
in `pager.c`'s dispatcher guarantees a chunk can never have two
outstanding fetches), these per-chunk locks never contend with each other
across concurrent fetches. The prior review's conclusion was correct, not
overturned.

**Additional, scope-adjacent finding (not the pair the review named, but
directly relevant to the same question): `maybe_prefetch()`
(`prefetch.c:20-116`), the `--sync-handler`/`prefetch_depth==1`-only
inline path, holds TWO chunk locks simultaneously at `pread()`** — its
own `target->lock` (acquired `prefetch.c:35`, held through `prefetch.c:115`)
AND the caller's `c->lock` for `just_resident` (acquired by `pager.c`'s
`handle_fault()` before calling `handle_absent()`, which calls
`maybe_prefetch()` internally, still held throughout). `budget_lock` is
released before `fetch_chunk()` here too (called `prefetch.c:65`, returns
before `fetch_chunk()` at line 80). This is still two PER-CHUNK locks, not
a global one, and doesn't create contention with itself — under
`--sync-handler` at depth 1, there is only ever one thread doing fetches
at all (no pool exists), so no other fetch can be blocked by either lock
regardless. Not a finding that changes the async-path conclusion above,
noted for completeness since it touches the same code family.

## Final check

- No number in this phase was estimated, inferred, or copied from
  documentation — 0.1's table is a direct re-computation over retained
  binary trace files; 0.2's table is a direct read of
  `results/data/historical/task-e-sweep-b.csv`; 0.3's line numbers were read from the
  current source files in this same session, not recalled from memory of
  writing them.
- No test was modified.
- Pre-registered interpretations were checked against measured values:
  0.1 confirms overlap exists on arm E, contrasted precisely against
  item 10e's arm-D-only finding; 0.2 states plainly the effect does not
  hold in Sweep B's own data and identifies the specific methodological
  cause (a policy confound in the gate script); 0.3 confirms the prior
  review's claim for the exact pair of functions named.
- No gate in this phase (analysis only, no verification gates
  pre-registered for Phase 0).
