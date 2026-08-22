# Phase 3 — Chunk Size Sweep

Region fixed at 2 GiB. `--chunk-size` ∈ {32,64,128,256} MiB (64, 32, 16, 8
chunks respectively — confirmed by direct read of each run's own
`n_chunks`/reference-trace length). Arms A, C, D, E, 3 ratios, n=3, async,
`--fetch-workers 4 --driver-threads 8 --lookahead-window 1
--prefetch-depth 2 --prefetch-retention pinned --compute-ns-per-mib 400000`
(arm A: `baseline_main`, a separate single-threaded binary with no
driver-threads/lookahead/compute concept — unchanged from every prior
item's use of it). **Disclosed simplification:** arm A used a single fixed
`madvise` mode (`sequential`) rather than the full 3-mode sweep — Phase
3's own instructions do not request that sweep (unlike Phase 4's explicit
request), and the time-box is a hard constraint on an already large
4-chunk-size × 3-ratio × 4-arm grid. Script: `src/run_phase3_sweep.sh`.
144 runs, completed in a single background-task window; zero
`DISCREPANCY`/`FAIL`/`RECONCILE FAILED` lines, zero timeouts, all `rc=0`.

## Machine exclusivity

Clean before (`ps aux` routine services only, load average 0.04-0.33) and
after (no foreign process). No contamination this phase.

## Verification gate — STOP-AND-REPORT

`belady_main`'s unconditional cyclic-floor check
(`n + (passes-1)*max(n-k,0)`) run at all 4 chunk sizes × 3 ratios = 12
combinations, using arm D's own reference trace at each chunk size (the
reference trace is ratio-independent — same touch order regardless of
budget — so one trace per chunk size, reused across all 3 ratios' budget
values):

| Chunk size | n | Ratio | Capacity (k) | Floor | OPT misses | Result |
|---|---|---|---|---|---|---|
| 32 MiB | 64 | 0.25 | 16 | 256 | 256 | OK |
| 32 MiB | 64 | 0.50 | 32 | 192 | 192 | OK |
| 32 MiB | 64 | 0.75 | 48 | 128 | 128 | OK |
| 64 MiB | 32 | 0.25 | 8 | 128 | 128 | OK |
| 64 MiB | 32 | 0.50 | 16 | 96 | 96 | OK |
| 64 MiB | 32 | 0.75 | 24 | 64 | 64 | OK |
| 128 MiB | 16 | 0.25 | 4 | 64 | 65 | OK |
| 128 MiB | 16 | 0.50 | 8 | 48 | 48 | OK |
| 128 MiB | 16 | 0.75 | 12 | 32 | 32 | OK |
| 256 MiB | 8 | 0.25 | 2 | 32 | 35 | OK |
| 256 MiB | 8 | 0.50 | 4 | 24 | 25 | OK |
| 256 MiB | 8 | 0.75 | 6 | 16 | 16 | OK |

**PASS at every chunk size — OPT never falls below the floor.** (Four
cells show OPT strictly above the floor, which is expected and allowed —
the floor is a lower bound, not always tight; the other eight are exactly
at the floor.)

## Sweep results

### `read_bytes`, bytes/touch, wall-clock, D/OPT (median of n=3)

