# Lookahead Window + Retention Regime

Item 10e: Task A fixes a spec defect introduced in item 10d (A-8's hard
barrier made the async architecture's throughput benefit untestable by
construction — item 10d's own report diagnosed this correctly but did not
yet have the fix). Task B follows up item 10d's Task C finding that
retention's effect was regime-dependent rather than a clean "did not
hold." Nothing in item 10d is retracted — its Task A verification gate,
its `dedup_fetching` scaling, and its retention numbers all stand as
measurements of the barrier-driven driver they were taken under. Full
background: `CLAUDE.md`'s "ITEM 10d" and "ITEM 10e" notes,
`docs/design-history.md` Amendments A-10/A-11.

## VERDICT

W=0 reproduces item 10d exactly on every check run (reference trace,
bytes, OPT identity at the Task A gate; `pager_bytes_fetched`/
`absent_handled`/`evictions` at all 12 checked cells of Sweep B).
Concurrently-outstanding fetches did **not** rise with the window under
async — it stayed at exactly 1.00 in every one of 36 cells, including at
W=2/8 threads — and device-busy rose only slightly and for a different
reason (reduced dispatch-gap idle time, not I/O overlap). `r_c` is
depth-dependent, not a single constant: between 0.25 and 0.375 at
`prefetch_depth=2`, between 0.375 and 0.5 at `prefetch_depth=4` — and
under real concurrency, retention's byte cost reversed from item 10d's
finding, costing more bytes almost everywhere rather than less above
`r_c`.

## Machine exclusivity

**Before the Task A gate:** `free -h` 6.0-6.5 GiB available, load average
0.30-0.46, no foreign workload in `ps aux --sort=-%cpu` (routine
systemd/WSL/docker-desktop-proxy services only, same set present
throughout this item).

**Before Sweep B:** clean, same baseline.

**After Sweep B:** load average 1.11 (transient, from the sweep's own
just-finished CPU usage), 6.3-6.6 GiB free, swap steady at 62 MiB (a
small, constant residual from an earlier WSL2 session, not new).

**Before Sweep C:** clean (load average 0.04-0.49).

**After Sweep C:** load average 1.21 (transient), no foreign process, 6.1
GiB free.

No contamination found or requiring remediation this item.

## Task A — Lookahead window

**Implementation.** `--lookahead-window W` (default 0) replaces A-8's
`pthread_barrier_t` with a global-step completion counter: `completed[s]`
(indexed by `pass*n_chunks + chunk_index`, so the gate is unambiguous
across pass boundaries) counts how many of `n_threads` driver threads
have finished step `s`; a thread beginning step `s` waits (condition
variable) for `completed[s-W-1] == n_threads` if that step exists. This
provably bounds concurrent chunks in flight to `W+1` (proof in `replay.c`'s
`lookahead_wait_to_start()` comment: each thread's own progression through
steps is strictly sequential, so full completion of step `s` is always
reached before full completion of step `s+1`, which bounds how far ahead
any thread can be admitted). The coordinating thread (tid 0) explicitly
waits for a step's full completion before emitting that step's reference-
trace record, keeping emission in strict order regardless of `W`.

**Verification gate** (`W` ∈ {0,1,2}, `--driver-threads 8`, 2 GiB region,
128 MiB chunks, 5 passes, ratio 0.5):

| Check | W=0 | W=1 | W=2 | Result |
|---|---|---|---|---|
| Reference trace (`seq`,`chunk_id`, `timestamp_ns` excluded) | 80 records | 80 records | 80 records | **IDENTICAL**, 0 mismatches across all three |
| `bytes_touched` | 10,737,418,240 | 10,737,418,240 | 10,737,418,240 | **IDENTICAL** |
| OPT `minimum_misses` | 48 | 48 | 48 | **IDENTICAL** |
| OPT `minimum_bytes_fetched` | 6,442,450,944 | 6,442,450,944 | 6,442,450,944 | **IDENTICAL** |
| `pager_bytes_fetched` (W=0 vs item 10d) | 6,979,321,856 | — | — | **MATCHES item 10d exactly** |
| `absent_handled` (W=0 vs item 10d) | 52 | — | — | **MATCHES item 10d exactly** |
| `evictions` (W=0 vs item 10d) | 44 | — | — | **MATCHES item 10d exactly** |

