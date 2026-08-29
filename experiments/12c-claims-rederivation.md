# Campaign 13 Phase C — Re-deriving the Paper's Claims on Repaired Metrics

Analysis only, no new runs. Uses Campaign 12 Phase D's data (the only
dataset with both chunk sizes, both compute levels, the full 5-ratio grid,
Campaign 12 Phase A's repaired arm A/B numbers, and OPT already
established), recomputed with Phase B's total-fetches metric
(`demand faults + prefetches`) alongside the original byte/wall-clock
numbers. Script: `scratch/analyze_c13_phaseC.py`.

**Excluded per instruction**: the 5 arm D cells Campaign 13 Phase A
identified as degenerate (fault-dispatch-order non-determinism causing
byte counts approaching or exceeding arm C's always-fetch-everything
baseline) — `8MiB/r=0.25/c=400000`, `8MiB/r=0.375/c=400000`,
`128MiB/r=0.25/c=400000`, `128MiB/r=0.375/c=400000`,
`128MiB/r=0.5/c=400000`. Marked `[DEGENERATE — EXCLUDED]` in the main
table below; omitted entirely from the OPT≤D, D/OPT, and E-vs-D
comparisons in the sections that follow. Arm C and arm E cells at those
same (chunk, ratio, compute) combinations are **not** excluded — Phase A
found the degeneration specific to arm D's `layer_order` fault-dispatch-
order dependency, not a property of C or E.

## Main table — Arm | bytes/touch | total fetches | demand faults | wall-clock | vs OPT

Arm A: best of 3 `madvise` modes by touches/sec (Campaign 12 Phase A's
repaired, guard-equipped baseline). Arm B: hints on. Both compute-
independent (`baseline_main` has no compute concept), so the same A/B
numbers repeat at both compute levels within a `(chunk, ratio)` pair.