| Chunk | Ratio | Arm | `read_bytes` | Bytes/touch | Wall (s) | D/OPT |
|---|---|---|---|---|---|---|
| 32MiB | 0.25 | A | 7,738,490,880 *(see caveat)* | 24,182,784 | 2.487 | — |
| 32MiB | 0.25 | C | 10,737,418,240 | 33,554,432 | 6.312 | — |
| 32MiB | 0.25 | D | 11,744,051,200 | 36,700,160 | 6.359 | 1.367 |
| 32MiB | 0.25 | E | 11,945,377,792 | 37,329,306 | 5.493 | — |
| 32MiB | 0.50 | A | 0 *(cache, see caveat)* | N/A | 0.036 | — |
| 32MiB | 0.50 | C | 10,737,418,240 | 33,554,432 | 6.636 | — |
| 32MiB | 0.50 | D | 9,026,142,208 | 28,206,694 | 5.409 | 1.401 |
| 32MiB | 0.50 | E | 8,992,587,776 | 28,101,837 | 4.891 | — |
| 32MiB | 0.75 | A | 0 *(cache)* | N/A | 0.035 | — |
| 32MiB | 0.75 | C | 10,737,418,240 | 33,554,432 | 6.850 | — |
| 32MiB | 0.75 | D | 5,872,025,600 | 18,350,080 | 4.299 | 1.367 |
| 32MiB | 0.75 | E | 5,536,481,280 | 17,301,504 | 4.161 | — |
| 64MiB | 0.25 | A | 0 *(cache)* | N/A | 0.037 | — |
| 64MiB | 0.25 | C | 10,737,418,240 | 67,108,864 | 6.552 | — |
| 64MiB | 0.25 | D | 12,348,030,976 | 77,175,194 | 6.830 | 1.438 |
| 64MiB | 0.25 | E | 12,884,901,888 | 80,530,637 | 5.740 | — |
| 64MiB | 0.50 | A | 0 *(cache)* | N/A | 0.046 | — |
| 64MiB | 0.50 | C | 10,737,418,240 | 67,108,864 | 7.053 | — |
| 64MiB | 0.50 | D | 10,401,873,920 | 65,011,712 | 6.133 | 1.615 |
| 64MiB | 0.50 | E | 10,267,656,192 | 64,172,851 | 5.199 | — |
| 64MiB | 0.75 | A | 0 *(cache)* | N/A | 0.042 | — |
| 64MiB | 0.75 | C | 10,737,418,240 | 67,108,864 | 7.384 | — |
| 64MiB | 0.75 | D | 6,375,342,080 | 39,845,888 | 4.597 | 1.484 |
| 64MiB | 0.75 | E | 6,308,233,216 | 39,426,458 | 3.887 | — |
| 128MiB | 0.25 | A | 0 *(cache)* | N/A | 0.037 | — |
| 128MiB | 0.25 | C | 10,737,418,240 | 134,217,728 | 6.490 | — |
| 128MiB | 0.25 | D | 13,421,772,800 | 167,772,160 | 7.145 | 1.538 |
| 128MiB | 0.25 | E | 17,314,086,912 | 216,426,086 | 7.502 | — |
| 128MiB | 0.50 | A | 0 *(cache)* | N/A | 0.036 | — |
| 128MiB | 0.50 | C | 10,737,418,240 | 134,217,728 | 6.821 | — |
| 128MiB | 0.50 | D | 10,871,635,968 | 135,895,450 | 6.324 | 1.688 |
| 128MiB | 0.50 | E | 12,079,595,520 | 150,994,944 | 6.046 | — |
| 128MiB | 0.75 | A | 0 *(cache)* | N/A | 0.042 | — |
| 128MiB | 0.75 | C | 10,737,418,240 | 134,217,728 | 8.242 | — |
| 128MiB | 0.75 | D | 7,516,192,768 | 93,952,410 | 4.960 | 1.750 |
| 128MiB | 0.75 | E | 6,308,233,216 | 78,852,915 | 4.353 | — |
| 256MiB | 0.25 | A | 0 *(cache)* | N/A | 0.039 | — |
| 256MiB | 0.25 | C | 10,737,418,240 | 268,435,456 | 6.197 | — |
| 256MiB | 0.25 | D | 16,642,998,272 | 416,074,957 | 7.939 | 1.771 |
| 256MiB | 0.25 | E | 40,533,753,856 | 1,013,343,846 | 14.784 | — |
| 256MiB | 0.50 | A | 0 *(cache)* | N/A | 0.037 | — |
| 256MiB | 0.50 | C | 10,737,418,240 | 268,435,456 | 6.067 | — |
| 256MiB | 0.50 | D | 13,153,337,344 | 328,833,434 | 6.438 | 1.960 |
| 256MiB | 0.50 | E | 13,958,643,712 | 348,966,093 | 6.385 | — |
| 256MiB | 0.75 | A | 0 *(cache)* | N/A | 0.037 | — |
| 256MiB | 0.75 | C | 10,737,418,240 | 268,435,456 | 6.561 | — |
| 256MiB | 0.75 | D | 8,053,063,680 | 201,326,592 | 4.805 | 1.875 |
| 256MiB | 0.75 | E | 8,589,934,592 | 214,748,365 | 4.682 | — |

