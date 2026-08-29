# Phase 2 — Compute Phase in the Driver

Adds `--compute-ns-per-mib X` to the replay driver (default 0, byte-for-byte
unchanged). Non-zero X runs a real, calibrated arithmetic loop over the
bytes a thread just read, proportional to X nanoseconds per MiB — modelling
the compute window real transformer inference spends on a layer's weights
before the next layer is needed, which is exactly the interval prefetch
exists to fill. Implementation: `src/replay.c` (`replay_calibrate_compute()`,
`compute_busy()`, `compute_unit()`), wired into both `replay_cyclic()` and
`replay_cyclic_mt()`. Script: `scripts/historical/run-phase2-sweep.sh`.

## Machine exclusivity

**Before implementation work:** clean.

**During the sweep — a genuine contamination event, handled per instruction
(stop, report, wait; do not kill):** a Gradle daemon (`java …
GradleDaemon`, pid 45112) was found running, started 07:39, discovered via
a routine exclusivity recheck after the first sweep attempt was killed by
the harness's background-task cap. It was not killed. The first sweep
attempt's 18 completed rows could not be precisely bounded as
contaminated-or-not (the daemon's cumulative CPU time suggested an earlier
active-build phase, not necessarily overlapping every one of those 18
rows) — per the conservative, established precedent (item 10c), all 18
were discarded rather than partially trusted. Reconfirmed clean via `top`
(99.4% system idle, load average decayed to 0.00-0.10) before restarting.
The daemon remained resident (not exited) throughout the rest of this
phase but stayed CPU-idle on every subsequent recheck (cumulative CPU
time grew by only 16-18 seconds across multi-hour windows) — treated as
no longer contending, not as fully clear; disclosed rather than silently
assumed clean.

**After the sweep:** load average 2.07 (transient, from the sweep's own
just-finished CPU usage), no other foreign process.

## Verification gate — STOP-AND-REPORT

At `--compute-ns-per-mib 0`, ratio 0.5, `--driver-threads 8
--lookahead-window 1`, arm D, n=3 (fresh runs, not reused from item 10e):

| Check | Rep 1 | Rep 2 | Rep 3 | Item 10e original |
|---|---|---|---|---|
| `pager_bytes_fetched` | 7,650,410,496 | 7,650,410,496 | 7,918,845,952 | 7,650,410,496 (all 3 reps) |
| `absent_handled` | 57 | 57 | 59 | 57 (all 3 reps) |
| `evictions` | 49 | 49 | 51 | 49 (all 3 reps) |
| `bytes_touched` | 10,737,418,240 | 10,737,418,240 | 10,737,418,240 | 10,737,418,240 |
| OPT `minimum_misses` | 48 | 48 | 48 | 48 |
| OPT `minimum_bytes_fetched` | 6,442,450,944 | 6,442,450,944 | 6,442,450,944 | 6,442,450,944 |

**PASS.** Reps 1-2 match item 10e's original values exactly. Rep 3 differs
(7,918,845,952 / 59 / 51) but this is not a new or anomalous value —
confirmed by direct lookup that 7,918,845,952 already appears in item
10e's own Sweep B data (at a different window/thread cell, same ratio),
i.e. within the already-documented, already-understood timing-dependent
eviction-order variance that exists at `--lookahead-window > 0` (item
10e's own Anomalies section). Reference trace, `bytes_touched`, and OPT
— the three quantities that are structurally deterministic regardless of
scheduling — matched exactly on every rep. Also spot-checked (not part of
the formal gate, but relevant): ratio 0.25's `pager_bytes_fetched`
(9,529,458,688) and ratio 0.75's (5,502,926,848, exact on all 3 original
reps) both fall within/match item 10e's own originally observed values at
those ratios too.

**Confirmed via `objdump`** that `compute_unit()`'s arithmetic (inlined
into `compute_busy.part.0` and `replay_calibrate_compute` at `-O2`) is not
eliminated: a register-register `imul %rdi,%rax` (the multiply, constant
loaded separately), `movzbl 0x0(%rbp,%rdx,1),%eax` (the byte read from
the buffer), and `xor`/`shr` (the mixing step) are all present in the
disassembly.