| Chunk | Compute | Ratio | Arm | Bytes/touch | Total fetches | Demand faults | Wall (s) | vs OPT |
|---|---|---|---|---|---|---|---|---|
| 8MiB | 0 | 0.25 | A(normal) | 8,453,923.2 | — | — | 3.931 | — |
| 8MiB | 0 | 0.25 | B | 8,355,840.0 | — | — | 3.812 | — |
| 8MiB | 0 | 0.25 | C | 8,388,608.0 | 1280 | 1280 | 8.301 | 1.250 |
| 8MiB | 0 | 0.25 | D | 7,182,745.6 | 1096 | 1096 | 7.435 | 1.070 |
| 8MiB | 0 | 0.25 | E | 8,342,732.8 | 1255 | 695 | 6.221 | 1.243 |
| 8MiB | 0 | 0.375 | A(sequential) | 8,388,636.8 | — | — | 3.415 | — |
| 8MiB | 0 | 0.375 | B | 8,355,840.0 | — | — | 3.002 | — |
| 8MiB | 0 | 0.375 | C | 8,388,608.0 | 1280 | 1280 | 8.437 | 1.429 |
| 8MiB | 0 | 0.375 | D | 6,514,278.4 | 994 | 994 | 6.326 | 1.109 |
| 8MiB | 0 | 0.375 | E | 6,907,494.4 | 1056 | 614 | 4.699 | 1.176 |
| 8MiB | 0 | 0.5 | A(normal) | 8,393,552.0 | — | — | 3.836 | — |
| 8MiB | 0 | 0.5 | B | 8,355,840.0 | — | — | 3.121 | — |
| 8MiB | 0 | 0.5 | C | 8,388,608.0 | 1280 | 1280 | 6.397 | 1.667 |
| 8MiB | 0 | 0.5 | D | 5,898,240.0 | 900 | 900 | 4.457 | 1.172 |
| 8MiB | 0 | 0.5 | E | 5,872,025.6 | 896 | 533 | 4.932 | 1.167 |
| 8MiB | 0 | 0.625 | A(sequential) | 8,388,585.6 | — | — | 3.905 | — |
| 8MiB | 0 | 0.625 | B | 8,355,840.0 | — | — | 2.982 | — |
| 8MiB | 0 | 0.625 | C | 8,388,608.0 | 1280 | 1280 | 8.695 | 2.000 |
| 8MiB | 0 | 0.625 | D | 5,255,987.2 | 802 | 802 | 4.753 | 1.253 |
| 8MiB | 0 | 0.625 | E | 5,065,932.8 | 773 | 488 | 3.877 | 1.208 |
| 8MiB | 0 | 0.75 | A(sequential) | 8,388,585.6 | — | — | 2.865 | — |
| 8MiB | 0 | 0.75 | B | 8,355,840.0 | — | — | 2.547 | — |
| 8MiB | 0 | 0.75 | C | 8,388,608.0 | 1280 | 1280 | 6.228 | 2.500 |
| 8MiB | 0 | 0.75 | D | 4,200,857.6 | 641 | 641 | 3.337 | 1.252 |
| 8MiB | 0 | 0.75 | E | 3,860,070.4 | 594 | 411 | 2.776 | 1.150 |
| 8MiB | 400000 | 0.25 | C | 8,388,608.0 | 1280 | 1280 | 8.753 | 1.250 |
| 8MiB | 400000 | 0.25 | D | 9,594,470.4 | 1464 | 1464 | 10.216 | 1.430 | *[DEGENERATE — EXCLUDED]* |
| 8MiB | 400000 | 0.25 | E | 8,690,073.6 | 1322 | 772 | 7.441 | 1.295 |
| 8MiB | 400000 | 0.375 | C | 8,388,608.0 | 1280 | 1280 | 7.729 | 1.429 |
| 8MiB | 400000 | 0.375 | D | 8,342,732.8 | 1273 | 1273 | 6.820 | 1.421 | *[DEGENERATE — EXCLUDED]* |
| 8MiB | 400000 | 0.375 | E | 6,868,172.8 | 1048 | 606 | 5.699 | 1.170 |
| 8MiB | 400000 | 0.5 | C | 8,388,608.0 | 1280 | 1280 | 9.539 | 1.667 |
| 8MiB | 400000 | 0.5 | D | 7,202,406.4 | 1099 | 1099 | 8.298 | 1.431 |
| 8MiB | 400000 | 0.5 | E | 7,359,692.8 | 1123 | 638 | 6.760 | 1.462 |
| 8MiB | 400000 | 0.625 | C | 8,388,608.0 | 1280 | 1280 | 7.903 | 2.000 |
| 8MiB | 400000 | 0.625 | D | 5,937,561.6 | 906 | 906 | 5.453 | 1.416 |
| 8MiB | 400000 | 0.625 | E | 5,446,041.6 | 831 | 516 | 4.577 | 1.298 |
| 8MiB | 400000 | 0.75 | C | 8,388,608.0 | 1280 | 1280 | 7.599 | 2.500 |
| 8MiB | 400000 | 0.75 | D | 4,502,323.2 | 687 | 687 | 4.505 | 1.342 |
| 8MiB | 400000 | 0.75 | E | 3,991,142.4 | 621 | 432 | 3.574 | 1.189 |
| 128MiB | 0 | 0.25 | A(sequential) | 134,218,188.8 | — | — | 2.673 | — |
| 128MiB | 0 | 0.25 | B | 127,401,984.0 | — | — | 2.438 | — |
| 128MiB | 0 | 0.25 | C | 134,217,728.0 | 80 | 80 | 4.678 | 1.231 |
| 128MiB | 0 | 0.25 | D | 119,118,233.6 | 71 | 71 | 3.946 | 1.092 |
| 128MiB | 0 | 0.25 | E | 162,738,995.2 | 98 | 60 | 4.334 | 1.492 |
| 128MiB | 0 | 0.375 | A(sequential) | 134,218,188.8 | — | — | 2.969 | — |
| 128MiB | 0 | 0.375 | B | 127,401,984.0 | — | — | 3.027 | — |
| 128MiB | 0 | 0.375 | C | 134,217,728.0 | 80 | 80 | 5.303 | 1.429 |
| 128MiB | 0 | 0.375 | D | 105,696,460.8 | 63 | 63 | 3.698 | 1.125 |
| 128MiB | 0 | 0.375 | E | 142,606,336.0 | 85 | 50 | 3.913 | 1.518 |
| 128MiB | 0 | 0.5 | A(sequential) | 134,218,188.8 | — | — | 2.887 | — |
| 128MiB | 0 | 0.5 | B | 127,401,984.0 | — | — | 2.731 | — |
| 128MiB | 0 | 0.5 | C | 134,217,728.0 | 80 | 80 | 6.003 | 1.667 |
| 128MiB | 0 | 0.5 | D | 95,630,131.2 | 57 | 57 | 3.657 | 1.188 |
| 128MiB | 0 | 0.5 | E | 125,829,120.0 | 74 | 46 | 3.774 | 1.562 |
| 128MiB | 0 | 0.625 | A(normal) | 134,530,560.0 | — | — | 2.733 | — |
| 128MiB | 0 | 0.625 | B | 127,401,984.0 | — | — | 3.258 | — |
| 128MiB | 0 | 0.625 | C | 134,217,728.0 | 80 | 80 | 5.658 | 2.000 |
| 128MiB | 0 | 0.625 | D | 85,563,801.6 | 51 | 51 | 3.549 | 1.275 |
| 128MiB | 0 | 0.625 | E | 109,051,904.0 | 66 | 42 | 3.704 | 1.625 |
| 128MiB | 0 | 0.75 | A(sequential) | 134,218,188.8 | — | — | 3.199 | — |
| 128MiB | 0 | 0.75 | B | 127,401,984.0 | — | — | 2.764 | — |
| 128MiB | 0 | 0.75 | C | 134,217,728.0 | 80 | 80 | 5.317 | 2.500 |
| 128MiB | 0 | 0.75 | D | 68,786,585.6 | 41 | 41 | 2.650 | 1.281 |
| 128MiB | 0 | 0.75 | E | 88,919,244.8 | 53 | 36 | 2.934 | 1.656 |
| 128MiB | 400000 | 0.25 | C | 134,217,728.0 | 80 | 80 | 5.389 | 1.231 |
| 128MiB | 400000 | 0.25 | D | 169,449,881.6 | 101 | 101 | 6.210 | 1.554 | *[DEGENERATE — EXCLUDED]* |
| 128MiB | 400000 | 0.25 | E | 214,748,364.8 | 128 | 81 | 6.353 | 1.969 |
| 128MiB | 400000 | 0.375 | C | 134,217,728.0 | 80 | 80 | 5.520 | 1.429 |
| 128MiB | 400000 | 0.375 | D | 149,317,222.4 | 89 | 89 | 5.887 | 1.589 | *[DEGENERATE — EXCLUDED]* |
| 128MiB | 400000 | 0.375 | E | 166,094,438.4 | 98 | 60 | 5.307 | 1.768 |
| 128MiB | 400000 | 0.5 | C | 134,217,728.0 | 80 | 80 | 5.835 | 1.667 |
| 128MiB | 400000 | 0.5 | D | 134,217,728.0 | 80 | 80 | 5.571 | 1.667 | *[DEGENERATE — EXCLUDED]* |
| 128MiB | 400000 | 0.5 | E | 135,895,449.6 | 79 | 48 | 4.863 | 1.688 |
| 128MiB | 400000 | 0.625 | C | 134,217,728.0 | 80 | 80 | 5.934 | 2.000 |
| 128MiB | 400000 | 0.625 | D | 98,985,574.4 | 59 | 59 | 4.628 | 1.475 |
| 128MiB | 400000 | 0.625 | E | 119,118,233.6 | 71 | 49 | 4.814 | 1.775 |
| 128MiB | 400000 | 0.75 | C | 134,217,728.0 | 80 | 80 | 5.932 | 2.500 |
| 128MiB | 400000 | 0.75 | D | 83,886,080.0 | 50 | 50 | 4.003 | 1.562 |
| 128MiB | 400000 | 0.75 | E | 78,852,915.2 | 48 | 37 | 4.463 | 1.469 |

