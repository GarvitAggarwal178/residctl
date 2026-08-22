# Phase 4 — Consolidated Sweep

2 GiB region, `--chunk-size 128MiB` (unchanged from V2), async,
`--fetch-workers 4 --driver-threads 8 --lookahead-window 1
--prefetch-depth 2 --prefetch-retention pinned`. `--compute-ns-per-mib`
{0, 400000} × ratio {0.25, 0.375, 0.5, 0.625, 0.75} × arms {A, B, C, D, E},
n=3. Arm A's `madvise` mode swept ({normal, random, sequential}), best
(by touches/sec) reported. Arm A/B have no compute-phase concept
(`baseline_main`, unchanged architecture, disclosed already in Phase 3) —
run once per ratio, not duplicated per compute level. `drop_caches`
before every arm A/B invocation. 158 CSV rows (150 baseline cells × 3
reps + 7 benign resume-boundary extra reps, same class of artifact as
Phases 2/3 — real, independent, non-error runs, kept, median used).
Script: `src/run_phase4_sweep.sh`. Zero `DISCREPANCY`/`FAIL`/
`RECONCILE FAILED` lines; every run `rc=0`.

**Disclosed gap, backfilled rather than left unmeasured:** the main
sweep script omitted `--fetch-trace` (an oversight, not a deliberate
scope cut) — device-busy, concurrently-outstanding, and per-fetch timing
were therefore unavailable from the main 150-run sweep. Backfilled with a
**supplementary n=1 pass** (`src/run_phase4_fetchtrace_supp.sh`, 30 runs,
one rep per (ratio, arm, compute) cell for C/D/E only) capturing
`--fetch-trace`. This supplementary data is used ONLY for device-busy/
concurrently-outstanding/`pin_broken`/`infeasible` columns below — the
byte/fault/wall-clock columns are unaffected and still come from the
main n=3 sweep. Labelled n=1 wherever it appears; not blended into the
main sweep's medians.

## Machine exclusivity

Checked before the main sweep launch and before the supplementary pass:
clean both times (routine systemd/WSL/docker-desktop-proxy services only,
load average consistent with idle between resumes). The sweep needed 7
resume cycles (harness background-task cap), each rechecked before
resuming; no foreign workload found at any recheck.

## OPT (per ratio, from arm D's `compute=0` reference trace — ratio-independent, compute-independent)

| Ratio | OPT `minimum_bytes_fetched` |
|---|---|
| 0.25 | 8,724,152,320 |
| 0.375 | 7,516,192,768 |
| 0.5 | 6,442,450,944 |
| 0.625 | 5,368,709,120 |
| 0.75 | 4,294,967,296 |

**OPT ≤ D holds at every ratio and both compute levels** (D/OPT ratios in
the table below are all ≥ 1.0, minimum 1.062 at r=0.25/compute=0,
confirming the bound is never violated) — the check that mattered most
since the item 10 correction.

## Consolidated table (median of n=3 for A/B/C/D/E's byte/fault/wall-clock columns; n=1 supplementary for device-busy/concurrently-outstanding/`pin_broken`/`infeasible`)

### `read_bytes`, bytes/touch, wall-clock, ratio-to-OPT