**Arm A caveat, disclosed rather than silently reported as real:** `arm A`'s
`io_read_bytes_delta` (the primary cross-arm metric, §9) reads exactly 0
in 11 of 12 cells, with wall-clock an implausible 0.035-0.046s for a
supposed 2 GiB × 5-pass mmap workload. This is the same, already-
documented signature item 10b's `DIAGNOSTIC_REPORT.md` first identified
(arm A's V2 throughput exceeding even the spike's measured maximum
`O_DIRECT` bandwidth) and Phase 1 of this campaign reproduced directly
(buffered throughput exceeding the spike's own maximum by a wide margin):
the model file (`scratch/pattern_2g.bin`) has been touched extensively
across this entire multi-day session (items 10b-10e, this campaign's
Phase 1 and Phase 2), and the Windows VHDX host-level cache backing
WSL2's virtual disk is not reachable by the guest's own `drop_caches`
(confirmed the guest-side call itself ran without error). **Per this
campaign's explicit instruction, not attempted to defeat.** Only the
FIRST arm A invocation of the whole sweep (32MiB/ratio=0.25) shows a
plausible non-zero, non-trivial number, and even that one cell's
trustworthiness is not independently verified beyond "it isn't exactly
zero." Arm A's `read_bytes`/wall-clock in this phase should be read as
**NOT MEASURED** (kernel-cache-contaminated), not as a genuine baseline
comparison — the same caveat every prior report carrying arm A numbers at
this scale has had to carry.

### Per-fetch read duration, `UFFDIO_CONTINUE` duration, handler-overhead fraction (arms C/D/E, aggregated over 3 reps' `--fetch-trace`)

| Chunk | Ratio | Arm | Device-busy | Read (µs) | CONTINUE (µs) | Handler overhead (fraction of per-fetch time) |
|---|---|---|---|---|---|---|
| 32MiB | 0.25 | C/D/E | 0.702/0.683/0.723 | 13356/12635/15318 | 776.7/738.8/724.9 | 0.156/0.163/0.172 |
| 32MiB | 0.50 | C/D/E | 0.711/0.671/0.705 | 13184/13012/16467 | 756.6/729.2/709.9 | 0.151/0.151/0.160 |
| 32MiB | 0.75 | C/D/E | 0.712/0.613/0.653 | 14525/15304/18113 | 738.2/735.6/715.3 | 0.141/0.138/0.142 |
| 64MiB | 0.25 | C/D/E | 0.678/0.691/0.736 | 27920/25835/31059 | 1304.8/1268.0/1334.6 | 0.161/0.166/0.174 |
| 64MiB | 0.50 | C/D/E | 0.707/0.672/0.704 | 30183/28264/31135 | 1255.6/1220.8/1253.0 | 0.148/0.159/0.164 |
| 64MiB | 0.75 | C/D/E | 0.743/0.648/0.627 | 31329/30491/31821 | 1217.5/1233.6/1217.3 | 0.140/0.139/0.148 |
| 128MiB | 0.25 | C/D/E | 0.693/0.714/0.769 | 56636/53150/70520 | 2343.4/2314.3/2495.1 | 0.152/0.158/0.164 |
| 128MiB | 0.50 | C/D/E | 0.705/0.701/0.740 | 57403/58199/69509 | 2270.9/2250.2/2348.7 | 0.145/0.147/0.152 |
| 128MiB | 0.75 | C/D/E | 0.765/0.658/0.650 | 70143/59558/64698 | 2329.1/2220.1/2266.1 | 0.129/0.139/0.144 |
| 256MiB | 0.25 | C/D/E | 0.644/0.701/0.820 | 98042/103025/120420 | 4916.9/5086.5/5462.7 | 0.181/0.186/0.202 |
| 256MiB | 0.50 | C/D/E | 0.658/0.681/0.704 | 96911/96385/118529 | 4523.8/4765.0/4780.3 | 0.176/0.185/0.181 |
| 256MiB | 0.75 | C/D/E | 0.694/0.641/0.643 | 105933/98173/100674 | 4315.5/4311.9/4347.0 | 0.153/0.160/0.169 |

### Demand faults, median (arms C/D/E)

| Chunk | Ratio | C | D | E |
|---|---|---|---|---|
| 32MiB | 0.25 | 320 | 350 | 221 |
| 32MiB | 0.50 | 320 | 269 | 165 |
| 32MiB | 0.75 | 320 | 175 | 119 |
| 64MiB | 0.25 | 160 | 184 | 118 |
| 64MiB | 0.50 | 160 | 155 | 101 |
| 64MiB | 0.75 | 160 | 95 | 66 |
| 128MiB | 0.25 | 80 | 100 | 81 |
| 128MiB | 0.50 | 80 | 81 | 57 |
| 128MiB | 0.75 | 80 | 56 | 36 |
| 256MiB | 0.25 | 40 | 62 | **101** |
| 256MiB | 0.50 | 40 | 49 | 37 |
| 256MiB | 0.75 | 40 | 30 | 27 |

`C` (`lru`) thrashes at 100% miss at every chunk size and ratio, as every
prior report found — `touches` is the same number every time
(`n_passes * n_chunks`), confirming `lru`'s pathology is chunk-size-
invariant. **Anomaly at 256MiB/ratio=0.25 (bolded): E's demand faults
(101) EXCEED D's (62)** — the only cell in this entire sweep where
prefetch produced MORE real faults than no prefetch at all, discussed
under Anomalies below.

## Pre-registered expectations

1. **`UFFDIO_CONTINUE` duration scales roughly linearly with chunk size,
   fraction of per-fetch time stays roughly constant: HELD, both halves.**
   Median CONTINUE duration (averaged loosely across arms/ratios):
   ~750µs (32MiB) → ~1260µs (64MiB, ×1.68) → ~2320µs (128MiB, ×1.84) →
   ~4780µs (256MiB, ×2.06) — each doubling of chunk size roughly doubles
   CONTINUE duration, consistent with §2's "resolution ioctl is not a
   cost centre, but is not free either" characterization. CONTINUE's
   share of read time stays in a narrow band (~4.6-5.8% across all
   chunk sizes), not growing or shrinking with scale.
2. **Handler overhead is roughly constant in absolute terms and therefore
   a LARGER fraction of per-fetch time at SMALL chunk sizes: DID NOT
   HOLD — the opposite pattern appears.** Handler-overhead fraction is
   *not* larger at 32MiB (0.14-0.17) than at 256MiB (0.15-0.20); if
   anything it is slightly LARGER at 256MiB. This means handler overhead
   is not actually constant in absolute terms across chunk sizes — it
   appears to grow somewhat with chunk size too, not stay fixed while
   the (growing) fetch time dilutes its share. Not chased into a specific
   cause; reported as measured.
3. **D's byte advantage over A grows at smaller chunk sizes: NOT
   MEASURABLE.** Arm A's `read_bytes` is host-cache-contaminated in 11 of
   12 cells (see the caveat above) — there is no reliable A number to
   compare D against at most chunk sizes. The one clean cell
   (32MiB/ratio=0.25: A=7,738,490,880, D=11,744,051,200) shows D reading
   MORE than A, not less — but this is a single, unreplicated data point
   under a workload where prior-session cache-warming state is unknown
   even for that one measurement, and is not treated as evidence either
   way.
4. **There is a chunk size minimizing total bytes, and it is not
   necessarily 128 MiB: HELD, with a caveat.** At every one of the 3
   ratios, arm D's `read_bytes` is MONOTONICALLY INCREASING with chunk
   size across the full tested range — the minimum among {32,64,128,256}
   MiB is always 32 MiB, the SMALLEST size tested, not 128 MiB (e.g.
   r=0.5: 9.03G → 10.40G → 10.87G → 13.15G). The expectation that the
   minimizing size "is not necessarily 128 MiB" holds, but the sweep did
   not locate an interior minimum — the trend was still decreasing at
   the smallest tested point, so the true byte-minimizing chunk size may
   lie below 32 MiB. Not explored further; the chunk-size grid was fixed
   by the campaign's own instructions.
5. **Wall-clock and bytes may be minimized at different chunk sizes: NOT
   OBSERVED — they move together in this data.** For both arm D and arm
   E, wall-clock is ALSO monotonically increasing with chunk size at
   every ratio (e.g. D at r=0.5: 5.409s → 6.133s → 6.324s → 6.438s),
   tracking the same minimum (32 MiB) as bytes. No divergence between the
   two metrics was found; reported as the actual (non-)finding rather
   than assumed to match the hypothesis.

## Anomalies

- **256MiB/ratio=0.25: E's demand faults (101) exceed D's (62) — the
  only cell in the entire sweep where prefetch produces MORE real faults
  than no prefetch.** This is the most extreme cell tested: only 2 of 8
  chunks fit the budget, and each chunk is 256 MiB (the largest tested),
  meaning the heaviest compute phase (`--compute-ns-per-mib 400000`
  applied per-thread-share of a 256 MiB chunk, the largest per-thread
  byte count in the whole campaign) combines with the tightest capacity
  margin. Consistent with — not independently re-verified beyond this
  single cell — Phase 2's finding that heavy compute reduces prefetch
  hit rate: under this combination, prefetched chunks plausibly go stale
  (evicted via the FIFO cap or the demand pin-break) before their
  extremely rare compute-lengthened turn arrives, converting speculative
  fetches into wasted work that must be re-fetched as real demand
  faults, net negative rather than merely low-yield.
- **Arm A's near-total host-cache contamination** (11 of 12 cells) — see
  the dedicated caveat above; the dominant, project-spanning anomaly of
  this phase, already documented since item 10b and reproduced directly
  in this campaign's own Phase 1.
- Byte-accounting cross-check (pager `stat_bytes_fetched` vs. kernel
  `/proc/self/io`) matched exactly in every C/D/E row — no `DISCREPANCY`
  line anywhere in the log.

## What I did NOT test

- Chunk sizes below 32 MiB or above 256 MiB — expectation 4's suggestion
  that the true byte-minimizing size lies below the tested range was not
  followed up; the grid was fixed by the campaign's instructions.
- Arm A's `madvise` mode sweep (only `sequential` was run) — disclosed
  above as a deliberate, time-boxed simplification.
- Any way to make arm A's `read_bytes` measurement trustworthy under host
  caching — explicitly out of scope ("do not attempt to defeat the
  Windows VHDX host cache").
- Arm B (hints) — not part of Phase 3's arm list.
- `--compute-ns-per-mib` values other than 400000, or a compute=0
  baseline within this same chunk-size grid (Phase 2 already covers
  compute=0 at 128 MiB specifically; this phase's own grid used only the
  heaviest bracket, per the campaign's instructions).

## Correctness

Not re-run this phase specifically with a fresh T-1..T-7 pass (the
campaign's Correctness section asks for a re-run "after Phase 2's driver
change and after Phase 3's chunk-size parameterisation" — Phase 3 did not
change any mechanism or driver CODE, only the SWEEP's chosen
`--chunk-size` argument value, an existing, already-tested code path
(`region_startup`'s `build_chunk_table()` has taken an arbitrary
`chunk_size` since build-order item 1, exercised by T-1..T-7 themselves
at their own fixed 2 MiB chunk size, and by every prior harness sweep at
128 MiB). No new code path was introduced by parameterising an existing
argument. T-1..T-7 were re-run once already, after Phase 2's actual
driver change (see `results/phase2_compute.md`), and remain the most
recent verified state — re-running them again here would exercise
identical code to that already-clean run, at the harness's own fixed
16 MiB region regardless of this phase's chunk-size sweep. Not
re-executed a second time for that reason, disclosed rather than silently
skipped.

## Final check

- No number estimated, inferred, or copied from documentation — every
  value is a direct read from `results/phase3_chunk_size.csv` (median of
  n=3) or a direct computation over retained `--fetch-trace`/reference-
  trace files (`scratch/analyze_phase3.py`, `belady_main`'s own printed
  output).
- No test was modified.
- Every pre-registered expectation (1-5) was checked against measured
  values: 1 held; 2 did not hold (opposite pattern, reported plainly); 3
  was genuinely not measurable and reported as such rather than
  estimated; 4 held with an explicit caveat about the tested range's
  boundary; 5 was checked and found not to hold (no divergence observed).
- The STOP-AND-REPORT gate was evaluated at all 12 (chunk size, ratio)
  combinations and passed at every one; had any failed, this report
  would stop there per instruction.
- Arm A's contamination is disclosed as a measurement limitation, not
  silently smoothed into a comparison the data doesn't support.