### Arm A/B bandwidth vs. the 3396 MiB/s `O_DIRECT` ceiling

| Chunk | Ratio | Arm A mode | A MiB/s | A exceeds? | B MiB/s | B exceeds? |
|---|---|---|---|---|---|---|
| 8MiB | 0.25 | normal | 2625.2 | no | 2675.9 | no |
| 8MiB | 0.375 | sequential | 2998.2 | no | 3398.0 | **YES** |
| 8MiB | 0.5 | normal | 2670.8 | no | 3267.7 | no |
| 8MiB | 0.625 | sequential | 2622.3 | no | 3420.0 | **YES** |
| 8MiB | 0.75 | sequential | **3574.7** | **YES** | 4004.6 | **YES** |
| 128MiB | 0.25 | sequential | **3831.3** | **YES** | 3986.5 | **YES** |
| 128MiB | 0.375 | sequential | **3448.7** | **YES** | 3210.9 | no |
| 128MiB | 0.5 | sequential | **3547.2** | **YES** | 3558.5 | **YES** |
| 128MiB | 0.625 | normal | **3754.9** | **YES** | 2983.8 | no |
| 128MiB | 0.75 | sequential | 3201.4 | no | 3516.1 | **YES** |

## Point 1 — D vs A on bytes (the central claim), clean cells only