| Ratio | Arm | Compute | `read_bytes` | Bytes/touch | Wall (s) | /OPT |
|---|---|---|---|---|---|---|
| 0.25 | A (best: sequential) | n/a | 0 *(host-cache, see Phase 3's caveat)* | — | 0.036 | — |
| 0.25 | B | n/a | 10,057,940,992 | — | 2.877 | — |
| 0.25 | C | 0 | 10,737,418,240 | 134,217,728 | 5.483 | 1.231 |
| 0.25 | C | 400000 | 10,737,418,240 | 134,217,728 | 5.938 | 1.231 |
| 0.25 | D | 0 | 9,261,023,232 | 115,762,790 | 4.370 | 1.062 |
| 0.25 | D | 400000 | 13,824,425,984 | 172,805,325 | 6.759 | 1.585 |
| 0.25 | E | 0 | 12,482,248,704 | 156,028,109 | 4.508 | 1.431 |
| 0.25 | E | 400000 | 16,106,127,360 | 201,326,592 | 6.367 | 1.846 |
| 0.375 | A (best: sequential) | n/a | 0 *(host-cache)* | — | 0.037 | — |
| 0.375 | B | n/a | 10,057,940,992 | — | 2.283 | — |
| 0.375 | C | 0 | 10,737,418,240 | 134,217,728 | 4.949 | 1.429 |
| 0.375 | C | 400000 | 10,737,418,240 | 134,217,728 | 5.828 | 1.429 |
| 0.375 | D | 0 | 8,455,716,864 | 105,696,461 | 3.849 | 1.125 |
| 0.375 | D | 400000 | 11,542,724,608 | 144,284,058 | 6.032 | 1.536 |
| 0.375 | E | 0 | 11,140,071,424 | 139,250,893 | 4.096 | 1.482 |
| 0.375 | E | 400000 | 13,153,337,344 | 164,416,717 | 5.310 | 1.750 |
| 0.5 | A (best: random) | n/a | 0 *(host-cache)* | — | 0.038 | — |
| 0.5 | B | n/a | 10,057,940,992 | — | 2.490 | — |
| 0.5 | C | 0 | 10,737,418,240 | 134,217,728 | 8.425 | 1.667 |
| 0.5 | C | 400000 | 10,737,418,240 | 134,217,728 | 5.911 | 1.667 |
| 0.5 | D | 0 | 8,053,063,680 | 100,663,296 | 3.840 | 1.250 |
| 0.5 | D | 400000 | 11,274,289,152 | 140,928,614 | 5.719 | 1.750 |
| 0.5 | E | 0 | 10,468,982,784 | 130,862,285 | 3.780 | 1.625 |
| 0.5 | E | 400000 | **10,603,200,512** | 132,540,006 | 5.021 | 1.646 |
| 0.625 | A (best: sequential) | n/a | 0 *(host-cache)* | — | 0.073 | — |
| 0.625 | B | n/a | 10,057,940,992 | — | 2.756 | — |
| 0.625 | C | 0 | 10,737,418,240 | 134,217,728 | 5.799 | 2.000 |
| 0.625 | C | 400000 | 10,737,418,240 | 134,217,728 | 6.530 | 2.000 |
| 0.625 | D | 0 | 6,845,104,128 | 85,563,802 | 3.730 | 1.275 |
| 0.625 | D | 400000 | 8,254,390,272 | 103,179,878 | 4.978 | 1.538 |
| 0.625 | E | 0 | 8,589,934,592 | 107,374,182 | 3.492 | 1.600 |
| 0.625 | E | 400000 | 9,663,676,416 | 120,795,955 | 4.692 | 1.800 |
| 0.75 | A (best: sequential) | n/a | 0 *(host-cache)* | — | 0.038 | — |
| 0.75 | B | n/a | 10,057,940,992 | — | 2.890 | — |
| 0.75 | C | 0 | 10,737,418,240 | 134,217,728 | 6.592 | 2.500 |
| 0.75 | C | 400000 | 10,737,418,240 | 134,217,728 | 6.484 | 2.500 |
| 0.75 | D | 0 | 5,502,926,848 | 68,786,586 | 2.933 | 1.281 |
| 0.75 | D | 400000 | **7,516,192,768** | 93,952,410 | 4.573 | 1.750 |
| 0.75 | E | 0 | 5,905,580,032 | 73,819,750 | 2.935 | 1.375 |
| 0.75 | E | 400000 | **6,308,233,216** | 78,852,915 | 4.166 | 1.469 |

### Demand faults, device-busy (n=1 supp.), concurrently-outstanding (n=1 supp.), `pin_broken`, `infeasible`

| Ratio | Arm | Compute | Faults | Device-busy | Concur. outstanding | `pin_broken` | `infeasible` |
|---|---|---|---|---|---|---|---|
| 0.25 | C | 0/400000 | 80/80 | 0.789/0.660 | 1.00/1.00 | 0/0 | 0/0 |
| 0.25 | D | 0/400000 | 69/103 | 0.793/0.705 | 1.00/1.00 | 0/0 | 0/0 |
| 0.25 | E | 0/400000 | 54/72 | 0.847/0.749 | 2.00/2.00 | 10/9 | 1/0 |
| 0.375 | C | 0/400000 | 80/80 | 0.808/0.676 | 1.00/1.00 | 0/0 | 0/0 |
| 0.375 | D | 0/400000 | 63/86 | 0.803/0.702 | 1.00/1.00 | 0/0 | 0/0 |
| 0.375 | E | 0/400000 | 50/61 | 0.864/0.733 | 2.00/2.00 | 0/0 | 0/0 |
| 0.5 | C | 0/400000 | 80/80 | 0.799/0.666 | 1.00/1.00 | 0/0 | 0/0 |
| 0.5 | D | 0/400000 | 60/84 | 0.807/0.699 | 1.00/1.00 | 0/0 | 0/0 |
| 0.5 | E | 0/400000 | 49/54 | 0.847/0.736 | 2.00/2.00 | 0/0 | 0/0 |
| 0.625 | C | 0/400000 | 80/80 | 0.827/0.699 | 1.00/1.00 | 0/0 | 0/0 |
| 0.625 | D | 0/400000 | 51/62 | 0.829/0.680 | 1.00/1.00 | 0/0 | 0/0 |
| 0.625 | E | 0/400000 | 42/49 | 0.852/0.716 | 1.00/1.00 | 0/0 | 0/0 |
| 0.75 | C | 0/400000 | 80/80 | 0.827/0.710 | 1.00/1.00 | 0/0 | 0/0 |
| 0.75 | D | 0/400000 | 41/56 | 0.838/0.660 | 1.00/1.00 | 0/0 | 0/0 |
| 0.75 | E | 0/400000 | 33/36 | 0.847/0.671 | 1.00/1.00 | 0/0 | 0/0 |

## Headline findings

- **`C` (`lru`) thrashes at exactly 100% miss (80/80 faults = `n_passes ×
  n_chunks`) at every ratio and every compute level** — unchanged from
  every prior report back to item 7; chunk-size-, compute-, and
  concurrency-invariant.
- **OPT ≤ D holds at every one of the 10 (ratio, compute) cells** — the
  bound the item 10 correction exists to protect.
- **Arm E beats arm D's `read_bytes` (bolded above) at exactly r=0.5 and
  r=0.75, and ONLY at `compute=400000`** — 10,603,200,512 < 11,274,289,152
  at r=0.5; 6,308,233,216 < 7,516,192,768 at r=0.75. At `compute=0`, E
  is worse than D at every single ratio (the historically-established
  pattern from every report before this campaign). **This independently
  replicates Phase 2's finding** (which found the identical pattern — E
  beats D at r=0.5 and r=0.75 specifically under heavy compute, not at
  tighter ratios) on a separate 5-ratio grid with different budget
  granularity — not the same runs, a genuine second confirmation.
