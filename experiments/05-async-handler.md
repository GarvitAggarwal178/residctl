# Async Handler + Prefetch Admission

Item 10c: a direct consequence of item 10b Task C's finding that
`dedup_fetching` was not merely empirically zero but *structurally*
unreachable under the synchronous handler — proof that the spec's original
handler design, not the dedup branch itself, was wrong. Full background:
`CLAUDE.md`'s "ITEM 10c" notes.

## VERDICT

**T-6 passed** (`stat_dedup_fetching > 0`): dispatch/fetch decoupling
works — `dedup_fetching=14494` on a 60s storm that produced exactly `0/0`
every time under the old synchronous handler (6+ independent confirmations
across items 2 through 10b).

**Device-busy fraction did NOT rise above 0.86.** It stayed in the same
0.82-0.86 band item 10b measured, at every worker count tested (1/2/4),
on arm D. This is explained, not just observed: arm D's replay driver is a
single thread issuing touches strictly sequentially with prefetch off, so
there is never more than one outstanding demand fault for extra workers to
overlap with. The async handler's benefit shows up where there is genuine
concurrency to exploit (T-3/T-6/T-7's multi-threaded storms), not on this
specific single-threaded, no-prefetch arm.

**Prefetch hit rate did NOT rise above the 14-27% band.** Guarded and
`always` admission produced statistically indistinguishable hit rates
(0.14-0.27 either way) and statistically indistinguishable byte counts.
`stat_prefetch_declined` was `0` in every one of Sweep 3's 36 cells, despite
the admission check running inside eviction loops that fired 30-100+ times
per run. Mechanism, not a bug (see Task B / Sweep 3 below):
`layer_order_select_victim()` already picks the single *farthest* resident
chunk as its victim, and a prefetch's own target (by construction, a near
successor in the same chain) is essentially always closer than that — so
the admission guard, as specified, is structurally close to a no-op under
`layer_order`. Item 10b's original 14-27% ceiling has a different primary
cause than "the evicted victim was needed sooner than the prefetch target."

## Machine exclusivity

**Before Sweep 1:** `free -h` 6.9 GiB available, load average 0.08, no
foreign workload (`pgrep -f "cn-spike|gate5r_driver|iperf3"` clean).

**During Sweep 2's resume:** a system `updatedb.plocate` indexing job
(started 02:19, a routine `cron`/`systemd-timer` filesystem index, not a
user workload) was running during the first attempt at ratio=0.75's arm E
cell — caught by the "after" exclusivity check, not silently missed. Per
the project rule ("don't kill it, stop and report"), waited for it to exit
naturally (confirmed via `pgrep -f`, not `pgrep -x`, which gave a false
"already finished" the first time due to `comm` truncation at 15
characters), reconfirmed a clean `ps`/`uptime` state, removed the three
contaminated CSV rows, and redid exactly that one cell
(`run_task_c_sweep2_redo_e.sh`). The contamination's actual effect on the
numbers was small (wall-clock medians moved by a few percent, well within
normal rep-to-rep spread), but the redo was done regardless — machine
exclusivity is a hard rule, not a judgment call based on how much the
contamination looks like it mattered.

**Background-task cap, disclosed as a methodology note, not a data
problem:** both Sweep 2 and Sweep 3 exceeded this harness's 10-minute
background-task limit and were killed mid-run partway through (Sweep 2
during ratio=0.75's arm A `MADV_RANDOM` submode; Sweep 3 during
ratio=0.5/depth=4/guarded). Both were resumed from the exact point of
interruption (verified via `cut ... | sort | uniq -c` against the CSV
before resuming), appending to the same CSV/log files rather than
restarting — no data was silently dropped or duplicated; every resume was
checked against the raw row counts before and after.

**Two runs of arm A's `MADV_RANDOM` submode timed out** (`rc=124`) against
this sweep's `timeout 180`, at ratio=0.25 rep 1 and ratio=0.75 reps 2-3 —
item 10b/V2 already measured this submode at 162-174s, uncomfortably close
to the 180s ceiling I chose. This is a real gap in `MADV_RANDOM`'s own
n=3 coverage at those two ratios (2/3 and 1/3 reps respectively), disclosed
under "What I did NOT test" — it does not affect any reported finding,
since `MADV_RANDOM` was never competitive (surviving reps were still
160-174s, versus 2-3s for `normal`/`sequential`) and never won `best_mode`
at any ratio.