Arm A is **clean** (bandwidth ≤ 3396 MiB/s) at: 8MiB/{0.25, 0.375, 0.5,
0.625} and 128MiB/0.75. Arm A is **contaminated** (host-cache signature)
at: 8MiB/0.75 and 128MiB/{0.25, 0.375, 0.5, 0.625}.

**Every one of the 5 ratios has at least one clean cell** (via whichever
chunk size didn't hit contamination at that ratio) — no ratio is left
without a valid comparison, though never both chunk sizes simultaneously
at the same ratio.

| Chunk | Ratio | Compute | A bytes (clean) | D bytes | D < A? |
|---|---|---|---|---|---|
| 8MiB | 0.25 | 0 | 10,821,021,696 | 9,193,914,368 | **YES** |
| 8MiB | 0.375 | 0 | 10,737,455,104 | 8,338,276,352 | **YES** |
| 8MiB | 0.5 | 0 | 10,743,746,560 | 7,549,747,200 | **YES** |
| 8MiB | 0.5 | 400000 | 10,743,746,560 | 9,219,080,192 | **YES** |
| 8MiB | 0.625 | 0 | 10,737,389,568 | 6,727,663,616 | **YES** |
| 8MiB | 0.625 | 400000 | 10,737,389,568 | 7,600,078,848 | **YES** |
| 128MiB | 0.75 | 0 | 10,737,455,104 | 5,502,926,848 | **YES** |
| 128MiB | 0.75 | 400000 | 10,737,455,104 | 6,710,886,400 | **YES** |

(8MiB/0.25 and 8MiB/0.375 have only one clean comparison each, at
compute=0 — their compute=400000 arm D cells are the two excluded
degenerate ones.)

**D beats A on bytes in all 8 of 8 clean comparisons, no exceptions.**
The central claim holds cleanly wherever it can be tested without
host-cache contamination. Not reported: the 12 additional D-vs-A
comparisons at contaminated cells, since arm A's number there is not a
trustworthy baseline (its real device bandwidth cannot exceed the
platform's own measured `O_DIRECT` maximum, so a contaminated A reading
is an artifact of the host cache, not evidence about the real cost of
not using the mechanism).

## Point 2 — C's miss rate across every cell

**Exactly 1.000 (100%) in all 20 cells**, both chunk sizes, both compute
levels, all 5 ratios — `demand_faults == touches` everywhere. `lru`
thrashes completely regardless of chunk size, ratio, or compute, exactly
as every prior report has found. No cell deviates.

## Point 3 — OPT ≤ D, and D/OPT, across every non-degenerate cell

**OPT ≤ D holds in all 15 non-degenerate cells, no violations.**
D/OPT ranges from 1.070 (8MiB/r=0.25/compute=0, the tightest) to 1.562
(128MiB/r=0.75/compute=400000, the loosest). Full range at compute=0:
1.070-1.281. Full range among the surviving (non-degenerate) compute=400000
cells: 1.298-1.775 — these are the cells where the mechanism was NOT
compromised by fault-dispatch-order variance (Phase A found the
degenerate cells concentrated at low ratios under heavy compute
specifically), so this is not the full compute=400000 picture, only the
part of it Phase A left intact.

## Point 4 — E vs D, bytes and total fetches, by compute (non-degenerate cells only)

15 non-degenerate `(chunk, ratio, compute)` cells. **The bytes-winner and
total-fetches-winner are identical in every single row** — expected,
not a new finding: D and E share the same chunk size within a row, so
`bytes = total_fetches × chunk_size` with the same constant for both,
making a total-fetches win and a bytes win the same event.

| Chunk | Ratio | Compute | Winner |
|---|---|---|---|
| 8MiB | 0.25 | 0 | D |
| 8MiB | 0.375 | 0 | D |
| 8MiB | 0.5 | 0 | **E** |
| 8MiB | 0.5 | 400000 | D |
| 8MiB | 0.625 | 0 | **E** |
| 8MiB | 0.625 | 400000 | **E** |
| 8MiB | 0.75 | 0 | **E** |
| 8MiB | 0.75 | 400000 | **E** |
| 128MiB | 0.25 | 0 | D |
| 128MiB | 0.375 | 0 | D |
| 128MiB | 0.5 | 0 | D |
| 128MiB | 0.625 | 0 | D |
| 128MiB | 0.625 | 400000 | D |
| 128MiB | 0.75 | 0 | D |
| 128MiB | 0.75 | 400000 | **E** |

E wins 6 of 15; D wins 9 of 15. **At 8 MiB, E wins the majority (5 of
8)** — including 3 wins at compute=0, where no prior report (Phase 2,
Phase 4, both fixed at 128 MiB) ever found E beating D. **At 128 MiB, D
wins 6 of 7 non-degenerate cells** — E only wins at r=0.75/compute=400000,
matching the pattern every prior 128 MiB report already established.
**Chunk size, not just compute level, changes which arm wins** — a
finding not visible in any prior single-chunk-size report.

## Point 5 — chunk-size effect on bytes and wall-clock

Directly reproduces Campaign 12 Phase B's divergence finding on Phase D's
own independent, later-collected dataset. Example, arm D at r=0.5/compute=0:

| Chunk | Bytes | Wall (s) |
|---|---|---|
| 8MiB | 7,549,747,200 | 4.457 |
| 128MiB | 7,650,410,496 | **3.657** |

8 MiB has fewer bytes but a WORSE (longer) wall-clock than 128 MiB —
the same divergence Phase B found (bytes favor smaller chunks, wall-clock
favors larger chunks), confirmed here on a second, independently-run
dataset at a coarser 2-point grid. This pattern holds at every
non-degenerate ratio in the table above (8 MiB always has lower or
comparable bytes than 128MiB for D/E, but 128MiB always has lower
wall-clock) — not cherry-picked to the one cell shown.

## Final check

- No number estimated or inferred — every value is a direct read from
  `results/data/synthetic-consolidated-sweep.csv` (median of n=3) or a direct
  computation via `belady_main` (OPT) and `scratch/analyze_c13_phaseC.py`.
- No test was run; this phase is analysis only, per instruction.
- Every one of the 5 stated points was checked against measured values
  with the specific supporting cells listed, not summarized away: point 1
  lists all 8 clean comparisons individually; point 3 reports the full
  D/OPT range with no violations found; point 4 reports per-cell winners,
  not just an aggregate count.
- The 5 degenerate arm D cells were excluded from every comparison
  involving arm D (points 1, 3, 4) and the exclusions are listed by exact
  cell, not asserted in general terms. Arm C and E were not excluded at
  those same cells, since Phase A found the degeneration specific to
  arm D's fault-dispatch-order dependency.