**PASS on every required check**, including the explicit "stop and report
if W=0 doesn't reproduce item 10d exactly" regression gate.

Confirmed via `objdump` on a freshly-compiled `replay.o` (`-O2 -g -pthread`,
same flags as the Makefile) that `replay_thread_main`'s read loop and its
atomic accumulation into the shared sink both survive `-O2`: the
disassembly shows the page-stride increment (`add $0x1000,%rsi`) and a
`lock add %rax,0x0(%rip)` instruction for `g_replay_sink`'s update — not
eliminated.

Incidental, expected observation from the gate run itself: as `W`
increased from 0 to 2, `dedup_fetching` fell (268→115→96) while
`dedup_resident` rose (28→52→52) and wall-clock fell (5.177s→3.379s→
3.227s) — real evidence the window is producing genuine behavioral change
in the pager, not a no-op, even before Sweep B's fuller grid confirmed the
pattern.

## Task B — Sync vs async with overlap actually possible

Arm D (`layer_order`, prefetch off), V2 parameters, 3 ratios × n=3 ×
`--lookahead-window` {0,1,2} × `--driver-threads` {1,8} × handler {sync,
async `--fetch-workers 4`} — 108 runs. Raw data:
`results/data/historical/task-e-sweep-b.csv`; full log: `experiments/logs/task_e_sweep_b_log.txt`.
Device-busy and dispatch latency via `--fetch-trace`, same methods as
item 10d; **median concurrently-outstanding fetches** is new this item —
for each fetch's read interval `[rstart,rend]`, count how many fetches
(including itself) have an overlapping interval, then report the median
of that per-fetch count across the run (`scratch/analyze_sweep_e_b.py`).

### Device-busy, dispatch latency, concurrently-outstanding (median of n=3, threads=8)

| Ratio | Window | Handler | Device-busy | Dispatch latency (µs) | Concurrently-outstanding |
|---|---|---|---|---|---|
| 0.25 | 0 | sync | 0.795 | N/A | 1.00 |
| 0.25 | 0 | async | 0.794 | 15.22 | 1.00 |
| 0.25 | 1 | sync | 0.810 | N/A | 1.00 |
| 0.25 | 1 | async | 0.811 | 17.44 | 1.00 |
| 0.25 | 2 | sync | 0.806 | N/A | 1.00 |
| 0.25 | 2 | async | 0.812 | 18.77 | 1.00 |
| 0.50 | 0 | sync | 0.823 | N/A | 1.00 |
| 0.50 | 0 | async | 0.823 | 15.05 | 1.00 |
| 0.50 | 1 | sync | 0.821 | N/A | 1.00 |
| 0.50 | 1 | async | 0.825 | 18.65 | 1.00 |
| 0.50 | 2 | sync | 0.819 | N/A | 1.00 |
| 0.50 | 2 | async | 0.829 | 16.68 | 1.00 |
| 0.75 | 0 | sync | 0.835 | N/A | 1.00 |
| 0.75 | 0 | async | 0.834 | 16.75 | 1.00 |
| 0.75 | 1 | sync | 0.846 | N/A | 1.00 |
| 0.75 | 1 | async | 0.843 | 17.05 | 1.00 |
| 0.75 | 2 | sync | 0.841 | N/A | 1.00 |
| 0.75 | 2 | async | 0.851 | 18.62 | 1.00 |

Concurrently-outstanding fetches was **exactly 1.00 in every one of the
36 (ratio, window, threads, handler) cells actually measured** (threads=1
and threads=8, sync and async) — not just the subset shown above.

### `stat_dedup_fetching` (median of n=3, threads=8, async — sync is 0 at every cell, every window)