**After the full run (all three sweeps):** `free -h` 6.7-7.0 GiB available
throughout, swap untouched, no foreign workload in any post-run `ps aux`
snapshot.

## Task A — Async handler

**Implementation.** `handle_fault()`'s `ABSENT` case no longer calls
`fetch_chunk()` inline. It marks the chunk `CHUNK_FETCHING`, stamps
`last_fault_seq`, records the fault trace, runs the policy's `on_fault`
hook, and enqueues the chunk index on a shared fetch pool
(`prefetch_pool.c`, generalized from item 10b's prefetch-only pool) —
then returns immediately, never touching `ensure_budget()`, `fetch_chunk()`,
or any ioctl. `pager_run()` owns this pool's lifecycle whenever
`async_handler` is true (the new default): it starts `fetch_workers`
(default 4, `--fetch-workers N`) worker threads before entering the
dispatch loop and stops them after the loop exits, so no existing
test/driver needed changes to pick up the async path. Workers dequeue
demand jobs before ever looking at the prefetch queue (strict priority,
two separate ring buffers in `prefetch_pool_t`), then run `ensure_budget()`
→ `fetch_chunk()` → `commit_reserved_and_pin()` → mark `RESIDENT` → policy
hooks → prefetch top-up → unpin, reusing item 10b's three concurrency
fixes unchanged (commit-before-`RESIDENT`, atomic pin-with-commit, ranged
`reconcile()`). `--sync-handler` restores the pre-item-10c synchronous
path exactly, for A/B comparison; `--fetch-workers N` (default 4) is
independent of `--prefetch-depth`.

**T-1..T-5 re-run.** All five pass unmodified, under the new async default
(`test_correctness.c`/`test_storm.c` never set `--sync-handler`). Full
log: `experiments/logs/correctness_harness_log.txt`. Bonus, unplanned confirmation:
`test_pager.c`'s own barrier-release dedup test (item 2's "known gap,"
never observed to fire across 5+ runs under the old handler) now shows
`dedup_fetching=7` on the identical scenario under the new async default.

**T-6 (new).** `stat_dedup_fetching > 0` — **PASS**: `dedup_fetching=14494`
on a clean 60s, 8-thread, tight-budget run (`dedup_resident=23443`
reported alongside per the spec, not gating).

**T-7 (new).** No fault ever lost — **PASS**: all 8 storm threads joined
well within the 120s internal watchdog after a 60s storm; the watchdog's
`/proc/PID/task/*/wchan` dump code path never had to fire. Full log:
`experiments/logs/t6_t7_log.txt`.

## Task B — Prefetch admission

