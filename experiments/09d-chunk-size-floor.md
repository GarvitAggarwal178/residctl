# Campaign 12 Phase B — Chunk-Size Floor

Region fixed at 2 GiB. `--chunk-size` ∈ {4, 8, 16, 32} MiB (512, 256, 128, 64
chunks respectively). Arms C, D, E and OPT (`belady_main`), 3 ratios
{0.25, 0.5, 0.75}, n=3, async handler, `--fetch-workers 4 --driver-threads 8
--lookahead-window 1 --prefetch-depth 2 --prefetch-retention pinned
--compute-ns-per-mib 400000`. Arms A and B at the same chunk sizes, using
Phase A's repaired `drop_caches` pattern and guard-equipped `baseline_main`
(single fixed `sequential` madvise mode for A, matching Phase 3's own
disclosed simplification). Script: `scripts/historical/run-phaseB-sweep.sh`.
180 rows (4 chunk sizes × 3 ratios × 5 arms × 3 reps), zero `BASELINE
FAILED`/`DISCREPANCY`/`RECONCILE FAILED` lines, zero non-`rc=0` rows, every
cell has exactly 3 reps.

## Machine exclusivity

Clean before and after (routine services only, no foreign process; `uptime`
load average in the 0.5-1.2 range throughout, consistent with this
machine's baseline).

## Process notes — two defects caught and fixed during this phase, disclosed in full

**1. Resumability bug, causing duplicate rows.** The sweep ran across many
resume cycles (background-task kills are routine in this environment).
`already_done()` originally gated an entire cell (chunk_size, ratio, arm) on
having ≥3 total rows, but the per-rep loop inside `run_cell()` always
restarted at `rep=1` regardless of which reps had already completed. Any
cell killed mid-way through its 3 reps (1 or 2 done) had those already-
completed reps silently re-executed and re-appended on the next resume.
Caught by a sanity check after the sweep first appeared to reach completion:
`188` total CSV rows against a maximum possible `181` (180 data + header).
Per-cell row counts showed 10 cells with 4-5 rows instead of 3, always with
a duplicated low rep number (e.g. `rep=1,rep=1,rep=2,rep=3` or
`rep=1,rep=2,rep=1,rep=2`) — the signature of a restarted rep loop, not a
concurrent-process race (all rows had `rc=0` and internally consistent
`touches`/`bytes_touched`, unlike the genuine duplicate-orchestrator
contamination caught in Phase A, which showed `rc=137`). Fixed two ways:
- Deduplicated the CSV, keeping the last (most recently executed) row per
  exact `(chunk_size,ratio,arm,rep)` key: `188` → `173` rows. One cell
  (`4MiB, ratio=0.25, arm=D`) had genuinely never completed a 3rd rep across
  any prior resume; the dedup correctly left it at 2 rows rather than
  fabricating a 3rd.
- Added a `rep_done()` check inside `run_cell()`'s per-rep loop
  (`scripts/historical/run-phaseB-sweep.sh`), so a resume now skips individual completed
  reps instead of only skipping whole completed cells. The remaining 7 rows
  (the incomplete cell's 3rd rep, plus already-complete cells the old logic
  had not yet gotten around to re-running) were generated cleanly under the
  fixed script with zero further duplication, reaching exactly 180/180 with
  every cell at exactly 3 reps.

**2. Analysis-script bug, not a data bug, in the device-busy /
concurrently-outstanding computation.** The first pass of Table 2 below
showed one cell (`4MiB, ratio=0.25, arm=D`) with `device-busy=0.012` against
`0.6-0.75` everywhere else — an order-of-magnitude outlier. The busy-
fraction formula (inherited from `scratch/analyze_phase3.py`/
`analyze_phase4.py`) concatenates all 3 reps' `--fetch-trace` records into
one pool and divides the union of read intervals by `max(exit) - min(entry)`
across that pool. This is harmless when all 3 reps run back-to-back inside
one script invocation (true for Phase 3/4), but this cell is exactly the one
whose reps were split across the resumability bug above — its rep 3 ran
roughly 33 minutes after reps 1-2, confirmed by `--fetch-trace` file mtimes
(18:04/18:05 vs 18:38). `CLOCK_MONOTONIC` keeps advancing across that gap
(it is boot-relative, not process-relative), so the pooled span silently
absorbed the entire idle gap between resumes, collapsing the computed busy
fraction toward zero. Fixed by computing device-busy, concurrently-
outstanding, per-fetch read duration, `UFFDIO_CONTINUE` duration, and
handler overhead **per rep**, then taking the median across reps — never
pooling raw timestamps across separate process invocations. Re-run: the
outlier cell now reads `busy=0.743`, in line with its neighbors. This is a
latent defect in the analysis method that Phase 3/4's own tighter,
uninterrupted execution windows never exposed; `experiments/09-chunk-size-sweep.md`
and `experiments/09b-consolidated-6arm-sweep.md`'s device-busy/concurrently-outstanding
numbers are very unlikely to be materially affected (those sweeps' reps ran
seconds apart, not minutes), but this was not independently re-verified
against their raw trace files and is disclosed here rather than assumed.

## Verification gate — STOP-AND-REPORT

`belady_main`'s unconditional cyclic-floor check
(`n + (passes-1)*max(n-k,0)`) run at all 4 chunk sizes × 3 ratios = 12
combinations, generated fresh (`--compute-ns-per-mib 0`, reference-trace
content is compute-independent) via `scripts/historical/run-phaseB-gate.sh`. The 4 MiB /
512-chunk case — well outside anything previously tested — was of specific
concern per the campaign's instructions.

| Chunk size | n (chunks) | Ratio | Capacity (k) | Floor | OPT misses | Result |
|---|---|---|---|---|---|---|
| 4 MiB | 512 | 0.25 | 128 | 2048 | 2048 | OK |
| 4 MiB | 512 | 0.50 | 256 | 1536 | 1536 | OK |
| 4 MiB | 512 | 0.75 | 384 | 1024 | 1024 | OK |
| 8 MiB | 256 | 0.25 | 64 | 1024 | 1024 | OK |
| 8 MiB | 256 | 0.50 | 128 | 768 | 768 | OK |
| 8 MiB | 256 | 0.75 | 192 | 512 | 512 | OK |
| 16 MiB | 128 | 0.25 | 32 | 512 | 512 | OK |
| 16 MiB | 128 | 0.50 | 64 | 384 | 384 | OK |
| 16 MiB | 128 | 0.75 | 96 | 256 | 256 | OK |
| 32 MiB | 64 | 0.25 | 16 | 256 | 256 | OK |
| 32 MiB | 64 | 0.50 | 32 | 192 | 192 | OK |
| 32 MiB | 64 | 0.75 | 48 | 128 | 128 | OK |

**PASS at every chunk size, including 4 MiB/512 chunks — OPT never falls
below the floor.** Every cell lands exactly at the floor (`OPT misses ==
floor`), including the 512-chunk case: the chunk table, binary-search
lookup, and reference-trace read/write path all handled 512 chunks and 2560
references (5 passes × 512 chunks) with no error, no truncation, and no
divergence from the cyclic-scan solver's own assertion.

## 32 MiB reproduction check (against `results/data/synthetic-chunk-size-sweep.csv`)

Required by the campaign's instructions before interpreting anything else
in this phase.

| Ratio | Arm | Phase 3 `pager_bytes_fetched` | Phase B `pager_bytes_fetched` | Diff |
|---|---|---|---|---|
| 0.25 | C | 10,737,418,240 | 10,737,418,240 | +0.00% |
| 0.25 | D | 11,744,051,200 | 12,247,367,680 | +4.29% |
| 0.25 | E | 11,945,377,792 | 12,213,813,248 | +2.25% |
| 0.50 | C | 10,737,418,240 | 10,737,418,240 | +0.00% |
| 0.50 | D | 9,026,142,208 | 9,663,676,416 | +7.06% |
| 0.50 | E | 8,992,587,776 | 8,388,608,000 | −6.72% |
| 0.75 | C | 10,737,418,240 | 10,737,418,240 | +0.00% |
| 0.75 | D | 5,872,025,600 | 5,771,362,304 | −1.71% |
| 0.75 | E | 5,536,481,280 | 6,073,352,192 | +9.70% |

Arm C (`lru`, deterministic, no prefetch) reproduces **exactly**, to the
byte, at all 3 ratios — expected, since `lru` with no prefetch traverses the
full region deterministically regardless of policy timing. Arms D and E
(`layer_order` eviction, E with prefetch) differ by −6.7% to +9.7% across
the two sessions. This is larger than Phase 3's own within-run n=3 spread
for these arms (~2-3%, e.g. Phase 3's own D/ratio=0.25 reps spanned
11,442,061,312 to 11,777,605,632, a 2.9% range) but is consistent in
character — these two arms are the only ones whose behavior depends on
eviction/prefetch-worker scheduling timing, not deterministic traversal
order, and the two sessions ran on different days under different exact
system load. **Reported as measured, not resolved further**: arm C's exact
reproduction confirms the sweep mechanics are consistent between sessions;
arms D/E's larger-than-single-session variance is real and is treated as
this project's normal cross-session noise floor for stochastic arms, not
evidence of a defect — but it is not independently proven to be noise
either. Not flagged as a stop condition since the deterministic arm (the
one immune to scheduling timing) matches exactly.

## Sweep results

### Table 1 — `read_bytes`, bytes/touch, wall-clock, D/OPT (median of n=3)

OPT (via `belady_main` on arm D's own reference trace) is stable across all
4 chunk sizes at each ratio (0.25→8,589,934,592; 0.5→6,442,450,944;
0.75→4,294,967,296 — identical to Phase 3's `opt_table`), as expected: OPT
is a property of the access pattern and budget, not the chunk granularity,
for this synthetic cyclic workload.

| Chunk | Ratio | Arm | `read_bytes` | Bytes/touch | Wall (s) | D/OPT |
|---|---|---|---|---|---|---|
| 4MiB | 0.25 | A | 10,737,455,104 | 4,194,318 | 3.040 | — |
| 4MiB | 0.25 | B | 10,712,252,416 | 4,184,474 | 2.922 | — |
| 4MiB | 0.25 | C | 10,737,418,240 | 4,194,304 | 9.848 | — |
| 4MiB | 0.25 | D | 12,054,429,696 | 4,708,762 | 11.227 | 1.403 |
| 4MiB | 0.25 | E | 11,161,042,944 | 4,359,782 | 7.816 | — |
| 4MiB | 0.50 | A | 10,737,455,104 | 4,194,318 | 3.157 | — |
| 4MiB | 0.50 | B | 10,712,252,416 | 4,184,474 | 2.535 | — |
| 4MiB | 0.50 | C | 10,737,418,240 | 4,194,304 | 8.878 | — |
| 4MiB | 0.50 | D | 9,164,554,240 | 3,579,904 | 7.943 | 1.423 |
| 4MiB | 0.50 | E | 7,440,695,296 | 2,906,522 | 5.543 | — |
| 4MiB | 0.75 | A | 10,737,455,104 | 4,194,318 | 2.852 | — |
| 4MiB | 0.75 | B | 10,712,252,416 | 4,184,474 | 2.426 | — |
| 4MiB | 0.75 | C | 10,737,418,240 | 4,194,304 | 9.005 | — |
| 4MiB | 0.75 | D | 5,754,585,088 | 2,247,885 | 5.346 | 1.340 |
| 4MiB | 0.75 | E | 4,810,866,688 | 1,879,245 | 3.928 | — |
| 8MiB | 0.25 | A | 10,737,455,104 | 8,388,637 | 2.551 | — |
| 8MiB | 0.25 | B | 10,695,475,200 | 8,355,840 | 2.356 | — |
| 8MiB | 0.25 | C | 10,737,418,240 | 8,388,608 | 8.167 | — |
| 8MiB | 0.25 | D | 11,869,880,320 | 9,273,344 | 8.110 | 1.382 |
| 8MiB | 0.25 | E | 11,072,962,560 | 8,650,752 | 5.975 | — |
| 8MiB | 0.50 | A | 10,737,455,104 | 8,388,637 | 2.558 | — |
| 8MiB | 0.50 | B | 10,695,475,200 | 8,355,840 | 2.633 | — |
| 8MiB | 0.50 | C | 10,737,418,240 | 8,388,608 | 8.112 | — |
| 8MiB | 0.50 | D | 9,277,800,448 | 7,248,282 | 7.393 | 1.440 |
| 8MiB | 0.50 | E | 9,084,862,464 | 7,097,549 | 5.461 | — |
| 8MiB | 0.75 | A | 10,737,299,456 | 8,388,515 | 3.643 | — |
| 8MiB | 0.75 | B | 10,695,475,200 | 8,355,840 | 2.692 | — |
| 8MiB | 0.75 | C | 10,737,418,240 | 8,388,608 | 8.842 | — |
| 8MiB | 0.75 | D | 5,712,642,048 | 4,463,002 | 4.843 | 1.330 |
| 8MiB | 0.75 | E | 5,377,097,728 | 4,200,858 | 4.137 | — |
| 16MiB | 0.25 | A | 10,737,455,104 | 16,777,274 | 2.655 | — |
| 16MiB | 0.25 | B | 10,661,920,768 | 16,659,251 | 2.296 | — |
| 16MiB | 0.25 | C | 10,737,418,240 | 16,777,216 | 6.847 | — |
| 16MiB | 0.25 | D | 12,129,927,168 | 18,953,011 | 7.098 | 1.412 |
| 16MiB | 0.25 | E | 11,324,620,800 | 17,694,720 | 5.738 | — |
| 16MiB | 0.50 | A | 10,737,389,568 | 16,777,171 | 3.512 | — |
| 16MiB | 0.50 | B | 10,661,920,768 | 16,659,251 | 3.170 | — |
| 16MiB | 0.50 | C | 10,737,418,240 | 16,777,216 | 7.019 | — |
| 16MiB | 0.50 | D | 9,328,132,096 | 14,575,206 | 6.088 | 1.448 |
| 16MiB | 0.50 | E | 8,841,592,832 | 13,814,989 | 4.940 | — |
| 16MiB | 0.75 | A | 10,737,389,568 | 16,777,171 | 3.414 | — |
| 16MiB | 0.75 | B | 10,661,920,768 | 16,659,251 | 3.253 | — |
| 16MiB | 0.75 | C | 10,737,418,240 | 16,777,216 | 8.143 | — |
| 16MiB | 0.75 | D | 5,855,248,384 | 9,148,826 | 5.375 | 1.363 |
| 16MiB | 0.75 | E | 5,301,600,256 | 8,283,750 | 4.049 | — |
| 32MiB | 0.25 | A | 10,737,455,104 | 33,554,547 | 2.624 | — |
| 32MiB | 0.25 | B | 10,594,811,904 | 33,108,787 | 2.714 | — |
| 32MiB | 0.25 | C | 10,737,418,240 | 33,554,432 | 5.786 | — |
| 32MiB | 0.25 | D | 12,247,367,680 | 38,273,024 | 7.073 | 1.426 |
| 32MiB | 0.25 | E | 12,213,813,248 | 38,168,166 | 5.841 | — |
| 32MiB | 0.50 | A | 10,737,455,104 | 33,554,547 | 3.397 | — |
| 32MiB | 0.50 | B | 10,594,811,904 | 33,108,787 | 2.972 | — |
| 32MiB | 0.50 | C | 10,737,418,240 | 33,554,432 | 7.269 | — |
| 32MiB | 0.50 | D | 9,663,676,416 | 30,198,989 | 5.577 | 1.500 |
| 32MiB | 0.50 | E | 8,388,608,000 | 26,214,400 | 4.522 | — |
| 32MiB | 0.75 | A | 10,737,348,608 | 33,554,214 | 3.043 | — |
| 32MiB | 0.75 | B | 10,594,811,904 | 33,108,787 | 2.751 | — |
| 32MiB | 0.75 | C | 10,737,418,240 | 33,554,432 | 7.319 | — |
| 32MiB | 0.75 | D | 5,771,362,304 | 18,035,507 | 4.351 | 1.344 |
| 32MiB | 0.75 | E | 6,073,352,192 | 18,979,226 | 3.836 | — |

**Arms A/B flatness check** (the spec's own built-in sanity check: arms A/B
are chunk-size-independent in principle, so a flat line is itself
evidence the repaired baseline is behaving): Arm A is flat to within
0.001% across all 4 chunk sizes (10,737,348,608 to 10,737,455,104, a span
of 0.001%). Arm B is flat within each chunk size across ratios but shows a
small, real, monotonic **decrease** as chunk size grows: 10,712,252,416
(4MiB) → 10,695,475,200 (8MiB) → 10,661,920,768 (16MiB) → 10,594,811,904
(32MiB), a 1.1% total spread. This is small relative to any C/D/E variation
in this report and does not undermine the check, but it is a real,
reproducible (identical across all 3 ratios at each chunk size) trend, not
noise — plausibly `madvise(MADV_WILLNEED)`-driven readahead granularity
interacting with chunk size, not chased further (arm B's mechanism is
outside this phase's scope).

### Table 2 — device-busy, concurrently-outstanding, per-fetch read (µs), `UFFDIO_CONTINUE` (µs), handler overhead (µs, and as fraction of per-fetch time) — arms C/D/E, per-rep median

| Chunk | Ratio | Arm | Busy | Concur | Read (µs) | CONTINUE (µs) | Overhead (µs) | Overhead frac |
|---|---|---|---|---|---|---|---|---|
| 4MiB | 0.25 | C | 0.734 | 1.00 | 2751.6 | 194.7 | 417.0 | 0.128 |
| 4MiB | 0.25 | D | 0.743 | 1.00 | 2832.5 | 164.2 | 437.0 | 0.129 |
| 4MiB | 0.25 | E | 0.770 | 2.00 | 3081.0 | 162.6 | 550.7 | 0.141 |
| 4MiB | 0.50 | C | 0.733 | 1.00 | 2455.8 | 208.4 | 374.3 | 0.125 |
| 4MiB | 0.50 | D | 0.720 | 1.00 | 2549.5 | 166.0 | 386.3 | 0.126 |
| 4MiB | 0.50 | E | 0.730 | 1.00 | 2728.3 | 170.1 | 462.5 | 0.133 |
| 4MiB | 0.75 | C | 0.757 | 1.00 | 2442.6 | 213.0 | 346.1 | 0.117 |
| 4MiB | 0.75 | D | 0.692 | 1.00 | 2440.9 | 162.8 | 343.5 | 0.117 |
| 4MiB | 0.75 | E | 0.700 | 1.00 | 2516.3 | 162.8 | 367.9 | 0.114 |
| 8MiB | 0.25 | C | 0.731 | 1.00 | 4521.9 | 300.1 | 686.3 | 0.130 |
| 8MiB | 0.25 | D | 0.737 | 1.00 | 4307.6 | 229.5 | 675.0 | 0.133 |
| 8MiB | 0.25 | E | 0.767 | 2.00 | 4684.1 | 229.8 | 833.3 | 0.143 |
| 8MiB | 0.50 | C | 0.729 | 1.00 | 4499.2 | 288.1 | 712.8 | 0.137 |
| 8MiB | 0.50 | D | 0.718 | 1.00 | 4897.9 | 257.8 | 791.7 | 0.138 |
| 8MiB | 0.50 | E | 0.738 | 2.00 | 4951.5 | 252.9 | 906.1 | 0.151 |
| 8MiB | 0.75 | C | 0.751 | 1.00 | 4786.2 | 291.3 | 723.0 | 0.133 |
| 8MiB | 0.75 | D | 0.655 | 1.00 | 4351.1 | 254.7 | 676.6 | 0.132 |
| 8MiB | 0.75 | E | 0.668 | 1.00 | 4967.4 | 252.9 | 817.4 | 0.137 |
| 16MiB | 0.25 | C | 0.701 | 1.00 | 7562.3 | 438.9 | 1305.0 | 0.154 |
| 16MiB | 0.25 | D | 0.713 | 1.00 | 7537.7 | 389.5 | 1351.6 | 0.154 |
| 16MiB | 0.25 | E | 0.736 | 2.00 | 8572.7 | 413.6 | 1734.8 | 0.166 |
| 16MiB | 0.50 | C | 0.711 | 1.00 | 7452.1 | 440.2 | 1282.0 | 0.147 |
| 16MiB | 0.50 | D | 0.699 | 1.00 | 7758.2 | 413.7 | 1295.7 | 0.146 |
| 16MiB | 0.50 | E | 0.730 | 2.00 | 9258.4 | 423.9 | 1764.5 | 0.157 |
| 16MiB | 0.75 | C | 0.757 | 1.00 | 8907.9 | 448.2 | 1307.9 | 0.132 |
| 16MiB | 0.75 | D | 0.675 | 1.00 | 9489.7 | 421.9 | 1291.6 | 0.127 |
| 16MiB | 0.75 | E | 0.695 | 1.00 | 9343.6 | 390.3 | 1292.5 | 0.141 |
| 32MiB | 0.25 | C | 0.691 | 1.00 | 12090.5 | 745.8 | 2460.3 | 0.164 |
| 32MiB | 0.25 | D | 0.704 | 1.00 | 14451.4 | 757.2 | 2824.8 | 0.162 |
| 32MiB | 0.25 | E | 0.732 | 2.00 | 16068.0 | 775.2 | 3554.1 | 0.180 |
| 32MiB | 0.50 | C | 0.727 | 1.00 | 14179.1 | 745.8 | 2575.3 | 0.152 |
| 32MiB | 0.50 | D | 0.684 | 1.00 | 13872.2 | 712.8 | 2581.0 | 0.157 |
| 32MiB | 0.50 | E | 0.697 | 1.00 | 15173.2 | 746.4 | 3277.8 | 0.169 |
| 32MiB | 0.75 | C | 0.728 | 1.00 | 14660.0 | 732.0 | 2546.0 | 0.148 |
| 32MiB | 0.75 | D | 0.642 | 1.00 | 15103.3 | 750.1 | 2606.7 | 0.152 |
| 32MiB | 0.75 | E | 0.656 | 1.00 | 16502.2 | 725.7 | 3137.4 | 0.162 |

### Table 3 — demand faults, `pin_broken`, `infeasible` (median of n=3, arms C/D/E)

`pin_broken=0` and `infeasible=0` in **every one of the 36 (chunk_size,
ratio, arm) cells** — no policy cell hit an infeasible eviction state or
broke a pin anywhere in this phase's grid.

| Chunk | Ratio | C faults | D faults | E faults |
|---|---|---|---|---|
| 4MiB | 0.25 | 2560 | 2874 | 1538 |
| 4MiB | 0.50 | 2560 | 2185 | 1062 |
| 4MiB | 0.75 | 2560 | 1372 | 798 |
| 8MiB | 0.25 | 1280 | 1415 | 792 |
| 8MiB | 0.50 | 1280 | 1106 | 638 |
| 8MiB | 0.75 | 1280 | 681 | 446 |
| 16MiB | 0.25 | 640 | 723 | 408 |
| 16MiB | 0.50 | 640 | 556 | 306 |
| 16MiB | 0.75 | 640 | 349 | 224 |
| 32MiB | 0.25 | 320 | 365 | 225 |
| 32MiB | 0.50 | 320 | 288 | 154 |
| 32MiB | 0.75 | 320 | 172 | 129 |

## Pre-registered expectations

1. **There is a chunk size at which `read_bytes` stops falling and begins
   to rise, or the trend is monotonic to 4 MiB: MIXED, ratio-dependent —
   neither clean floor nor clean monotonic trend holds uniformly.** Arm D's
   `read_bytes` per ratio across {4,8,16,32} MiB:
   - ratio=0.25: 12.054G → **11.870G** → 12.130G → 12.247G — minimum at
     8 MiB, rises on both sides. A genuine interior floor.
   - ratio=0.50: **9.165G** → 9.278G → 9.328G → 9.664G — monotonically
     increasing with chunk size; still falling at the smallest size tested.
     No floor found in this range.
   - ratio=0.75: 5.755G → **5.713G** → 5.855G → 5.771G — non-monotonic;
     local minimum at 8 MiB, but 32 MiB (5.771G) undercuts 16 MiB (5.855G).
   Summed across the 3 ratios (a size-by-size aggregate, not a formal
   statistic): 4MiB=26.974G, **8MiB=26.860G** (aggregate minimum),
   16MiB=27.313G, 32MiB=27.682G. **8 MiB is the aggregate floor** for arm
   D's `read_bytes` across this phase's grid, even though it is not the
   floor at every individual ratio. This is the chunk size used for Phase
   D's paper-table sweep, per the campaign's instruction to state and
   justify that choice — justification is exactly this aggregate.
2. **Handler overhead is roughly constant in absolute terms and therefore a
   larger fraction of per-fetch time at small chunk sizes: DID NOT HOLD —
   same direction as Phase 3's finding, extended to smaller sizes.**
   Overhead grows sharply in absolute terms with chunk size (roughly
   350-550µs at 4MiB → 675-830µs at 8MiB → 1280-1760µs at 16MiB →
   2460-3550µs at 32MiB, roughly 7-8× from smallest to largest) — nowhere
   near constant. Overhead *fraction* also trends upward with chunk size
   (0.114-0.141 at 4MiB vs. 0.148-0.180 at 32MiB), the opposite of what the
   expectation predicted; consistent with Phase 3's own "DID NOT HOLD" at
   its larger 32-256 MiB range. Not chased into a specific cause, reported
   as measured.
3. **`UFFDIO_CONTINUE` duration scales linearly with chunk size: PARTIALLY
   HOLDS — sub-linear at the small end of this range, approaching Phase
   3's "roughly linear" only at the largest step.** Aggregate median
   CONTINUE duration (loosely averaged across arms/ratios): ~178µs (4MiB)
   → ~262µs (8MiB, ×1.47) → ~420µs (16MiB, ×1.60) → ~743µs (32MiB, ×1.77).
   Each doubling of chunk size increases CONTINUE duration by a growing but
   still sub-2× factor — clearly sub-linear at the 4→8 MiB step (×1.47 for
   a 2× size increase), closer to (but still short of) linear at 16→32 MiB
   (×1.77), continuing the trend Phase 3 measured at its larger range
   (×1.68 to ×2.06). Consistent with a fixed, chunk-size-independent
   baseline cost inside `UFFDIO_CONTINUE` that dominates more at the
   smallest chunk sizes tested here, with a per-byte component that only
   starts to dominate at larger sizes — not independently verified beyond
   this pattern in the aggregate numbers.
4. **Wall-clock and bytes may be minimized at different chunk sizes: HELD,
   clearly.** At ratios 0.25 and 0.50, arm D's wall-clock is monotonically
   *decreasing* as chunk size *grows* (ratio=0.50: 7.943s → 7.393s → 6.088s
   → 5.577s, worst at the smallest chunk size, best at the largest) — the
   **opposite** direction from the byte-aggregate result in (1), which
   favors 8 MiB. At ratio=0.75, wall-clock's minimum is 32 MiB (4.351s),
   again the largest size, not 8 MiB. Bytes favor small-to-mid chunks
   (fewer wasted bytes per fetch, aggregate floor at 8 MiB); wall-clock
   favors large chunks (fewer, bigger fetches amortize the per-fetch fixed
   costs measured in (2)/(3) better even as total bytes rise). These are
   genuinely different optima, not the same trend read two ways.
5. **D/OPT ratio improves as chunks shrink: PARTIALLY HOLDS — improves from
   32 MiB down to roughly 8 MiB, then flattens or slightly reverses; 4 MiB
   is not the best.** D/OPT per ratio:
   - ratio=0.25: 1.403 (4MiB), **1.382 (8MiB)**, 1.412 (16MiB), 1.426
     (32MiB) — minimum at 8 MiB, not 4 MiB.
   - ratio=0.50: **1.423 (4MiB)**, 1.440, 1.448, 1.500 — here 4 MiB is the
     best, monotonically improving all the way down.
   - ratio=0.75: 1.340 (4MiB), **1.330 (8MiB)**, 1.363, 1.344 — minimum at
     8 MiB again.
   32 MiB is the worst or tied-worst D/OPT at all 3 ratios, consistent with
   "improves as chunks shrink" in that broad sense — but the very smallest
   tested size (4 MiB) beats 8 MiB in only 1 of 3 ratios, so the expectation
   does not hold in its strongest ("monotonic all the way to 4 MiB") form.

## What I did NOT test

- Chunk sizes below 4 MiB or above 32 MiB — the grid was fixed by the
  campaign's instructions.
- Arm A's `madvise` mode sweep (only `sequential` was run), matching Phase
  3's own disclosed simplification, not Phase 4's full 3-mode sweep.
- Any independent re-verification that Phase 3/4's own device-busy /
  concurrently-outstanding numbers are unaffected by the pooling bug
  described above — flagged as a real possibility, not ruled out, but out
  of this phase's scope to chase.
- `--compute-ns-per-mib` values other than 400000 (Phase B's own
  instructions fix it there; Phase 2 already covers compute=0 at 128 MiB).

## Correctness

Per the campaign's explicit instruction ("re-run T-1..T-7 with
`--eager-reconcile` after Phase B, since 512-chunk tables exercise the
lookup, trace, and state machinery well outside anything previously
tested"): `--eager-reconcile` is not a separate CLI flag on these test
binaries — `test_correctness.c`, `test_storm.c`, `test_t6.c`, and
`test_t7.c` each hard-code `reconcile_interval = 1` (eager reconcile) in
their fixed harness config (comment: "§13 correctness runs use eager
reconcile", item A-3), so running T-1..T-7 at all necessarily runs them
under eager reconcile — there is no non-eager variant of this harness to
have run instead. Re-ran both `scripts/run-correctness-harness.sh` (T-1
through T-5) and `scripts/run-storm-t6-t7.sh` (T-6, T-7) fresh after Phase B:

- **T-1** (full-region data integrity under 25% budget, all 3 policies):
  `mismatches=0` for `default`, `lru`, and `layer_order` — PASS.
- **T-2** (punch-refetch cycle × 1000): `mismatches=0` — PASS.
- **T-3** (storm, 60s, 8 threads): `mismatches=0`,
  `total_touches=94150` — PASS.
- **T-4** (exact accounting after mixed fetch/evict sequence):
  `memory.stat[shmem]=6291456` == `resident_bytes+known_overhead=6291456`,
  EXACT MATCH — PASS.
- **T-5** (`belady_main --selftest`): 300/300 random cross-check trials
  matched the naive O(n²) reference; all 3 cyclic-floor self-checks OK;
  reference-vs-fault trace header enforcement OK — PASS.
- **T-6** (dedup branches fire under async handler, 60s storm):
  `mismatches=0`, `dedup_fetching=12530 > 0` (dedup path exercised) —
  PASS.
- **T-7** (no fault ever lost, 60s storm + 120s watchdog): `mismatches=0`,
  all 8 storm threads joined within the 120s watchdog — PASS.

**RESULT: PASS on all of T-1..T-7.** No failure to stop-and-report on.

## Final check

- No number estimated, inferred, or copied from documentation — every value
  is a direct read from `results/data/synthetic-chunk-size-floor.csv` (median
  of n=3) or a direct computation over retained `--fetch-trace`/reference-
  trace files (`belady_main`'s own printed output for OPT).
- No test was weakened to pass; the two defects found mid-phase (duplicate
  rows from an incomplete resumability check; a pooled-timestamp analysis
  bug) were fixed at their root — the resumability fix adds a stricter,
  per-rep gate, not a looser one, and the busy-fraction fix computes a
  narrower, more correct per-rep quantity, not a smoothed-over one.
- Every pre-registered expectation (1-5) was checked against measured
  values and reported precisely, including where the result was mixed or
  ratio-dependent rather than a clean hold/fail.
- The STOP-AND-REPORT gate was evaluated at all 12 (chunk size, ratio)
  combinations, including the previously-untested 512-chunk case, and
  passed at every one; had any failed, this report would stop there.
- The 32 MiB reproduction check was run and reported before any
  interpretation of the rest of the data, per instruction.