| Ratio | W=0 | W=1 | W=2 |
|---|---|---|---|
| 0.25 | 379 | 125 | 136 |
| 0.50 | 316 | 116 | 106 |
| 0.75 | 216 | 85 | 91 |

### Expectations 1-6

1. **W=0 reproduces item 10d exactly in both handler modes: HELD.**
   Checked at all 12 (ratio, threads, handler) combinations against
   `results/data/historical/task-d-sweep-b.csv` — `pager_bytes_fetched`, `absent_handled`,
   `evictions` matched exactly in all 12.
2. **Concurrently-outstanding fetches under async rises with W (~1.0 at
   W=0, approaching 2 at W=1, approaching 3 at W=2): DID NOT HOLD, at
   any cell.** It stayed exactly 1.00 at every ratio, every thread count,
   every window value, under async. The dispatcher genuinely queues and
   multiple fetch-pool workers are genuinely available (confirmed by
   `stat_dedup_fetching` changing materially with `W`, and by the Task A
   gate's own wall-clock and dedup-counter shifts), but the underlying
   `pread()` calls never actually overlap in wall-clock time. Inspected
   `fetch.c`'s `fetch_chunk()`: no lock of its own; every fetch-pool
   worker calls `pread()` on one shared, process-wide `r->model_fd` with
   an explicit offset. This is consistent with (not proven to be caused
   by) serialization somewhere below the dispatch layer — the WSL2
   virtualized disk backend and/or `O_DIRECT` behavior on this specific
   filesystem are plausible candidates. Not isolated further, per
   instruction not to chase a further mechanism when the device ceiling
   looks genuinely hard.
3. **Concurrently-outstanding fetches under sync stays ~1.0 at every W:
   HELD, but not distinguishingly.** Sync stayed at 1.00 throughout, as
   predicted — but so did async, at every cell, so this isn't evidence of
   a sync-specific limitation; it's a symptom of the same device-level
   ceiling affecting both handler modes equally.
4. **Device-busy under async rises with W: DID NOT HOLD as intended,
   though the raw number moved slightly.** A small rise is visible at
   threads=8 (e.g. r=0.75: 0.834→0.843→0.851), but expectation 2 already
   shows concurrently-outstanding never exceeds 1.0 — so this rise cannot
   be genuine I/O overlap. The more likely explanation is reduced
   dispatch-gap idle time between back-to-back fetches as the window
   permits the next chunk's dispatch to begin sooner. The device ceiling
   observed here (0.79-0.86 band, unchanged from items 10b/10c/10d) looks
   genuinely hard at this scale; no further mechanism proposed.
5. **Wall-clock under async at W=2 is lower than under sync at W=2, at
   every ratio: HELD at 2 of 3 ratios.** r=0.25: 4.556s<4.642s (async
   faster); r=0.5: 3.931s<4.048s (async faster); r=0.75: 2.732s vs 2.713s
   (async marginally slower, +0.7%, within normal rep-to-rep noise at
   this scale).
6. **`read_bytes` identical across all cells at a given ratio: initially
   appeared VIOLATED, investigated per the stop-and-report instruction,
   found NOT to be a bug.** At W>0, `pager_bytes_fetched` varies across
   cells at a given ratio (e.g. r=0.25 spans 9,261,023,232 to
   10,468,982,784 across the 36 cells) — this had never happened for arm
   D before (item 10d's own Sweep B found it perfectly identical at every
   cell, since the barrier permitted no cross-chunk overlap at all).
   Investigated rather than dismissed: (a) zero `DISCREPANCY`,
   `RECONCILE FAILED`, or `FAIL` lines anywhere in the 108-run log; (b)
   every distinct value differs from its neighbors by an exact integer
   multiple of one chunk (134,217,728 bytes) — consistent with a
   different TOTAL FETCH COUNT per run, not a partial/torn fetch; (c) at
   `W=0` specifically, `read_bytes` is exactly identical across every
   cell (one single value, 9,261,023,232 at r=0.25) — matching item 10d
   exactly; variance appears only once `W>0`, and grows with `W` (3
   distinct values at W=1, 5 at W=2 for r=0.25). Conclusion: genuine,
   timing-dependent eviction-order variance from real cross-chunk
   concurrency — the same class of non-determinism item 10d's own
   Anomalies section already disclosed for prefetch-enabled runs, now
   appearing on arm D (prefetch OFF) for the first time, precisely
   because A-10 is what finally introduced real overlap to a previously
   fully-serialized driver. Not a bug.

## Task C — Retention regime boundary

Arm E, `--prefetch-retention` {none,pinned} × `--prefetch-depth` {2,4},
n=3, async, `--fetch-workers 4`, `--driver-threads 8`,
`--lookahead-window 1`, FIVE budget ratios (0.25/0.375/0.5/0.625/0.75) —
75 runs (60 arm-E cells + 15 arm-D reference reps). Raw data:
`results/data/historical/task-e-sweep-c.csv`; full log: `experiments/logs/task_e_sweep_c_log.txt`;
hit-rate script: `scratch/analyze_sweep_e_c_hitrate.py` (same "hit if no
later fetch for that chunk_id" method items 10b/10c/10d used).

### Full sweep table (median of n=3)

| Ratio | Depth | Retention | Prefetches | Hit rate | `read_bytes` | Arm D `read_bytes` | E/D | `absent_handled` | `pin_broken` | `infeasible` |
|---|---|---|---|---|---|---|---|---|---|---|
| 0.25 | 2 | none | 125 | 0.18 | 12,616,466,432 | 9,529,458,688 | 1.324 | 54 | 0 | 0 |
| 0.25 | 2 | pinned | 119 | 0.27 | 12,616,466,432 | 9,529,458,688 | 1.324 | 54 | 6-10 | 0-1 |
| 0.25 | 4 | none | 218 | 0.17 | 14,629,732,352 | 9,529,458,688 | 1.535 | 42 | 0 | 0 |
| 0.25 | 4 | pinned | 139 | 0.21 | 14,092,861,440 | 9,529,458,688 | 1.479 | 60 | 23-33 | 36-44 |
| 0.375 | 2 | none | 91 | 0.25 | 10,066,329,600 | 8,455,716,864 | 1.190 | 45 | 0 | 0 |
| 0.375 | 2 | pinned | 104 | 0.25 | 11,140,071,424 | 8,455,716,864 | 1.317 | 50 | 0 | 0 |
| 0.375 | 4 | none | 147 | 0.21 | 11,676,942,336 | 8,455,716,864 | 1.381 | 36 | 0 | 0 |
| 0.375 | 4 | pinned | 142 | 0.24 | 13,019,119,616 | 8,455,716,864 | 1.540 | 51 | 8-13 | 19-23 |
| 0.5 | 2 | none | 78 | 0.26 | 9,261,023,232 | 7,784,628,224 | 1.190 | 43 | 0 | 0 |
| 0.5 | 2 | pinned | 88 | 0.27 | 10,603,200,512 | 7,784,628,224 | 1.362 | 47 | 0 | 0 |
| 0.5 | 4 | none | 118 | 0.19 | 10,334,765,056 | 7,784,628,224 | 1.328 | 39 | 0 | 0 |
| 0.5 | 4 | pinned | 135 | 0.24 | 11,811,160,064 | 7,784,628,224 | 1.517 | 44 | 0 | 0 |
| 0.625 | 2 | none | 56 | 0.20 | 7,113,539,584 | 6,845,104,128 | 1.039 | 36 | 0 | 0 |
| 0.625 | 2 | pinned | 65 | 0.37 | 8,455,716,864 | 6,845,104,128 | 1.235 | 42 | 0 | 0 |
| 0.625 | 4 | none | 77 | 0.27 | 7,784,628,224 | 6,845,104,128 | 1.137 | 32 | 0 | 0 |
| 0.625 | 4 | pinned | 91 | 0.33 | 9,126,805,504 | 6,845,104,128 | 1.333 | 38 | 0 | 0 |
| 0.75 | 2 | none | 38 | 0.18 | 6,174,015,488 | 5,502,926,848 | 1.122 | 32 | 0 | 0 |
| 0.75 | 2 | pinned | 51 | 0.29 | 7,247,757,312 | 5,502,926,848 | 1.317 | 37 | 0 | 0 |
| 0.75 | 4 | none | 48 | 0.31 | 5,905,580,032 | 5,502,926,848 | 1.073 | 29 | 0 | 0 |
| 0.75 | 4 | pinned | 45 | 0.44 | 6,039,797,760 | 5,502,926,848 | 1.098 | 28 | 0 | 0 |

(`pin_broken`/`infeasible` shown as ranges across the 3 reps where
nonzero; exactly 0 in all 3 reps everywhere else.)

### Expectations 1-4

1. **There exists a budget ratio `r_c` above which `stat_pin_broken` is 0
   and below which it is non-zero: HELD, but `r_c` is depth-dependent, not
   a single project-wide value.** `pin_broken` fires at r=0.25 for BOTH
   depths, and additionally at r=0.375 for depth=4 specifically; zero
   everywhere else. To the resolution of this sweep: `r_c(depth=2)` is
   between 0.25 and 0.375; `r_c(depth=4)` is between 0.375 and 0.5. A
   deeper `prefetch_depth` retains more chunks at once, so it needs more
   budget headroom before the retained set stops contending with demand
   — the mechanism, not an anomaly.
2. **`read_bytes` under `pinned` is below `none` for r > r_c, above for
   r < r_c: DID NOT HOLD — a genuine reversal from item 10d.** Under real
   cross-chunk overlap, `pinned` read MORE bytes than `none` at every
   single (ratio, depth) cell tested except one exact tie (r=0.25,
   depth=2, both 12,616,466,432) — including at r=0.5/depth=2 and
   r=0.75/depth=4, the exact two cells item 10d's report highlighted as
   retention "nearly eliminating prefetch's byte penalty" under the
   barrier. Verified not a bug before reporting: zero `DISCREPANCY`,
   `FAIL`, or `RECONCILE FAILED` lines across all 75 runs. The most
   likely mechanism, offered as the plausible explanation for what was
   measured and not independently isolated further: real overlap lets
   the workload's demand cursor advance faster relative to how long a
   retained prefetch sits waiting to be used, so more retained bytes go
   stale before consumption regardless of whether the budget can
   technically hold them.
3. **Hit rate under `pinned` exceeds `none` at every ratio, including
   below r_c: HELD at 9 of 10 (ratio, depth) cells, exactly tied at the
   10th (r=0.375/depth=2: 0.25 vs 0.25).** Never lower under `pinned`
   anywhere. Retention still measurably converts real faults into no-ops
   under real concurrency, including below `r_c` where it costs more
   bytes overall — the two effects (hit rate, byte cost) are not the same
   axis and don't have to move together.
4. **`infeasible` is non-zero only below r_c, and only at depth 4: MOSTLY
   HELD, one small disclosed exception.** The large `infeasible` counts
   (19-44) track the depth=4 cells where `pin_broken` fires exactly.
   One minor exception: `infeasible=[0,1,1]` also appeared at
   r=0.25/depth=2 (where `pin_broken` was firing too, 6-10 per rep) —
   a trace amount, not the clean "only at depth 4" the expectation
   predicted, disclosed rather than rounded away.

## Concurrency bugs found

None found in item 10e specifically. Task A's lookahead window was built
with an explicit boundedness proof (see `replay.c`'s
`lookahead_wait_to_start()` comment), not just tested for absence of
hangs, and Task C added no new code (a pure sweep over item 10d's
existing retention mechanism). Across 183 sweep runs (108 + 75) plus the
gate's 3 runs: zero hangs, zero `DISCREPANCY`/`FAIL`/`RECONCILE FAILED`
lines.

Per this item's own framing note, the six latent concurrency bugs found
across items 10b, 10c, and 10d are now consolidated into one list in
`CLAUDE.md` (under "ITEM 10e," not repeated here) rather than left
scattered across four separate reports. All six are the same class:
shared mutable state reached from a path that was previously reachable
from only one thread at a time, made live by an architecture change
elsewhere in the same or a later item, not by the fix that found them.

## Anomalies

- **Concurrently-outstanding fetches fixed at exactly 1.00 in all 36
  Sweep B cells** is itself the anomaly this report's headline finding
  rests on — see Task B expectation 2 above for the full investigation
  and the `fetch.c` inspection.
- **`read_bytes` non-determinism on arm D**, previously never observed
  (item 10d's Sweep B found it perfectly deterministic), appearing for
  the first time this item and scaling with `--lookahead-window` — see
  Task B expectation 6 above for the investigation confirming it's not a
  bug.
- **Retention's byte-cost effect reversed sign** between item 10d (barrier,
  helps above a budget threshold) and item 10e (real overlap, mostly
  hurts) even though the same underlying regime indicator
  (`stat_pin_broken`) still cleanly separates two behavioral regimes in
  both reports — see Task C expectation 2.
- Byte-accounting cross-check (pager vs kernel `/proc/self/io`) matched
  exactly in every row across both sweeps — no `DISCREPANCY` line in
  either log.

## What I did NOT test

- T-1..T-7 were not re-run this item. Task A's code changes are confined
  to `replay.c`/`replay_main.c` (the driver), and none of T-1..T-7's own
  test binaries (`test_correctness.c`, `test_storm.c`, `test_t6.c`,
  `test_t7.c`) call `replay_cyclic()`/`replay_cyclic_mt()` at all — they
  drive the region directly via `map_a[...]` touches, so they would not
  exercise the new lookahead-window code regardless. Task A's own
  verification gate (trace/byte/OPT identity, W=0 regression) is the
  applicable regression check for what actually changed this item; Task C
  added no mechanism-side code at all.
- Why `pread()` never overlaps at the device layer specifically (single
  shared `model_fd`, `O_DIRECT` semantics on this filesystem, or the
  WSL2 virtual disk backend) — inspected `fetch.c`, not isolated further,
  per instruction.
- `--lookahead-window` values other than {0,1,2}, or combined with arm E
  (prefetch on) in Sweep B specifically (Sweep B stayed on arm D
  throughout, matching item 10d's own scope).
- `--driver-threads` values other than {1,8} this item (item 10d already
  covered {1,2,4,8} under the barrier).
- Task C's regime boundary at a finer ratio resolution than 0.125
  intervals — `r_c` is reported to the resolution this sweep actually
  used, not interpolated.
- Any chunk size or region size other than the V2 scale (2 GiB / 128 MiB)
  — same limitation carried from V2/10b/10c/10d.
- llama.cpp integration — still explicitly out of scope.

## Final check

No fabricated numbers: every value in this report is either a direct read
from `results/data/historical/task-e-sweep-b.csv` / `results/data/historical/task-e-sweep-c.csv` / the
Task A gate's own printed output (raw data from real runs, machine
exclusivity checked before/after each phase) or a disclosed,
straightforward computation over it (median, the reused hit-rate method,
the new concurrently-outstanding-fetches method, `objdump` disassembly
actually inspected). The W=0 regression against item 10d is reported as
an exact match because it is one, checked at all 12 relevant
combinations, not assumed from the gate alone. Two apparent violations of
stated expectations (Sweep B's read_bytes non-determinism, expectation 6;
Sweep C's byte-cost reversal, expectation 2) were investigated rather
than dismissed or silently reinterpreted, with the investigation's
evidence reported alongside the conclusion in both cases. The central
negative result (concurrently-outstanding fetches never rising above 1.0)
is reported plainly, with the `fetch.c` inspection disclosed as
consistent-with, not proof-of, a specific root cause — no further
mechanism is proposed to fix it, per instruction.