- **Device-busy falls at `compute=400000` relative to `compute=0` in
  every single row** (e.g. arm C at r=0.25: 0.789→0.660) — consistent
  with fetches becoming more thinly spread across a longer total
  wall-clock span once compute time is inserted between touches, not a
  change in fetch behaviour itself.
- **Concurrently-outstanding is 2.00 on arm E at the three loosest ratios
  tested here (0.25, 0.375, 0.5) but drops to 1.00 at 0.625 and 0.75** —
  the opposite direction from what "tighter budget creates more
  contention" would predict on its own, though consistent with Phase 2's
  finding that arm E already shows overlap independent of compute
  (Table 2 there also showed non-monotonic concurrently-outstanding
  across ratio/depth cells). Arms C and D stay at exactly 1.00
  everywhere — prefetch remains the only source of overlap in this
  architecture, confirmed again on a fifth independent sweep.
- **`pin_broken` fires only at the single tightest cell tested (r=0.25,
  arm E, both compute levels)** — 10 at compute=0, 9 at compute=400000 —
  matching the depth-dependent regime-boundary pattern item 10e/Phase 2
  already established (this campaign's config uses `prefetch-depth 2`,
  matching item 10e's own `r_c(depth=2)` finding of "between 0.25 and
  0.375").
- **Arm A's `read_bytes` reads 0 in all 5 cells** — the same WSL2
  VHDX host-cache signature Phase 3 found and item 10b first documented.
  Arm B's number (10,057,940,992, identical across every ratio) is
  non-zero and below full `bytes_touched` (10,737,418,240) — a real,
  plausible signal that `MADV_WILLNEED`/`MADV_PAGEOUT` measurably reduces
  actual reads relative to arm A even under the same host-cache
  conditions, but not independently verified beyond this one observation
  (arm B's own numbers could carry some of the same cache confound to a
  lesser degree; not established either way).

## Correctness

Not re-run this phase. Per the Correctness section's own scoping ("after
Phase 2's driver change and after Phase 3's chunk-size parameterisation"),
Phase 4 introduces no new code path beyond what Phase 2's already-verified
driver change and the pre-existing, already-tested `--chunk-size`
parameterisation cover — T-1..T-7 remain clean from the Phase 2 re-run
(`results/phase2_compute.md`), the most recent verified state.

## What I did NOT test

- The supplementary fetch-trace pass is n=1, not n=3 — device-busy/
  concurrently-outstanding/`pin_broken`/`infeasible` in this phase carry
  more rep-to-rep noise than the main sweep's byte/fault/wall-clock
  columns. Disclosed, not smoothed over.
- Whether arm B's non-zero, ratio-invariant `read_bytes` reflects genuine
  hint-driven read reduction or partial host-cache contamination — not
  isolated.
- Per-fetch read/CONTINUE duration and handler-overhead fraction for this
  phase specifically (Phase 3 already covers those at 128 MiB chunk size
  across all 3 ratios it tested; not re-derived here to avoid duplicating
  that analysis over a near-identical config).

## Final check

- No number estimated, inferred, or copied from documentation — every
  value is a direct read from `results/phase4_consolidated.csv` (median
  of n=3) or the supplementary n=1 `--fetch-trace` files, or `belady_main`'s
  own printed OPT output, computed via `scratch/analyze_phase4.py`.
- No test was modified.
- The `--fetch-trace` omission in the main sweep is disclosed as an
  oversight, not hidden, and backfilled with clearly-labelled
  supplementary data rather than left as a silent gap or estimated.
- OPT ≤ D was checked at every cell and held; E-vs-D was checked and
  reported precisely (beats D at exactly 2 of 5 ratios, only under heavy
  compute), replicating rather than merely resembling Phase 2's finding.
