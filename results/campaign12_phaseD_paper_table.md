# Campaign 12 Phase D — The Paper Table

**Chunk size chosen: 8 MiB**, per instruction ("use the size Phase B
measured as best on `read_bytes` for arm D"). Phase B's own report
(`results/campaign12_phaseB_chunk_floor.md`, expectation 1) found arm D's
`read_bytes` floor is ratio-dependent — not a single clean minimum, and
not monotonic to 4 MiB either (4 MiB loses to 8 MiB in 2 of 3 ratios
tested) — so the fallback clause ("if Phase B's trend is monotonic to
4 MiB, use 4 MiB") does not apply. Phase B's own resolution was the
**aggregate** minimum across its 3 tested ratios: summed `read_bytes`
across {0.25, 0.5, 0.75} was 26.974G (4MiB), **26.860G (8MiB, lowest)**,
27.313G (16MiB), 27.682G (32MiB). 8 MiB is used here as that aggregate
floor. Also run at **128 MiB** (fixed) for comparability with every prior
report (V2, item 10-10e, Campaign 11).

Fixed: 2 GiB region, async handler, `--fetch-workers 4 --driver-threads 8
--lookahead-window 1 --prefetch-depth 2 --prefetch-retention pinned`.
Varied: arms {A, B, C, D, E} × 5 ratios {0.25, 0.375, 0.5, 0.625, 0.75} ×
`--compute-ns-per-mib` {0, 400000} × n=3, at both chunk sizes (OPT computed
offline via `belady_main` from arm D's own reference trace at each
(chunk_size, ratio), not compute-dependent). Arm A: full 3-mode `madvise`
sweep, best reported. Arm A/B: compute=n/a, run once per ratio (no compute
concept in `baseline_main`), `drop_caches` before every invocation using
Phase A's corrected pipe pattern, guard-equipped `baseline_main` active.
`--fetch-trace` enabled on every C/D/E pager run (Phase 4 omitted this and
had to backfill at n=1 — not repeated here). Script:
`src/run_phaseD_sweep.sh` (main grid, excludes arm A's `random` mode from
its automatic loop — see Process notes) + `src/run_phaseD_random_rep.sh`
(dedicated single-rep helper for `random` mode). 300 rows (2 chunk sizes ×
5 arms-worth-of-cells × ... — precisely: 2 × [3 modes × 5 ratios + 5
ratios (arm B) + 3 arms × 5 ratios × 2 computes] × n=3 = 300). Zero
`BASELINE FAILED`/`DISCREPANCY`/`RECONCILE FAILED` lines, zero non-`rc=0`
rows in the final CSV, every cell has exactly 3 reps, zero duplicate
`(chunk_size,ratio,arm,detail,compute,rep)` keys.

## Machine exclusivity

Clean before and after (routine services only; `uptime` load average
0.3-1.4 throughout, consistent with this machine's baseline; no foreign
process at any exclusivity check).

## Process notes — resumability held up; one new defect class found and fixed, fully disclosed

**`random` madvise mode excluded from the main sweep's automatic loop.**
Item 10c/Campaign 12 Phase A precedent: this mode is known-slow
(160-220s/rep). It repeatedly landed right at this session's
background-task window boundary (observed window: as short as ~90s in
this session, well under the requested timeout), orphaning the child
process under its own `timeout 220` when the wrapping script's parent was
killed — the result printed to a pipe with no live reader, never reaching
the CSV. After this recurred twice in the first few resume cycles, `random`
was pulled out of `run_phaseD_sweep.sh`'s automatic mode loop entirely
(comment left in place explaining why) and handled instead via a
dedicated single-rep helper, `src/run_phaseD_random_rep.sh`, invoked
synchronously in the foreground once per `(chunk_size, ratio, rep)` —
the same disclosed strategy Phase A used. Result: **30/30 random-mode
reps captured** (unlike Phase A's partial 7/15 under a tighter timeout
budget) — every single foreground invocation completed and wrote a real
row.

**A resumability edge case surfaced 4 broken rows, caught by a post-sweep
row-count sanity check, not silently trusted.** Before `random` was
excluded from the automatic loop, it ran there briefly and produced 3 rows
with `rc=124` (timeout) and empty data fields — these still got written
(the script's `run_row()` always appends a row regardless of exit code),
and because `rep_done()` checks only whether a row *exists* for a given
`(chunk_size,ratio,arm,detail,compute,rep)` key, not whether it contains
real data, these 3 broken-but-present rows were never flagged as missing
by the later dedicated random-mode completion pass. Separately, one fully
garbage row (`,,A,random,n/a,,,,,...,,rc=136`, every identifying field
empty) was traced to my own tooling mistake: an early attempt to loop over
`(chunk_size, ratio, rep)` triples using a `read ... <<< "$t"` here-string
inside a `wsl bash -c` wrapper silently failed to populate the variables,
so `baseline_main` was invoked with empty positional arguments and exited
immediately (`rc=136`, consistent with a divide-by-zero from an empty
chunk-size argument). Caught by a straightforward check after the sweep
first reported complete: `301` total CSV rows against the grid's exact
expected total of `300`. Diagnosed precisely (all 4 rows identified by
exact content, not by guessing), the 4 rows deleted, and the 3 real
missing reps (2× `8MiB/ratio=0.25`, 1× `8MiB/ratio=0.375`) re-run cleanly
via the same dedicated helper. Final state, independently re-verified:
exactly 300 data rows, every `(chunk_size,ratio,arm,detail,compute,rep)`
key unique, every cell has exactly 3 reps, `rc=0` on all 300 rows, zero
lines in the log matching `BASELINE FAILED`/`DISCREPANCY`/
`RECONCILE FAILED`.

## Arm A — best `madvise` mode per (chunk size, ratio), bandwidth vs. the spike's 3396 MiB/s `O_DIRECT` ceiling

Best mode selected by touches/sec (matching Phase 4's own selection
method), per Phase A's repaired baseline. 6 of 10 cells exceed the
ceiling — a real, disclosed host-cache signature (per Phase A's diagnosis:
this is the Windows VHDX **host** cache, explicitly out of scope to
defeat), not the old guest-cache-failure defect (`read_bytes=0` never
appears — the guard would have aborted the run if it had).

| Chunk | Ratio | Best mode | `read_bytes` | Wall (s) | MiB/s | Exceeds 3396 MiB/s? |
|---|---|---|---|---|---|---|
| 8MiB | 0.25 | normal | 10,821,021,696 | 3.931 | 2625.2 | no |
| 8MiB | 0.375 | sequential | 10,737,455,104 | 3.415 | 2998.2 | no |
| 8MiB | 0.5 | normal | 10,743,746,560 | 3.836 | 2670.8 | no |
| 8MiB | 0.625 | sequential | 10,737,389,568 | 3.905 | 2622.3 | no |
| 8MiB | 0.75 | sequential | 10,737,389,568 | 2.865 | 3574.7 | **YES** |
| 128MiB | 0.25 | sequential | 10,737,455,104 | 2.673 | 3831.3 | **YES** |
| 128MiB | 0.375 | sequential | 10,737,455,104 | 2.969 | 3448.7 | **YES** |
| 128MiB | 0.5 | sequential | 10,737,455,104 | 2.887 | 3547.2 | **YES** |
| 128MiB | 0.625 | normal | 10,762,444,800 | 2.733 | 3754.9 | **YES** |
| 128MiB | 0.75 | sequential | 10,737,455,104 | 3.199 | 3201.4 | no |

## Arm B — bandwidth vs. the 3396 MiB/s ceiling

| Chunk | Ratio | `read_bytes` | Wall (s) | MiB/s | Exceeds 3396 MiB/s? |
|---|---|---|---|---|---|
| 8MiB | 0.25 | 10,695,475,200 | 3.812 | 2675.9 | no |
| 8MiB | 0.375 | 10,695,475,200 | 3.002 | 3398.0 | **YES** |
| 8MiB | 0.5 | 10,695,475,200 | 3.121 | 3267.7 | no |
| 8MiB | 0.625 | 10,695,475,200 | 2.982 | 3420.0 | **YES** |
| 8MiB | 0.75 | 10,695,475,200 | 2.547 | 4004.6 | **YES** |
| 128MiB | 0.25 | 10,192,158,720 | 2.438 | 3986.5 | **YES** |
| 128MiB | 0.375 | 10,192,158,720 | 3.027 | 3210.9 | no |
| 128MiB | 0.5 | 10,192,158,720 | 2.731 | 3558.5 | **YES** |
| 128MiB | 0.625 | 10,192,158,720 | 3.258 | 2983.8 | no |
| 128MiB | 0.75 | 10,192,158,720 | 2.764 | 3516.1 | **YES** |

11 of 20 arm A/B cells exceed the ceiling — more than half. Flagged
explicitly per instruction, not resolved further (out of scope: "do not
attempt to defeat the Windows VHDX host cache").

## OPT per (chunk size, ratio)

Computed via `belady_main` on arm D's own compute=0/rep1 reference trace.
Stable across chunk size at a given ratio (a property of the access
pattern and budget, not chunk granularity), matching every prior report's
OPT table exactly at the ratios they share.

| Ratio | OPT (8 MiB) | OPT (128 MiB) |
|---|---|---|
| 0.25 | 8,589,934,592 | 8,724,152,320 |
| 0.375 | 7,516,192,768 | 7,516,192,768 |
| 0.5 | 6,442,450,944 | 6,442,450,944 |
| 0.625 | 5,368,709,120 | 5,368,709,120 |
| 0.75 | 4,294,967,296 | 4,294,967,296 |

(128 MiB/ratio=0.25 differs slightly from 8 MiB's OPT at the same ratio —
128 MiB's coarser granularity means the budget doesn't divide evenly into
chunks, shifting the achievable capacity by one chunk; matches the same
small effect seen in Phase 3's own OPT table at its finest ratio.)

## Main table — `read_bytes`, bytes/touch, wall-clock, demand faults, `/OPT` (arms C/D/E, median of n=3)

| Chunk | Ratio | Arm | Compute | `read_bytes` | Bytes/touch | Wall (s) | Faults | `/OPT` |
|---|---|---|---|---|---|---|---|---|
| 8MiB | 0.25 | C | 0 | 10,737,418,240 | 8,388,608.0 | 8.301 | 1280 | 1.250 |
| 8MiB | 0.25 | C | 400000 | 10,737,418,240 | 8,388,608.0 | 8.753 | 1280 | 1.250 |
| 8MiB | 0.25 | D | 0 | 9,193,914,368 | 7,182,745.6 | 7.435 | 1096 | 1.070 |
| 8MiB | 0.25 | D | 400000 | 12,280,922,112 | 9,594,470.4 | 10.216 | 1464 | 1.430 |
| 8MiB | 0.25 | E | 0 | 10,678,697,984 | 8,342,732.8 | 6.221 | 695 | 1.243 |
| 8MiB | 0.25 | E | 400000 | 11,123,294,208 | 8,690,073.6 | 7.441 | 772 | 1.295 |
| 8MiB | 0.375 | C | 0 | 10,737,418,240 | 8,388,608.0 | 8.437 | 1280 | 1.429 |
| 8MiB | 0.375 | C | 400000 | 10,737,418,240 | 8,388,608.0 | 7.729 | 1280 | 1.429 |
| 8MiB | 0.375 | D | 0 | 8,338,276,352 | 6,514,278.4 | 6.326 | 994 | 1.109 |
| 8MiB | 0.375 | D | 400000 | 10,678,697,984 | 8,342,732.8 | 6.820 | 1273 | 1.421 |
| 8MiB | 0.375 | E | 0 | 8,841,592,832 | 6,907,494.4 | 4.699 | 614 | 1.176 |
| 8MiB | 0.375 | E | 400000 | 8,791,261,184 | 6,868,172.8 | 5.699 | 606 | 1.170 |
| 8MiB | 0.5 | C | 0 | 10,737,418,240 | 8,388,608.0 | 6.397 | 1280 | 1.667 |
| 8MiB | 0.5 | C | 400000 | 10,737,418,240 | 8,388,608.0 | 9.539 | 1280 | 1.667 |
| 8MiB | 0.5 | D | 0 | 7,549,747,200 | 5,898,240.0 | 4.457 | 900 | 1.172 |
| 8MiB | 0.5 | D | 400000 | 9,219,080,192 | 7,202,406.4 | 8.298 | 1099 | 1.431 |
| 8MiB | 0.5 | E | 0 | 7,516,192,768 | 5,872,025.6 | 4.932 | 533 | 1.167 |
| 8MiB | 0.5 | E | 400000 | 9,420,406,784 | 7,359,692.8 | 6.760 | 638 | 1.462 |
| 8MiB | 0.625 | C | 0 | 10,737,418,240 | 8,388,608.0 | 8.695 | 1280 | 2.000 |
| 8MiB | 0.625 | C | 400000 | 10,737,418,240 | 8,388,608.0 | 7.903 | 1280 | 2.000 |
| 8MiB | 0.625 | D | 0 | 6,727,663,616 | 5,255,987.2 | 4.753 | 802 | 1.253 |
| 8MiB | 0.625 | D | 400000 | 7,600,078,848 | 5,937,561.6 | 5.453 | 906 | 1.416 |
| 8MiB | 0.625 | E | 0 | 6,484,393,984 | 5,065,932.8 | 3.877 | 488 | 1.208 |
| 8MiB | 0.625 | E | 400000 | 6,970,933,248 | 5,446,041.6 | 4.577 | 516 | 1.298 |
| 8MiB | 0.75 | C | 0 | 10,737,418,240 | 8,388,608.0 | 6.228 | 1280 | 2.500 |
| 8MiB | 0.75 | C | 400000 | 10,737,418,240 | 8,388,608.0 | 7.599 | 1280 | 2.500 |
| 8MiB | 0.75 | D | 0 | 5,377,097,728 | 4,200,857.6 | 3.337 | 641 | 1.252 |
| 8MiB | 0.75 | D | 400000 | 5,762,973,696 | 4,502,323.2 | 4.505 | 687 | 1.342 |
| 8MiB | 0.75 | E | 0 | 4,940,890,112 | 3,860,070.4 | 2.776 | 411 | 1.150 |
| 8MiB | 0.75 | E | 400000 | 5,108,662,272 | 3,991,142.4 | 3.574 | 432 | 1.189 |
| 128MiB | 0.25 | C | 0 | 10,737,418,240 | 134,217,728.0 | 4.678 | 80 | 1.231 |
| 128MiB | 0.25 | C | 400000 | 10,737,418,240 | 134,217,728.0 | 5.389 | 80 | 1.231 |
| 128MiB | 0.25 | D | 0 | 9,529,458,688 | 119,118,233.6 | 3.946 | 71 | 1.092 |
| 128MiB | 0.25 | D | 400000 | 13,555,990,528 | 169,449,881.6 | 6.210 | 101 | 1.554 |
| 128MiB | 0.25 | E | 0 | 13,019,119,616 | 162,738,995.2 | 4.334 | 60 | 1.492 |
| 128MiB | 0.25 | E | 400000 | 17,179,869,184 | 214,748,364.8 | 6.353 | 81 | 1.969 |
| 128MiB | 0.375 | C | 0 | 10,737,418,240 | 134,217,728.0 | 5.303 | 80 | 1.429 |
| 128MiB | 0.375 | C | 400000 | 10,737,418,240 | 134,217,728.0 | 5.520 | 80 | 1.429 |
| 128MiB | 0.375 | D | 0 | 8,455,716,864 | 105,696,460.8 | 3.698 | 63 | 1.125 |
| 128MiB | 0.375 | D | 400000 | 11,945,377,792 | 149,317,222.4 | 5.887 | 89 | 1.589 |
| 128MiB | 0.375 | E | 0 | 11,408,506,880 | 142,606,336.0 | 3.913 | 50 | 1.518 |
| 128MiB | 0.375 | E | 400000 | 13,287,555,072 | 166,094,438.4 | 5.307 | 60 | 1.768 |
| 128MiB | 0.5 | C | 0 | 10,737,418,240 | 134,217,728.0 | 6.003 | 80 | 1.667 |
| 128MiB | 0.5 | C | 400000 | 10,737,418,240 | 134,217,728.0 | 5.835 | 80 | 1.667 |
| 128MiB | 0.5 | D | 0 | 7,650,410,496 | 95,630,131.2 | 3.657 | 57 | 1.188 |
| 128MiB | 0.5 | D | 400000 | 10,737,418,240 | 134,217,728.0 | 5.571 | 80 | **1.667** |
| 128MiB | 0.5 | E | 0 | 10,066,329,600 | 125,829,120.0 | 3.774 | 46 | 1.562 |
| 128MiB | 0.5 | E | 400000 | 10,871,635,968 | 135,895,449.6 | 4.863 | 48 | 1.688 |
| 128MiB | 0.625 | C | 0 | 10,737,418,240 | 134,217,728.0 | 5.658 | 80 | 2.000 |
| 128MiB | 0.625 | C | 400000 | 10,737,418,240 | 134,217,728.0 | 5.934 | 80 | 2.000 |
| 128MiB | 0.625 | D | 0 | 6,845,104,128 | 85,563,801.6 | 3.549 | 51 | 1.275 |
| 128MiB | 0.625 | D | 400000 | 7,918,845,952 | 98,985,574.4 | 4.628 | 59 | 1.475 |
| 128MiB | 0.625 | E | 0 | 8,724,152,320 | 109,051,904.0 | 3.704 | 42 | 1.625 |
| 128MiB | 0.625 | E | 400000 | 9,529,458,688 | 119,118,233.6 | 4.814 | 49 | 1.775 |
| 128MiB | 0.75 | C | 0 | 10,737,418,240 | 134,217,728.0 | 5.317 | 80 | 2.500 |
| 128MiB | 0.75 | C | 400000 | 10,737,418,240 | 134,217,728.0 | 5.932 | 80 | 2.500 |
| 128MiB | 0.75 | D | 0 | 5,502,926,848 | 68,786,585.6 | 2.650 | 41 | 1.281 |
| 128MiB | 0.75 | D | 400000 | 6,710,886,400 | 83,886,080.0 | 4.003 | 50 | 1.562 |
| 128MiB | 0.75 | E | 0 | 7,113,539,584 | 88,919,244.8 | 2.934 | 36 | 1.656 |
| 128MiB | 0.75 | E | 400000 | 6,308,233,216 | 78,852,915.2 | 4.463 | 37 | 1.469 |

**Notable cell (bolded above):** `128MiB/ratio=0.5/D/compute=400000`
reads the **entire region** (`10,737,418,240` bytes, `D/OPT=1.667`) —
identical to arm C's (`lru`, which always reads everything) byte count at
that exact cell. Under heavy compute at this chunk size and ratio, `D`'s
`layer_order` eviction policy degenerates to LRU-equivalent byte cost.
Not chased further; reported as measured.

## Device-busy, concurrently-outstanding, `pin_broken`, `infeasible` (arms C/D/E, per-rep median across n=3)

| Chunk | Ratio | Arm | Compute | Busy | Concur | `pin_broken` | `infeasible` |
|---|---|---|---|---|---|---|---|
| 8MiB | 0.25 | C | 0 | 0.809 | 1.00 | 0 | 0 |
| 8MiB | 0.25 | C | 400000 | 0.729 | 1.00 | 0 | 0 |
| 8MiB | 0.25 | D | 0 | 0.788 | 1.00 | 0 | 0 |
| 8MiB | 0.25 | D | 400000 | 0.743 | 1.00 | 0 | 0 |
| 8MiB | 0.25 | E | 0 | 0.846 | 2.00 | 0 | 0 |
| 8MiB | 0.25 | E | 400000 | 0.771 | 2.00 | 0 | 0 |
| 8MiB | 0.375 | C | 0 | 0.818 | 1.00 | 0 | 0 |
| 8MiB | 0.375 | C | 400000 | 0.726 | 1.00 | 0 | 0 |
| 8MiB | 0.375 | D | 0 | 0.807 | 1.00 | 0 | 0 |
| 8MiB | 0.375 | D | 400000 | 0.718 | 1.00 | 0 | 0 |
| 8MiB | 0.375 | E | 0 | 0.857 | 2.00 | 0 | 0 |
| 8MiB | 0.375 | E | 400000 | 0.750 | 2.00 | 0 | 0 |
| 8MiB | 0.5 | C | 0 | 0.823 | 1.00 | 0 | 0 |
| 8MiB | 0.5 | C | 400000 | 0.751 | 1.00 | 0 | 0 |
| 8MiB | 0.5 | D | 0 | 0.821 | 1.00 | 0 | 0 |
| 8MiB | 0.5 | D | 400000 | 0.730 | 1.00 | 0 | 0 |
| 8MiB | 0.5 | E | 0 | 0.839 | 1.00 | 0 | 0 |
| 8MiB | 0.5 | E | 400000 | 0.754 | 2.00 | 0 | 0 |
| 8MiB | 0.625 | C | 0 | 0.838 | 1.00 | 0 | 0 |
| 8MiB | 0.625 | C | 400000 | 0.735 | 1.00 | 0 | 0 |
| 8MiB | 0.625 | D | 0 | 0.821 | 1.00 | 0 | 0 |
| 8MiB | 0.625 | D | 400000 | 0.680 | 1.00 | 0 | 0 |
| 8MiB | 0.625 | E | 0 | 0.857 | 1.00 | 0 | 0 |
| 8MiB | 0.625 | E | 400000 | 0.698 | 1.00 | 0 | 0 |
| 8MiB | 0.75 | C | 0 | 0.828 | 1.00 | 0 | 0 |
| 8MiB | 0.75 | C | 400000 | 0.740 | 1.00 | 0 | 0 |
| 8MiB | 0.75 | D | 0 | 0.830 | 1.00 | 0 | 0 |
| 8MiB | 0.75 | D | 400000 | 0.662 | 1.00 | 0 | 0 |
| 8MiB | 0.75 | E | 0 | 0.857 | 1.00 | 0 | 0 |
| 8MiB | 0.75 | E | 400000 | 0.673 | 1.00 | 0 | 0 |
| 128MiB | 0.25 | C | 0 | 0.793 | 1.00 | 0 | 0 |
| 128MiB | 0.25 | C | 400000 | 0.654 | 1.00 | 0 | 0 |
| 128MiB | 0.25 | D | 0 | 0.786 | 1.00 | 0 | 0 |
| 128MiB | 0.25 | D | 400000 | 0.686 | 1.00 | 0 | 0 |
| 128MiB | 0.25 | E | 0 | 0.860 | 2.00 | **9** | **1** |
| 128MiB | 0.25 | E | 400000 | 0.747 | 2.00 | **8** | 0 |
| 128MiB | 0.375 | C | 0 | 0.804 | 1.00 | 0 | 0 |
| 128MiB | 0.375 | C | 400000 | 0.673 | 1.00 | 0 | 0 |
| 128MiB | 0.375 | D | 0 | 0.801 | 1.00 | 0 | 0 |
| 128MiB | 0.375 | D | 400000 | 0.697 | 1.00 | 0 | 0 |
| 128MiB | 0.375 | E | 0 | 0.858 | 2.00 | 0 | 0 |
| 128MiB | 0.375 | E | 400000 | 0.733 | 2.00 | 0 | 0 |
| 128MiB | 0.5 | C | 0 | 0.843 | 1.00 | 0 | 0 |
| 128MiB | 0.5 | C | 400000 | 0.688 | 1.00 | 0 | 0 |
| 128MiB | 0.5 | D | 0 | 0.822 | 1.00 | 0 | 0 |
| 128MiB | 0.5 | D | 400000 | 0.687 | 1.00 | 0 | 0 |
| 128MiB | 0.5 | E | 0 | 0.859 | 2.00 | 0 | 0 |
| 128MiB | 0.5 | E | 400000 | 0.728 | 1.00 | 0 | 0 |
| 128MiB | 0.625 | C | 0 | 0.839 | 1.00 | 0 | 0 |
| 128MiB | 0.625 | C | 400000 | 0.701 | 1.00 | 0 | 0 |
| 128MiB | 0.625 | D | 0 | 0.837 | 1.00 | 0 | 0 |
| 128MiB | 0.625 | D | 400000 | 0.685 | 1.00 | 0 | 0 |
| 128MiB | 0.625 | E | 0 | 0.864 | 1.00 | 0 | 0 |
| 128MiB | 0.625 | E | 400000 | 0.711 | 1.00 | 0 | 0 |
| 128MiB | 0.75 | C | 0 | 0.831 | 1.00 | 0 | 0 |
| 128MiB | 0.75 | C | 400000 | 0.704 | 1.00 | 0 | 0 |
| 128MiB | 0.75 | D | 0 | 0.841 | 1.00 | 0 | 0 |
| 128MiB | 0.75 | D | 400000 | 0.644 | 1.00 | 0 | 0 |
| 128MiB | 0.75 | E | 0 | 0.851 | 1.00 | 0 | 0 |
| 128MiB | 0.75 | E | 400000 | 0.669 | 1.00 | 0 | 0 |

`pin_broken`/`infeasible` are zero everywhere except
`128MiB/ratio=0.25/E` (both compute levels) — the tightest-budget,
largest-chunk cell, matching the pattern Phase 2/3/4 also found at their
own tightest-margin cells (small capacity in chunks, real prefetch
overlap, more chances for a pin to be broken by a demand fault before
consumption).

## Correctness

Phase D introduces no new chunk size beyond what Phase B's
STOP-AND-REPORT gate already covered ({4,8,16,32} MiB, including 8 MiB
used here) and no code path beyond what Phase B's own T-1..T-7 re-run
(after the 512-chunk exercise) already verified — 128 MiB has been
exercised by every prior sweep back to V2. Per the campaign's Correctness
section, which specifically asked for a re-run "after Phase B" (for the
512-chunk exercise), and per Phase 3's own established precedent for not
re-running T-1..T-7 a second time when no new mechanism or code path is
introduced by a sweep that only varies an existing, already-tested
argument (chunk size, ratio, compute level — all pre-existing parameters):
**not re-executed a second time this phase**, disclosed rather than
silently skipped. Phase B's T-1..T-7 pass (all PASS, zero mismatches)
remains the most recent verified state.

## What I did NOT test

- Any chunk size other than 8 MiB and 128 MiB.
- Any ratio outside {0.25, 0.375, 0.5, 0.625, 0.75} or compute level
  outside {0, 400000} — the campaign's own fixed grid.
- `--sync-handler` or driver-thread counts other than 8 — fixed per
  instruction.
- Re-running T-1..T-7 a third time this phase (see Correctness above).

## Final check

- No number estimated, inferred, or copied from documentation — every
  value is a direct read from `results/campaign12_phaseD_paper_table.csv`
  (median of n=3) or a direct computation over retained
  `--fetch-trace`/reference-trace files (`belady_main`'s own printed
  output for OPT; `scratch/analyze_phaseD.py` for busy/concurrency
  metrics, using Phase B's per-rep-then-median fix rather than the
  pooled-timestamp method that produced a false outlier there).
- No test was weakened. The 4 broken/garbage rows found mid-phase were
  identified by exact content, deleted, and their 3 real missing reps
  re-run cleanly — nothing was silently smoothed over or estimated in
  their place.
- Every requested metric was reported: `read_bytes` per touch, demand
  faults, wall-clock, device-busy, concurrently-outstanding, `pin_broken`,
  `infeasible`, OPT, and both D/OPT and E/OPT ratios (C/OPT also reported,
  as a free sanity check on the always-fetch-everything baseline).
- Every arm A/B cell's achieved bandwidth was reported against the
  3396 MiB/s ceiling, with cells exceeding it flagged explicitly (11 of
  20) rather than silently averaged away.
- The chunk size used for the 8 MiB grid was chosen and justified openly
  at the top of this file, from Phase B's own measured aggregate result,
  not assumed or guessed.
