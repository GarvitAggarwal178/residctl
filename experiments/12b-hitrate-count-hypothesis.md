# Campaign 12 Phase C — Prefetch Hit-Rate Metric Audit

Analysis only, no new runs. Uses `results/data/synthetic-compute-phase-sweep.csv` (arm E,
`--prefetch-depth 2 --prefetch-retention pinned`, the exact configuration
Phase 2's Table 1 used for its D-vs-E byte comparison, n=3) and its
retained `--fetch-trace` binary files, plus `results/data/synthetic-6arm-consolidated-sweep.csv`
and its n=1 supplementary `--fetch-trace` files
(`scratch/phase4supp_E_c{compute}_r{ratio}.fetchtrace`, already disclosed
in `experiments/09b-consolidated-6arm-sweep.md` as a backfill at n=1, not blended into
Phase 4's n=3 medians). Script: `scratch/analyze_phaseC.py`.

## Why this audit

Phase 2 found the prefetch hit rate **fell** with heavier compute in 14 of
18 series, while arm E's `read_bytes` **improved** enough to beat arm D at
r=0.5 and r=0.75 (`experiments/08-compute-phase.md`, expectations 2 and 4). The
hit rate is a post-hoc proxy (a prefetch counts as a hit if no later fetch
event exists for that `chunk_id` before the run ends — a chunk prefetched
twice scores one miss and one hit regardless of whether either fetch was
ever consumed), flagged as an observability workaround since item 10b.
`read_bytes` is directly measured. Both statements can't straightforwardly
describe the same mechanism improving.

**The hypothesis under test:** heavier compute slows the demand cursor, so
`prefetch_pool_top_up` fires less often, so fewer prefetches are issued —
reducing total waste even at a worse per-prefetch hit rate.

## Cross-check: trace-derived prefetch count vs. CSV's own `prefetches` field

Before trusting the `--fetch-trace` binary breakdown, checked it against
the CSV's own accounting, summed across n=3 reps (matching
`experiments/08-compute-phase.md`'s own Table 2 convention for "n prefetches" — verified
directly: `37+36+38=111` reproduces its `0.25/depth=2/pinned/compute=0`
cell exactly). Trace-derived prefetch counts matched the CSV's
`prefetches` sum **exactly** at all 9 (ratio, compute) Phase 2 cells
tested (e.g. 111/111, 124/124, 141/141 at ratio=0.25). The trace files are
trustworthy for this audit.

## Table — Phase 2 (2 GiB region, arm E, depth=2/pinned, totals summed over n=3 reps)

| Ratio | Compute | Prefetches issued | Prefetch bytes | Demand-fetch bytes | Demand faults | Prefetch bytes / total bytes |
|---|---|---|---|---|---|---|
| 0.25 | 0 | 111 | 14,898,167,808 | 23,085,449,216 | 172 | 0.3922 |
| 0.25 | 100000 | 124 | 16,642,998,272 | 23,622,320,128 | 176 | 0.4133 |
| 0.25 | 400000 | **141** | 18,924,699,648 | 32,078,036,992 | 239 | 0.3711 |
| 0.50 | 0 | 78 | 10,468,982,784 | 19,193,135,104 | 143 | 0.3529 |
| 0.50 | 100000 | 82 | 11,005,853,696 | 18,656,264,192 | 139 | 0.3710 |
| 0.50 | 400000 | **90** | 12,079,595,520 | 18,924,699,648 | 141 | 0.3896 |
| 0.75 | 0 | 33 | 4,429,185,024 | 13,690,208,256 | 102 | 0.2444 |
| 0.75 | 100000 | 40 | 5,368,709,120 | 14,227,079,168 | 106 | 0.2740 |
| 0.75 | 400000 | **35** | 4,697,620,480 | 15,166,603,264 | 113 | 0.2365 |

## Table — Phase 4 cross-check (128 MiB chunk, arm E, depth=2/pinned fixed, n=1 supplementary — disclosed as noisier)

| Ratio | Compute | Prefetches issued | Prefetch bytes | Demand-fetch bytes | Demand faults (CSV median, n=3) | Prefetch bytes / total bytes |
|---|---|---|---|---|---|---|
| 0.25 | 0 | 39 | 5,234,491,392 | 6,979,321,856 | 54 | 0.4286 |
| 0.25 | 400000 | 45 | 6,039,797,760 | 10,334,765,056 | 72 | 0.3689 |
| 0.375 | 0 | 37 | 4,966,055,936 | 6,845,104,128 | 50 | 0.4205 |
| 0.375 | 400000 | 33 | 4,429,185,024 | 7,918,845,952 | 61 | 0.3587 |
| 0.50 | 0 | 29 | 3,892,314,112 | 6,710,886,400 | 49 | 0.3671 |
| 0.50 | 400000 | 28 | 3,758,096,384 | 8,187,281,408 | 54 | 0.3146 |
| 0.625 | 0 | 24 | 3,221,225,472 | 5,771,362,304 | 42 | 0.3582 |
| 0.625 | 400000 | 26 | 3,489,660,928 | 4,966,055,936 | 49 | 0.4127 |
| 0.75 | 0 | 13 | 1,744,830,464 | 4,831,838,208 | 33 | 0.2653 |
| 0.75 | 400000 | 13 | 1,744,830,464 | 5,502,926,848 | 36 | 0.2407 |

## Direct test of the hypothesis

**Does prefetch count fall as compute rises? NO — it RISES at every ratio
tested in the primary (n=3) Phase 2 data.** Comparing compute=0 to
compute=400000:

- ratio=0.25: 111 → 141 (**+27.0%**)
- ratio=0.50: 78 → 90 (**+15.4%**)
- ratio=0.75: 33 → 35 (**+6.1%**)

Prefetch count rises monotonically with compute at ratio=0.25 and 0.50
(0→100000→400000 all increasing); at ratio=0.75 it rises then dips slightly
at the highest bracket (33→40→35) but is still higher than compute=0's
baseline. **In no ratio does prefetch count fall from compute=0 to
compute=400000.** The Phase 4 cross-check (n=1, noisier, disclosed as
such) is genuinely mixed — rises at ratio=0.25 and 0.625, falls at 0.375
and 0.5, flat at 0.75 — and does not itself support a falling trend either.

**Does prefetch-bytes-as-a-fraction-of-total fall as compute rises?
MIXED, not a clean fall.** Comparing compute=0 to compute=400000: falls at
ratio=0.25 (0.3922 → 0.3711) and ratio=0.75 (0.2444 → 0.2365), but *rises*
at ratio=0.5 (0.3529 → 0.3896). Two of three ratios move in the predicted
direction, one moves opposite; none of the three moves by more than ~5
percentage points either way.

**The hypothesis is wrong.** Its central, checkable claim —
`prefetch_pool_top_up` fires less often under heavier compute, so fewer
prefetches are issued — is directly contradicted by the primary (n=3)
data: prefetch count rises, sometimes substantially (+27% at ratio=0.25),
as compute increases at every ratio tested. The premise that heavier
compute *slows the demand cursor enough to reduce top-up frequency* does
not hold in this data; if anything, the opposite association appears
(more prefetches, not fewer, under heavier compute). The prefetch-bytes-
fraction check is inconclusive on its own (mixed direction) but is now
moot, since the hypothesis has already failed its first, more direct test.

**Per instruction: the contradiction between Phase 2's falling hit rate
and arm E's improving `read_bytes` at heavier compute remains open. Not
resolved here. No alternative mechanism is proposed.**

## What I did NOT test

- Any depth/retention combination other than depth=2/retention=pinned —
  the one Phase 2's own Table 1 used for its D-vs-E byte comparison and
  the one this audit's hypothesis was raised against. The other 8 of 18
  series in Phase 2's hit-rate table were not re-analyzed for prefetch
  count/byte trends.
- Phase 4's `--compute-ns-per-mib 100000` bracket (Phase 4's grid only
  used {0, 400000}) or its 3 middle ratios' n=3 fetch-trace data (only
  Phase 4's disclosed n=1 supplementary backfill exists at all).
- Any alternative explanation for why hit rate falls while `read_bytes`
  improves — explicitly out of scope per instruction once the
  pre-registered hypothesis failed its direct test.

## Final check

- No number estimated or inferred — every value is a direct sum/read from
  `results/data/synthetic-compute-phase-sweep.csv` and `results/data/synthetic-6arm-consolidated-sweep.csv`, or a
  direct computation over retained `--fetch-trace` binary files
  (`scratch/analyze_phaseC.py`), cross-validated against the CSV's own
  `prefetches` field (exact match at all 9 Phase 2 cells).
- No test was weakened; this phase ran no new tests (analysis only, as
  instructed).
- The pre-registered hypothesis was checked against measured values on
  both of its stated halves (prefetch count, prefetch-byte fraction) and
  found not to hold on the first, more direct one — reported plainly, with
  no alternative mechanism proposed, per instruction.