## Calibration accuracy, disclosed rather than assumed

A real discrepancy was found and investigated (not fixed by relaxing
verification): a first calibration attempt (buffer = 4096 bytes) measured
achieved ns/MiB at 2.6× the requested value (1,045,176 vs. 400,000
requested). Root-cause hypothesis: `compute_unit()`'s loop only ever
touches indices `[0, COMPUTE_UNIT_OPS)` (65,536) via `i % len` — for any
`len >= COMPUTE_UNIT_OPS` (every real chunk in this project), the modulo
never wraps and the access pattern is a plain 64 KiB linear scan,
regardless of how much bigger the real buffer is. A 4096-byte calibration
buffer touches a footprint 16× smaller and wraps constantly, which could
plausibly measure a faster, more cache-favorable rate than production.
**Fixed the calibration buffer to match the real 64 KiB footprint exactly**
— this did not resolve the discrepancy (re-measured smoke test: achieved
1,127,111 ns/MiB against the same 400,000 request, statistically
unchanged). The cache-footprint hypothesis is therefore not the (or not
the only) explanation. The more likely remaining cause, not isolated
further given the time-box: **CPU/cache/memory-bandwidth contention among
the 8 concurrently-running compute threads** — calibration runs solo,
single-threaded, before any driver thread starts; production compute runs
with up to 8 threads (plus up to 4 fetch workers, plus the dispatcher)
all competing for the same cores simultaneously. Measured directly across
the full sweep: achieved/requested is not even a constant multiplier —
requested 100,000 achieved a median of 205,042 ns/MiB (≈2.05×), requested
400,000 achieved a median of 1,203,315 ns/MiB (≈3.01×) — the ratio itself
grows with the requested load, consistent with contention that worsens as
more concurrent compute work is scheduled, not a fixed calibration
offset. **Reported as measured, not corrected further** — the campaign's
own design anticipated exactly this ("report the achieved ns/MiB against
the requested value, so the parameter is verified rather than assumed").
All results below are interpreted against the ACHIEVED values, not the
nominal 0/100000/400000 labels.

## Sweep

Arm D and arm E, 3 ratios × n=3(+), async, `--fetch-workers 4
--driver-threads 8 --lookahead-window 1`, `--prefetch-depth` {1,2,4} ×
`--prefetch-retention` {none,pinned} for arm E, `--compute-ns-per-mib`
{0, 100000, 400000} — 189 planned runs. Raw data: `results/data/synthetic-compute-phase-sweep.csv`
(191 data rows: a resume-boundary artifact gave cell (ratio=0.25, E,
depth=4, retention=none, compute=0) 4 reps instead of 3 — all 4 are
genuine, independently-executed, non-error runs with real, differing
measured values, not literal duplicates; kept in the raw CSV, median used
for that cell same as every other). Full log:
`experiments/logs/phase2_compute_log.txt`. Zero `DISCREPANCY`, `FAIL`, or
`RECONCILE FAILED` lines across the whole sweep; every run `rc=0`, no
timeouts.

### Table 1 — Arm D vs Arm E (depth=2, `pinned`), `read_bytes` and wall-clock

| Ratio | Compute (requested) | D bytes | E bytes | E/D | D wall (s) | E wall (s) |
|---|---|---|---|---|---|---|
| 0.25 | 0 | 9,529,458,688 | 12,348,030,976 | 1.296 | 4.551 | 4.318 |
| 0.25 | 100000 | 9,529,458,688 | 13,421,772,800 | 1.408 | 5.128 | 6.085 |
| 0.25 | 400000 | 13,421,772,800 | 16,508,780,544 | 1.230 | 6.335 | 7.004 |
| 0.50 | 0 | 7,650,410,496 | 9,797,894,144 | 1.281 | 3.301 | 3.742 |
| 0.50 | 100000 | 8,053,063,680 | 9,932,111,872 | 1.233 | 3.868 | 3.845 |
| 0.50 | 400000 | 10,603,200,512 | 10,334,765,056 | **0.975** | 5.176 | 4.411 |
| 0.75 | 0 | 5,502,926,848 | 6,174,015,488 | 1.122 | 2.570 | 2.567 |
| 0.75 | 100000 | 5,502,926,848 | 6,710,886,400 | 1.220 | 2.543 | 2.869 |
| 0.75 | 400000 | 6,576,668,672 | 6,308,233,216 | **0.959** | 3.828 | 3.597 |

### Table 2 — Hit rate, concurrently-outstanding fetches, `pin_broken`, `infeasible` (all arm E cells)

| Ratio | Depth | Retention | Compute | n prefetches | Hit rate | Concur. outstanding (median) | `pin_broken` | `infeasible` |
|---|---|---|---|---|---|---|---|---|
| 0.25 | 1 | none | 0/100000/400000 | 87/94/86 | 0.24/0.20/0.10 | 2.00/2.00/2.00 | 0/0/0 | 0/0/0 |
| 0.25 | 1 | pinned | 0/100000/400000 | 93/86/82 | 0.29/0.23/0.14 | 2.00/2.00/1.00 | 0/0/0 | 0/0/0 |
| 0.25 | 2 | none | 0/100000/400000 | 122/147/150 | 0.18/0.16/0.15 | 2.00/2.00/2.00 | 0/0/0 | 0/0/0 |
| 0.25 | 2 | pinned | 0/100000/400000 | 111/124/141 | 0.26/0.23/0.15 | 2.00/2.00/2.00 | 6/7/10 | 1/0/0 |
| 0.25 | 4 | none | 0/100000/400000 | 360/227/221 | 0.16/0.16/0.14 | 3.00/3.00/3.00 | 0/0/0 | 0/0/0 |
| 0.25 | 4 | pinned | 0/100000/400000 | 148/168/203 | 0.18/0.16/0.14 | 3.00/3.00/3.00 | 31/32/44 | 43/43/33 |
| 0.50 | 1 | none | 0/100000/400000 | 54/61/63 | 0.22/0.18/0.10 | 1.00/1.00/1.00 | 0/0/0 | 0/0/0 |
| 0.50 | 1 | pinned | 0/100000/400000 | 60/57/58 | 0.20/0.19/0.24 | 1.00/1.00/1.00 | 0/0/0 | 0/0/0 |
| 0.50 | 2 | none | 0/100000/400000 | 77/92/91 | 0.28/0.15/0.19 | 1.00/2.00/1.00 | 0/0/0 | 0/0/0 |
| 0.50 | 2 | pinned | 0/100000/400000 | 78/82/90 | 0.26/0.23/0.24 | 2.00/1.00/1.00 | 0/0/0 | 0/0/0 |
| 0.50 | 4 | none | 0/100000/400000 | 126/115/142 | 0.21/0.21/0.17 | 2.00/2.00/2.00 | 0/0/0 | 0/0/0 |
| 0.50 | 4 | pinned | 0/100000/400000 | 135/126/124 | 0.25/0.29/0.27 | 3.00/2.00/2.00 | 0/0/0 | 0/0/0 |
| 0.75 | 1 | none | 0/100000/400000 | 24/29/30 | 0.25/0.18/0.22 | 1.00/1.00/1.00 | 0/0/0 | 0/0/0 |
| 0.75 | 1 | pinned | 0/100000/400000 | 27/29/34 | 0.33/0.30/0.31 | 1.00/1.00/1.00 | 0/0/0 | 0/0/0 |
| 0.75 | 2 | none | 0/100000/400000 | 38/41/35 | 0.14/0.14/0.17 | 1.00/1.00/1.00 | 0/0/0 | 0/0/0 |
| 0.75 | 2 | pinned | 0/100000/400000 | 33/40/35 | 0.36/0.29/0.27 | 1.00/1.00/1.00 | 0/0/0 | 0/0/0 |
| 0.75 | 4 | none | 0/100000/400000 | 51/39/51 | 0.27/0.27/0.22 | 1.00/1.00/1.00 | 0/0/0 | 0/0/0 |
| 0.75 | 4 | pinned | 0/100000/400000 | 46/40/45 | 0.47/0.44/0.38 | 1.00/1.00/1.00 | 0/0/0 | 0/0/0 |

**Arm D's concurrently-outstanding stayed at exactly 1.00 in every one of
9 (ratio, compute) cells** — compute alone, with prefetch off, never
produces overlap (unsurprising: with no prefetch there is still only ever
one thing to fetch).

## Pre-registered expectations

1. **At `--compute-ns-per-mib 0`, all results reproduce item 10d/10e:
   HELD.** Confirmed by the formal gate above (ratio 0.5) and spot-checks
   at ratios 0.25 and 0.75 — every compute=0 value observed either
   matches item 10e's original exactly or falls within its already-
   documented variance set.
2. **Prefetch hit rate rises with compute time; at 400000 it exceeds the
   14-46% band substantially: DID NOT HOLD — the opposite happened in
   most cells.** Of the 18 (ratio, depth, retention) series in Table 2,
   hit rate FELL from compute=0 to compute=400000 in 14 of 18; rose in 2
   (0.50/depth=1/pinned: 0.20→0.24; 0.50/depth=4/pinned: 0.25→0.27); was
   roughly flat in 2 (0.75/depth=1/pinned, 0.75/depth=4/none). No cell
   exceeded the 14-46% band at compute=400000; several cells (0.25/depth=1
   both retentions, 0.50/depth=1/none) fell to their LOWEST hit rate of
   the whole sweep specifically at the highest compute setting (0.10).
   Not chased into a proposed fix, per instruction — reported as the
   measured, opposite-of-expected result. A plausible (not verified)
   mechanism: heavier compute lengthens how long each step takes, which
   under `--lookahead-window 1`'s bound lengthens how long a prefetched
   chunk must survive between arrival and consumption without buying it
   any more protection — more real wall-clock time for the FIFO cap or a
   demand-driven pin-break to evict it before use, not less.
3. **Concurrently-outstanding fetches exceeds 1.0 on arm E once compute
   time is non-zero: DID NOT HOLD, because the premise was already false
   at compute=0.** Phase 0.1 already established arm E shows real overlap
   (median 1-3) without any compute at all — Table 2 confirms this
   directly in-sweep: concurrently-outstanding is >1.0 at compute=0 in 11
   of 18 series. Adding compute does not show a consistent further rise:
   it stays flat in most series, and in 2 of 18 (0.25/depth=1/pinned:
   2.00→1.00; 0.50/depth=2/pinned: 2.00→1.00) it falls. There is no
   evidence compute is what produces overlap on arm E — prefetch itself
   already does, independent of compute.
4. **Arm E's `read_bytes` falls below arm D's at r=0.75 at the highest
   compute setting: HELD.** E/D = 0.959 at r=0.75/compute=400000
   (6,308,233,216 vs 6,576,668,672) — the first time in this project's
   entire history that arm E has beaten arm D on `read_bytes` at this
   scale under `layer_order`. **Also held, unrequested but notable, at
   r=0.5/compute=400000** (E/D = 0.975, 10,334,765,056 vs
   10,603,200,512) — the pattern is not confined to the one ratio the
   expectation named. Does NOT hold at r=0.25/compute=400000 (E/D=1.230,
   E still well above D).
5. **Wall-clock advantage of E over D grows with compute time: MIXED,
   holds at 2 of 3 ratios, not at the third.** At r=0.5: E goes from
   slower than D at compute=0 (3.742s vs 3.301s) to faster at compute=
   400000 (4.411s vs 5.176s) — a real reversal in E's favor as compute
   rises. At r=0.75: similar pattern, ending with E faster at compute=
   400000 (3.597s vs 3.828s) after a non-monotonic middle point (E
   slower at compute=100000). At r=0.25: the pattern is the OPPOSITE — E
   starts faster at compute=0 (4.318s vs 4.551s) and becomes
   progressively SLOWER at both higher compute settings (6.085s vs
   5.128s at 100000; 7.004s vs 6.335s at 400000). Reported per-ratio, not
   forced into one verdict.
6. **Retention's byte effect under compute — reported, not reconciled
   with prior items, per instruction.** At depth=2 (Table 4 data, folded
   into Table 1's D/E columns using depth=2/pinned specifically): the
   sign of (pinned vs. none) flips inconsistently across every
   (ratio, compute) cell tested — `pinned<none` at 3 of 9 cells (0.25/0,
   0.25/100000, 0.5/100000), `pinned>none` at 5 of 9, tied at 1 of 9 —
   with no visible correlation to compute level (both directions appear
   at low and high compute alike). This matches neither item 10d's
   pattern (a budget-tightness-correlated regime split) nor item 10e's
   (retention consistently costing more bytes under real overlap)
   cleanly; reported as its own, third, mixed pattern rather than forced
   into either.

## Correctness

T-1 through T-7 re-run with `--eager-reconcile` after this phase's driver
change, all binaries rebuilt from a clean `make all` first (a `make clean`
+ partial rebuild during the calibration fix had left several binaries,
including the correctness-harness ones, stale/missing — caught by the
harness itself failing with "No such file or directory" rather than
silently running old binaries, and fixed before re-running, not papered
over). All PASS: T-1 (0 mismatches, all 3 policies), T-2 (0 mismatches,
999 evictions), T-3 (0 mismatches, no hang), T-4 (exact
`memory.stat[shmem]` match), T-5 (belady selftest, 300/300 + exact floor
checks), T-6 (`dedup_fetching=13557>0`), T-7 (all 8 threads joined inside
the 120s watchdog). Full logs: `experiments/logs/correctness_harness_log.txt`,
`experiments/logs/t6_t7_log.txt` (both overwritten with this item's run, per the
project's established convention of the harness logs reflecting the most
recent verified state).

## Anomalies

- **The calibration discrepancy itself** (achieved ns/MiB 2-3× the
  requested value, growing with requested load) — see the dedicated
  section above. Not resolved; reported as measured.
- **One cell (`ratio=0.25, arm=E, depth=4, retention=none, compute=0`)
  has 4 reps instead of 3** — a resume-boundary artifact from the
  contamination-driven restart sequence (see Machine exclusivity above).
  All 4 rows are genuine independent runs (rc=0, differing real values,
  no duplication of a single measurement). Median used for consistency
  with every other cell.
- Hit rate FALLING with more compute time, the opposite of the
  pre-registered direction, in the large majority of series — the
  central, disclosed negative result of this phase (expectation 2 above).

## What I did NOT test

- Compute settings between 100000 and 400000, or above 400000 — only the
  two nominal brackets the campaign specified, at their ACHIEVED (not
  requested) rates.
- `--compute-ns-per-mib` at `--driver-threads 1` or under `--sync-handler`
  — the sweep used only the async/8-thread configuration the campaign
  specified.
- Isolating the calibration discrepancy's root cause beyond the
  cache-footprint hypothesis (tested and ruled out) — CPU/cache/memory-
  bandwidth contention among concurrent compute threads is offered as the
  most plausible remaining explanation, not independently verified.
- Any chunk size or region size other than the V2 scale.

## Final check

- No number estimated, inferred, or copied from documentation — every
  value in every table is a direct read from `results/data/synthetic-compute-phase-sweep.csv`
  (median of n=3, or n=4 for the one disclosed extra-rep cell) or a
  direct computation over retained `--fetch-trace` binary files
  (`scratch/analyze_phase2.py`), or a value cross-checked against
  `results/data/historical/task-e-sweep-b.csv`'s own recorded numbers.
- No test was modified. The correctness harness's binaries were rebuilt
  (they were stale, not edited) before re-running, and the harness itself
  caught the staleness rather than silently passing on old code.
- Every pre-registered expectation (1-6) was checked against a measured
  value and reported at the granularity the data actually supports — held
  cleanly (1, 4), held with reversal disclosed (2, 3 — both stated
  clearly as NOT holding, opposite of hoped), mixed and reported per-
  ratio rather than forced into one verdict (5), and reported as a third,
  unreconciled pattern rather than blended into either prior item's
  finding (6).
- The STOP-AND-REPORT gate was evaluated and passed; had it failed, this
  report would stop here per instruction — it did not.