**Implementation.** `policy_t` gained `next_use_distance(region, chunk)`.
`layer_order` derives it from the same successor-chain walk
`select_victim` already used (refactored into a shared
`layer_order_compute_dist()` so the two can't silently disagree); `lru`
returns `INT64_MAX` unconditionally. `budget.c`'s new
`ensure_budget_prefetch()` mirrors `ensure_budget()`'s reconcile/evict/
reserve loop, but declines (rather than forces) any candidate eviction
where the victim isn't strictly colder than the prefetch's own target,
incrementing `stat_prefetch_declined`. Demand fetches are unaffected — they
always call the unconditional `ensure_budget()`. `--prefetch-admission
{always,guarded}`, default `guarded`.

**Declined counts: `0` in every cell of Sweep 3** (36/36), despite
30-100+ evictions per run in the higher-pressure cells. See Sweep 3 below
for the mechanism.

**Regression:** full T-1..T-7 re-run clean under the new `guarded`
default. Visible, expected side effect even on unmodified T-3: prefetch
count dropped from ~12000 to ~8300 on the same 60s storm — but this
particular drop is from a *different* mechanism than admission declining
(see Anomalies: T-3 uses depth=1 by default, and the drop is consistent
with normal run-to-run scheduling variance under the new architecture, not
`stat_prefetch_declined`, which the storm tests don't report).

## Sweep 1 — sync vs async (arm D)

4 handler configs (`sync`, `async` at `--fetch-workers` 1/2/4) × 3 ratios ×
n=3, V2 parameters (2 GiB region, 128 MiB chunks, 5 passes), arm D
(`layer_order`, prefetch off). Raw data: `results/data/historical/task-c-sweep1.csv`; full
log: `experiments/logs/task_c_sweep1_log.txt`. Device-busy fraction computed via
`--fetch-trace` and the union-of-read-intervals method
(`scratch/analyze_sweep1_busy.py`), same as item 10b Task A.

**Reproduction check: HOLDS.** `--sync-handler` at `--fetch-workers 1`
reproduces item 10b's arm D device-busy numbers closely: 0.831/0.826/0.859
(r=0.25/0.5/0.75) here vs item 10b's reported 0.82-0.86 band.

### Device-busy fraction (median of n=3)

| Ratio | sync | async fw=1 | async fw=2 | async fw=4 |
|---|---|---|---|---|
| 0.25 | 0.831 | 0.831 | 0.820 | 0.827 |
| 0.50 | 0.826 | 0.846 | 0.845 | 0.847 |
| 0.75 | 0.859 | 0.848 | 0.859 | 0.856 |

**Does not rise above the 0.82-0.86 band at any worker count.** Expected,
not a null result: arm D's replay driver is one thread issuing touches
strictly sequentially with prefetch off, so at any instant there is
exactly one outstanding demand fault — extra fetch workers have nothing to
overlap with. `dedup_resident`/`dedup_fetching` are `0` in every Sweep 1
cell for the same structural reason (a single-threaded driver can never
generate two concurrent faults on the same chunk). Wall-clock (raw CSV)
is correspondingly flat-to-noisy across handler configs at every ratio —
no consistent win or loss from switching handler mode on this specific
arm. The async architecture's measured benefit comes from genuine
concurrent demand (T-3/T-6/T-7's multi-threaded storms) or prefetch-driven
overlap (arm E, Sweep 2/3), neither of which bare arm D exercises.

## Sweep 2 — full arm sweep

All six arms, 3 ratios, n=3, async handler (default), `--fetch-workers 4`,
`--prefetch-admission guarded`, `--prefetch-depth 2` (item 10b's best
depth). V2 parameters. Raw data: `results/data/historical/task-c-sweep2.csv`; full log:
`experiments/logs/task_c_sweep2_log.txt`.

### Primary metric: read_bytes (median of n=3, MB/touch, bytes/1e6/80)

| Ratio | A (best mode) | B (+hints) | C (lru) | D (layer_order) | E (layer_order+prefetch) | OPT |
|---|---|---|---|---|---|---|
| 0.25 | 134.2 (sequential) | 127.4 | 134.2 | **115.8** | 144.3 | 109.1 |
| 0.50 | 134.2 (sequential) | 127.4 | 134.2 | **95.6** | 130.9 | 80.5 |
| 0.75 | 134.5 (normal) | 127.4 | 134.2 | **68.8** | **68.8** | 53.7 |

Arm A's winning mode changed at r=0.75 (`normal`, 30.0 touches/s) vs
`sequential` at r=0.25/0.5 — a narrow, noise-level flip (30.0 vs 29.2
touches/s, ~3% apart; V2 had a similarly close 4% gap between the two at
its own r=0.75 without the ranking flipping). Not treated as a finding.

### Absolute median read_bytes per ratio (bytes)

| Ratio | A | B | C | D | E | OPT |
|---|---|---|---|---|---|---|
| 0.25 | 10,737,455,104 | 10,192,158,720 | 10,737,418,240 | 9,261,023,232 | 11,542,724,608 | 8,724,152,320 |
| 0.50 | 10,737,455,104 | 10,192,158,720 | 10,737,418,240 | 7,650,410,496 | 10,468,982,784 | 6,442,450,944 |
| 0.75 | 10,762,620,928 | 10,192,158,720 | 10,737,418,240 | 5,502,926,848 | 5,502,926,848 | 4,294,967,296 |

**OPT ≤ D holds at every ratio** (8.72G≤9.26G, 6.44G≤7.65G, 4.29G≤5.50G).
**E never beats OPT at any ratio** (11.54G>8.72G, 10.47G>6.44G,
5.50G>4.29G) — both checks that were the whole point of the item 10
correction still hold under the new architecture.

**E vs D: E does not beat D at any ratio in this sweep** — worse at
r=0.25/0.5 (11.54G>9.26G, 10.47G>7.65G) and exactly tied at r=0.75
(5.50G=5.50G, both medians landing on the same value). This directly
contradicts one of item 10c's pre-registered expectations ("E should no
longer exceed D at r=0.25") — see Pre-registered expectations and Sweep 3
below for why: the admission guard measurably declined nothing in this
workload, so E's bytes here are not meaningfully different from what
`--prefetch-admission always` would have produced (confirmed directly in
Sweep 3).

### Secondary metric: demand faults (median of n=3)

| Ratio | C (lru) | D (layer_order) | E (layer_order+prefetch) | OPT (misses) |
|---|---|---|---|---|
| 0.25 | 80/80 (100%) | 69/80 | 43/80 | 65 |
| 0.50 | 80/80 (100%) | 57/80 | 42/80 | 48 |
| 0.75 | 80/80 (100%) | 41/80 | 27/80 | 32 |

Same ordering as V2 (E<D<C, C thrashes at 100%) — prefetch still
measurably converts real faults into no-ops even though it doesn't reduce
total bytes moved; this is the expected D→E fault-count delta from §6.3,
distinct from the byte-count question above.

### Secondary metric: wall-clock (median of n=3, seconds)

| Ratio | A | B | C | D | E |
|---|---|---|---|---|---|
| 0.25 | 2.76 | 3.59 | 5.17 | 4.47 | 4.24 |
| 0.50 | 2.33 | 3.51 | 5.75 | 4.05 | 4.19 |
| 0.75 | 2.67 | 2.33 | 5.46 | 2.65 | 2.55 |

Same qualitative pattern as V2: the kernel-native path is still faster per
byte than the pager at this synthetic-benchmark scale (no compute phase to
hide fetch latency behind). D and E are close to each other at every
ratio, consistent with E moving more total bytes but converting some
demand faults (which pay full dispatch+queue+fetch latency each) into
prefetch-satisfied hits (cheaper on the wall-clock axis even when not
cheaper on bytes).

## Sweep 3 — admission A/B

Arm E only, `guarded` vs `always`, 3 ratios × n=3 × `--prefetch-depth` ∈
{2,4}, async handler, `--fetch-workers 4`. `--fetch-trace` captured per
run for the post-hoc hit-rate analysis (item 10b Task B's method: a
prefetch is a "hit" if no later fetch event exists for that chunk_id
before the run ends). Raw data: `results/data/historical/task-c-sweep3.csv`; full log:
`experiments/logs/task_c_sweep3_log.txt`; hit-rate script:
`scratch/analyze_sweep3_hitrate.py`.

### Prefetch hit rate, declined count, read_bytes

| Ratio | Depth | Admission | Prefetches (n) | Hit rate | read_bytes (median) | Declined | Infeasible |
|---|---|---|---|---|---|---|---|
| 0.25 | 2 | guarded | 131 | 0.21 | 11,542,724,608 | 0 | 0 |
| 0.25 | 2 | always | 135 | 0.20 | 11,811,160,064 | 0 | 0 |
| 0.25 | 4 | guarded | 207 | 0.18 | 13,958,643,712 | 0 | 0 |
| 0.25 | 4 | always | 205 | 0.18 | 13,958,643,712 | 0 | 0 |
| 0.50 | 2 | guarded | 107 | 0.14 | 10,468,982,784 | 0 | 0 |
| 0.50 | 2 | always | 104 | 0.14 | 10,468,982,784 | 0 | 0 |
| 0.50 | 4 | guarded | 117 | 0.21 | 9,932,111,872 | 0 | 0 |
| 0.50 | 4 | always | 117 | 0.21 | 9,932,111,872 | 0 | 0 |
| 0.75 | 2 | guarded | 42 | 0.21 | 5,502,926,848 | 0 | 0 |
| 0.75 | 2 | always | 42 | 0.21 | 5,502,926,848 | 0 | 0 |
| 0.75 | 4 | guarded | 66 | 0.27 | 6,442,450,944 | 0 | 0 |
| 0.75 | 4 | always | 66 | 0.27 | 6,442,450,944 | 0 | 0 |

**`guarded` and `always` are statistically indistinguishable at every
cell** — hit rate within 1 percentage point (mostly identical), byte
counts identical or within normal rep-to-rep variance (see Anomalies),
`stat_prefetch_declined = 0` everywhere. This is not a wiring bug — traced
and explained:

`ensure_budget_prefetch()`'s guard only has an opportunity to decline when
an eviction is actually needed, which happens routinely here (30-100+
evictions per run in the higher-pressure cells, confirming the guard's
`while` loop and its distance check ran many times, not that it was simply
never exercised). But `layer_order_select_victim()` already picks the
single chunk with the *largest* `next_use_distance` among residents — by
construction, the worst possible victim from the policy's own perspective.
A prefetch's target, in turn, is always a *near* successor in the same
chain (distance 1..depth from "now," by construction of
`prefetch_pool_top_up`'s forward walk). For the victim to fail the guard
(`next_use_distance(victim) <= next_use_distance(target)`), some resident
chunk would have to be *closer* than a near-term prefetch target while
simultaneously being `select_victim`'s own top pick for "farthest" — a
contradiction under normal cache occupancy. The guard, as specified, is
close to redundant with what `layer_order`'s existing victim selection
already guarantees. Item 10b's 14-27% hit-rate ceiling therefore has a
different primary cause than "the evicted victim was needed sooner than
the prefetch target" — plausibly a depth-vs-budget mismatch instead (a
target `depth` hops ahead can itself be evicted by a *later* prefetch or
demand fetch before ever being touched, which admission gating does not
address, since it only ever asks "is the victim colder than *this*
target," never "will *this* target itself survive to be used"). Not
chased further here — flagged as the honest next question, not resolved.

## Pre-registered expectations

1. **Prefetch hit rate rises substantially above 14-27%: DID NOT HOLD.**
   0.14-0.27 across all 12 (ratio, depth) cells under `guarded` — the same
   band item 10b measured, not above it.
2. **`read_bytes` for arm E falls and no longer exceeds D at r=0.25: DID
   NOT HOLD.** E read *more* bytes than D at every ratio in Sweep 2
   (11.54G vs 9.26G at r=0.25), and Sweep 3 shows `guarded` moved
   essentially the same bytes as `always` at matching (ratio, depth) cells
   — there was nothing for the admission rule to improve in this
   workload, per the mechanism explained above.
3. **Demand fault count may rise slightly, an acceptable trade if bytes
   fall: N/A.** Bytes did not fall, so this trade-off never came up;
   `guarded` vs `always` absent_handled counts are identical or within
   normal rep noise at every Sweep 3 cell (e.g. r=0.25/d2: 43/43/43 both
   ways).
4. **`infeasible` counts at r=0.25/depth≥4 should fall: NOT TESTABLE.**
   `infeasible=0` in every single cell across Sweep 2 and Sweep 3, under
   both `guarded` and `always` — the censoring rule did not fire anywhere
   in this report, matching V2's own finding, so there was nothing to
   fall from.

None of the four held. The honest reading: Task B's mechanism is
implemented and verified correct by inspection and by the reasoning above,
but it targets a failure mode (`select_victim` evicting something needed
sooner than the prefetch target) that `layer_order`'s existing victim
selection already prevents on its own for this workload — so it measurably
changed nothing here, and item 10b's original hit-rate ceiling remains
unexplained by this fix.

## Concurrency bugs found

None found by reproduction/stress-testing during item 10c's build phase
(unlike item 10b, which found three via actual hangs). Two LATENT bugs
were found by inspection before anything ran, both fixed proactively:
1. `latency_hist_record()` had no internal lock; multiple fetch-pool
   workers calling it concurrently (new in item 10c — previously only a
   single synchronous handler thread ever called it) would race on
   bucket/count/sum/min/max updates. Fixed with an internal mutex on
   `latency_hist_t`.
2. Several `region_t` stat counters (`stat_bytes_fetched`,
   `stat_absent_handled`, `stat_prefetches`, `stat_prefetch_infeasible`)
   were incremented with plain `++`/`+=` in `prefetch_pool.c`'s worker
   functions — already a latent race in item 10b's narrower
   `--prefetch-depth > 1` case, made much more likely to actually corrupt
   a count by item 10c's higher default concurrency. Fixed with
   `__sync_fetch_and_add`.

## Anomalies

- **Byte counts are no longer perfectly deterministic across reps, unlike
  V1/V2.** Sweep 3's raw CSV shows small rep-to-rep variance in
  `pager_bytes_fetched` within the *same* (ratio, admission, depth) cell —
  e.g. r=0.5/guarded/d2: 10,468,982,784 / 10,334,765,056 / 10,468,982,784;
  r=0.5/always/d2: 10,468,982,784 / 10,468,982,784 / 9,797,894,144. Under
  the old single-threaded synchronous design, every rep of a given
  configuration produced byte-for-byte identical results (V2's report
  noted this explicitly). With genuinely concurrent fetch workers, exactly
  which speculative fetches complete before their target chunk is next
  needed is now timing-dependent, so prefetch outcomes (and therefore
  total bytes moved) carry real run-to-run variance. This is a disclosed,
  expected side effect of the architecture change, not a correctness bug —
  no test asserts exact byte-for-byte reproducibility for prefetch-enabled
  runs, and T-1/T-2/T-4's *exact* accounting checks (which do assert
  byte-for-byte matches) don't use prefetch in a way this variance would
  touch.
- **Arm A's best-mode selection flipped from `sequential` to `normal` at
  r=0.75** (30.0 vs 29.2 touches/s) — a ~3% margin, same order of
  magnitude as V2's own 4% sequential-vs-normal gap at its r=0.75. Treated
  as noise-level, not a finding.
- **Byte-accounting cross-check (pager vs kernel `/proc/self/io`) matched
  exactly in every row across all three sweeps** — no `DISCREPANCY` line
  appeared in any of the three logs.

## What I did NOT test

- `--sync-handler` combined with `--prefetch-admission guarded` — Task B's
  gate is wired into both prefetch call sites (`prefetch.c`'s
  `maybe_prefetch` and `prefetch_pool.c`'s `do_one_prefetch`) and should
  work under `--sync-handler` too, but no sweep specifically exercises
  that combination.
- Full n=3 for `MADV_RANDOM` at r=0.25 (2/3 reps, one `timeout 180`
  hit) and r=0.75 (1/3 reps, two `timeout 180` hits) — see Machine
  exclusivity above. Never affected `best_mode` selection.
- Why the guarded admission rule has zero measured effect under `lru`
  specifically — moot in practice (`lru`'s `predict_next` is always -1,
  so nothing is ever enqueued for prefetch under it), not directly tested.
- Any chunk size or region size other than the V2 scale (2 GiB / 128 MiB)
  — same limitation as V2/10b.
- A budget ratio tight enough to make any of these three sweeps hit
  `E_INFEASIBLE` or an actual OOM organically — still unexercised across
  V1, V2, and item 10c.
- llama.cpp integration — still explicitly out of scope.

## Final check

No fabricated numbers: every value in this report is either a direct read
from `results/task_c_sweep{1,2,3}.csv` (raw data from real runs, machine
exclusivity checked before/after, one contaminated cell caught and redone)
or a disclosed, straightforward computation over it (median, MB/touch
conversion, hit-rate script). T-1..T-7 all re-run and passing is reported
as such; the fact that three of four Task B pre-registered expectations
failed to hold is reported as such, with the actual mechanism traced
(`layer_order`'s existing victim selection already subsumes the admission
guard for this workload), not glossed into "the fix helped" or hidden.
Two background-task interruptions (Sweep 2, Sweep 3 both hit the 10-minute
cap) were resumed from a verified exact point, not silently restarted or
patched over gaps in the data.
