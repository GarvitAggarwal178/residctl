# Campaign 13 Phase A — Policy Determinism: Arm D Degenerated to Arm C, Isolated

## The observation and the hypothesis, restated precisely

`experiments/10-consolidated-sweep.md` recorded that at 128 MiB / r=0.5 /
`--compute-ns-per-mib 400000`, arm D read the entire region
(`read_bytes=10,737,418,240`), byte-identical to arm C's always-fetch-
everything `lru` baseline. At r=0.5 with 16 chunks, `layer_order` holds 8;
the provable cyclic floor (`n + (passes-1)×max(n-k,0)` = `16 + 4×8` = 48)
is well below both that cell's 80 and V2's original 57.

**Hypothesis under test:** `layer_order`'s replacement decisions are
timing-dependent because the successor chain (`layer_order_state_t`) is
built from the *order in which chunks are faulted*, and under concurrent
driver threads plus a real compute phase, that order varies run to run.

**Source-code check, before running anything (per instruction: "test it,
do not assume it").** Read `src/policy.c` and the two `on_fault` call
sites in `src/pager.c`:

- `layer_order_on_fault()` (`policy.c:67-73`) is the **only writer** into
  `successor[]`/`last_fetched` (the cursor). It is called from
  `handle_absent_dispatch()` (`pager.c:167-176`, the async handler's
  dispatch path) under `r->budget_lock`, immediately after a real
  userfault is received and dispatched — **at fault-dispatch time, not
  when the chunk later becomes `CHUNK_RESIDENT`.**
- The campaign's own hypothesis text describes the cursor as "derived
  from the chunk that most recently became `CHUNK_RESIDENT`" —
  **this is not quite what the code does.** `on_resident` is a no-op for
  `layer_order` (`policy.c:75`); the cursor (`last_fetched`) is set at
  **fault** time, before the fetch even starts, not at residency time.
  Corrected framing: the successor chain records **fault-dispatch order**,
  not resident-completion order.
- Under the async handler, `handle_absent_dispatch()` runs in the single
  pager thread that reads uffd messages — but under `--driver-threads 8`,
  which of 8 concurrently-running driver threads' page faults reaches the
  kernel's uffd queue **first** is a real OS-scheduling question, not
  fixed by the reference sequence. With `--compute-ns-per-mib 400000`
  making each thread's per-chunk wall-clock time genuinely variable
  (real, measured arithmetic, not a fixed delay), the relative arrival
  order of concurrent faults becomes timing-dependent in a way it isn't
  when compute is 0 (threads move in near-lockstep) or when there's only
  one driver thread (no concurrent faults to order at all).

This structurally supports a **refined** version of the hypothesis: not
"residency order," but **fault-dispatch order**, is the timing-dependent
input. Tested directly below.

## A.1 — Reproduce and characterize, n=10

Script: `scripts/historical/run-c13-phaseA1.sh`. Exact cell: arm D, 128 MiB chunks,
r=0.5, `--compute-ns-per-mib 400000`, async, `--fetch-workers 4
--driver-threads 8 --lookahead-window 1`, n=10. Every rep reported
individually, `rc=0` on all 10, clean machine exclusivity before/after.

| Rep | `read_bytes` | `absent_handled` | `evictions` | Wall (s) |
|---|---|---|---|---|
| 1 | 10,468,982,784 | 78 | 70 | 6.302 |
| 2 | **10,737,418,240** | **80** | 72 | 5.974 |
| 3 | 10,468,982,784 | 78 | 70 | 5.577 |
| 4 | 10,603,200,512 | 79 | 71 | 5.672 |
| 5 | 10,603,200,512 | 79 | 71 | 5.857 |
| 6 | 11,140,071,424 | **83** | 75 | 6.659 |
| 7 | 10,603,200,512 | 79 | 71 | 5.750 |
| 8 | 10,334,765,056 | 77 | 69 | 5.626 |
| 9 | 9,932,111,872 | 74 | 66 | 5.210 |
| 10 | 10,200,547,328 | 76 | 68 | 5.556 |

**Not deterministic at this configuration.** `absent_handled` spans
74-83 across 10 reps (touches=80 for all reps) — a 9-fault, ~12% range.
Rep 2 reproduces Phase D's reported degenerate cell exactly (80/80,
byte-identical to arm C). **Rep 6 is worse than arm C**:
`absent_handled=83` **exceeds** the 80 total reference touches, and
`read_bytes=11,140,071,424` exceeds arm C's 10,737,418,240 by 3.8% — arm D
fetched more data than "always fetch everything" would. This is only
possible if the same logical touch triggers more than one real fetch
within a single pass (a chunk evicted and re-faulted before the touch
that needed it is fully consumed by all 8 threads) — a real, measured
phenomenon, not a bookkeeping artifact (confirmed independently in A.4
below and consistent with Phase D's own raw data). **A policy whose fault
count varies run to run, reported as measured, not averaged into a single
number**, per instruction.

## A.2 — Determinism sweep

Script: `scripts/historical/run-c13-phaseA2.sh`. Arm D, n=5, r=0.5, at each of the 6
cells. All `rc=0`, clean exclusivity.

| Cell | Threads | Window | Compute | Chunk | `absent_handled` (5 reps) | Deterministic? |
|---|---|---|---|---|---|---|
| 1 | 1 | 0 | 0 | 128MiB | 57, 57, 57, 57, 57 | **YES** |
| 2 | 1 | 0 | 400000 | 128MiB | 57, 57, 57, 57, 57 | **YES** |
| 3 | 8 | 0 | 0 | 128MiB | 57, 57, 57, 57, 57 | **YES** |
| 4 | 8 | 1 | 0 | 128MiB | 57, 57, 57, 57, 57 | **YES** |
| 5 | 8 | 1 | 400000 | 128MiB | 76, 77, 70, 72, 73 | **NO** |
| 6 | 8 | 1 | 400000 | 8MiB | 1059, 1029, 1063, 1061, 1063 | **NO** |

**Cell 1 (fully serial) is deterministic** at exactly 57/57 —
**matches V2's original measurement (57) exactly**, confirming both the
gate and this campaign's own harness reproduce the established baseline
before any new variable is introduced.

**Clean, complete isolation of the trigger.** Every combination of
threads/window/compute that leaves at least one of {real concurrency,
real overlap, real timing variance} absent is perfectly deterministic:

- 8 threads alone, hard barrier, no compute (cell 3): deterministic.
  Concurrency without overlap-freedom doesn't matter — the barrier forces
  full resynchronization at every chunk boundary regardless of thread
  count.
- 8 threads, real overlap (`--lookahead-window 1`), no compute (cell 4):
  **still deterministic.** Overlap-freedom without timing variance
  doesn't matter either — with compute=0, all 8 threads move through
  each chunk's page range at essentially the same (near-instant) rate, so
  even though the *scheduler* is technically free to interleave, there is
  no substantial timing signal for it to interleave *differently* run to
  run.
- 1 thread, compute=400000 (cell 2): deterministic. Real per-touch timing
  variance without any other thread to race against cannot produce
  ordering non-determinism — there's nothing to race.
- **8 threads + real overlap + real compute (cell 5) is the only
  combination that shows variance**, and it does so at both chunk sizes
  tested (cell 6, at 8 MiB, shows the same qualitative pattern: 1029-1063
  out of a much larger 1280-touch scale, a comparable ~3% relative
  spread).

**This refines the campaign's own hypothesis with more precision than
originally stated: the degeneration requires all three of {`--driver-
threads > 1`, `--lookahead-window > 0`, `--compute-ns-per-mib > 0`}
simultaneously — not "concurrency" broadly, and not any two of the three
alone.** This matches the source-code finding directly: `--lookahead-
window 0` forces the hard barrier that used to be the only option before
item 10e (A-10), which removes any freedom for fault-dispatch order to
vary; without a compute phase, there is no real timing signal for that
freedom (when it exists) to act on.

## A.3 — Instrument the victim decision

Added `--policy-trace <path>` to `replay_main` (mirrors `--fetch-trace`'s
design exactly: preallocated array, flushed once at exit, gated behind
the flag, zero cost when unused). New files: `src/policy_trace.h`,
`src/policy_trace.c`. Record format, exactly as specified, with one
disclosed sizing choice: a **fixed 64-bit** (`lo`+`hi`) resident-set
bitmap, valid for `n_chunks <= 64` — sufficient for every cell this phase
traces (128 MiB = 16 chunks), not valid at 8 MiB (256 chunks) or smaller;
`n_resident` itself is always correct regardless of `n_chunks`. One record
per `select_victim()` call that returns a real victim (both the demand
and prefetch eviction paths in `budget.c` are instrumented); calls
returning `CHUNK_NONE` are not eviction decisions and are not recorded.
`cursor_chunk` required exposing `layer_order`'s existing `last_fetched`
through a new read-only `policy_t.trace_cursor` interface member
(`lru` returns `CHUNK_NONE`, has no cursor concept) — **does not change
`select_victim`, `predict_next`, or `next_use_distance`'s outcome**, only
observes the existing decision after the fact. Smoke-tested standalone
before use (15/15 evictions captured correctly against a hand-checkable
small run).

Captured on cell 1 (non-degenerate, fully serial, deterministic) and
cell 5's exact configuration (degenerate), both with a paired reference
trace from the same run. Non-degenerate: 49 evictions. Degenerate: 66
evictions (a third and fourth independent degenerate capture, without a
reference trace, gave 60 and 69 evictions respectively — already
demonstrating victim-sequence-level non-determinism across nominally
identical degenerate reps, before even comparing against the baseline).

### Q1 — Same sequence of victim choices? Where's the first divergence?

**No. First divergence at eviction index 10 (the 11th eviction).**

| Index | Non-degenerate victim | Degenerate victim |
|---|---|---|
| 8 | 15 | 15 |
| 9 | 0 | 0 |
| **10** | **1** | **8** |
| 11 | 2 | 1 |
| 12 | 3 | 9 |
| 13 | 4 | 1 |

### Q2 — At the first divergence, what is the cursor, and what does the walk return?

**The resident set is identical at this exact point in both runs**
(`n_resident=8`, `bitmap_lo=32514` in both — the same 8 chunks are
resident). **The cursor differs**: non-degenerate `cursor=2`, degenerate
`cursor=0`. Non-degenerate's walk from cursor 2 returns chunk 1 at a
known distance of 15 (one short of the maximum possible in a 16-chunk
cycle — the chain has, by this point, learned nearly the whole loop).
Degenerate's walk from cursor 0 returns chunk 8 at `dist=UNKNOWN`
(`INT64_MAX` — "never reached by the walk").

**This is the direct mechanism.** Same data (identical resident set),
different cursor. The cursor is `last_fetched`, written only by
`on_fault()` — so the two runs dispatched their 11th real fault in a
different order relative to which chunks had already looped through
`on_fault` by that point, exactly as the source-code review predicted.
The victim decision itself (`select_victim`) is unchanged and
deterministic *given* the chain state; the chain state itself is what
varies, because **fault-dispatch order is not fixed by the reference
sequence under concurrent driver threads with real timing variance.**

### Q3 — Anti-optimal, or merely suboptimal?

Checked via a self-contained, trace-only measurement (no additional
instrumentation needed): for every `UNKNOWN`-distance victim (the walk
believed it was "infinitely far" / not yet reached), find how many
evictions later that same chunk is evicted **again** — a short gap means
it was actually needed again soon, contradicting the walk's belief.

| Run | Evictions | `UNKNOWN`-distance victims | ...of which re-evicted sooner than one full cycle (16 evictions) |
|---|---|---|---|
| Non-degenerate (baseline) | 49 | 28 (57%) | 19/22 measurable (86%) |
| Degenerate (A.3b) | 66 | 47 (71%) | 28/36 measurable (78%) |
| Degenerate rep (A.3, no reftrace) | 60 | 38 (63%) | 23/29 measurable (79%) |
| Degenerate rep (A.3, no reftrace) | 69 | 49 (71%) | 29/38 measurable (76%) |

**Nuanced result, not a clean "yes."** Conditional on a victim being
classified `UNKNOWN`, the rate at which it turns out to have been needed
again within one cycle is **similar across degenerate and non-degenerate
runs (76-86%)** — the degenerate runs are not choosing qualitatively
*worse* individual victims than the clean baseline does, given the same
kind of uncertainty. What differs is the **volume**: the degenerate runs
make substantially more `UNKNOWN`-distance decisions in total (63-71% of
all evictions vs. 57% in the deterministic baseline). The mechanism is
not "the walk gets tricked into confidently anti-optimal choices under
concurrency" — it is **"concurrency-driven fault-dispatch reordering
leaves the chain less complete more often, and each additional
`UNKNOWN`-distance decision carries the same ~80% chance of being wrong
that it always did."** More decisions made under the same per-decision
uncertainty, not worse decisions under the same uncertainty. Reported as
measured; not chased further into why the ~80% baseline rate itself is
that high (a property of `UNKNOWN`'s definition — "not yet reached by an
online-learned chain" — combined with this workload's bounded 16-chunk
cycle, not something this phase's instrumentation was built to isolate
further).

## A.4 — Scope check

Scanned every arm D cell in `results/data/synthetic-consolidated-sweep.csv` against
arm C at the same `(chunk_size, ratio, compute)`.

| Chunk | Ratio | Compute | C `read_bytes` | D `read_bytes` | D/C |
|---|---|---|---|---|---|
| 8MiB | 0.25 | 400000 | 10,737,418,240 | 12,280,922,112 | **1.1438** |
| 8MiB | 0.375 | 400000 | 10,737,418,240 | 10,678,697,984 | 0.9945 |
| 128MiB | 0.25 | 400000 | 10,737,418,240 | 13,555,990,528 | **1.2625** |
| 128MiB | 0.375 | 400000 | 10,737,418,240 | 11,945,377,792 | **1.1125** |
| 128MiB | 0.5 | 400000 | 10,737,418,240 | 10,737,418,240 | **1.0000** |

(All other 15 of 20 arm D cells in the grid have D/C ≤ 0.89 — clearly
below arm C, no approach to degeneration; full ratios reported in
`scratch/phaseA_scope_check.py`'s output.)

**This is not a one-off cell — it is the worst-visible end of a pattern
that is, in fact, more severe elsewhere in the grid than the single cell
Phase D happened to flag.** Every one of the 5 qualifying cells is at
`--compute-ns-per-mib 400000`; **zero** cells at `compute=0` come
anywhere close (the highest compute=0 ratio in the entire grid is 0.89,
at 128MiB/r=0.25). Two cells — **128MiB/r=0.25 (D reads 26% MORE than
C) and 8MiB/r=0.25 (14% more)** — are more severe than the originally
reported r=0.5 cell, and both **exceed** arm C, meaning arm D fetched
more total bytes than the always-fetch-everything baseline. Lower
ratios (r=0.25, r=0.375) are more affected than the originally-flagged
r=0.5, not less — the degeneration's severity does not track budget
tightness in the direction one might expect.

## Correctness

Re-ran T-1..T-7 after adding `--policy-trace` (`reconcile_interval=1`/
eager is already hard-coded into every T-1..T-7 test binary, per A-3 —
same situation as Campaign 12's own Correctness sections). All PASS:
T-1 (0 mismatches, all 3 policies), T-2 (0 mismatches, 999 evictions),
T-3 (0 mismatches, 60s storm), T-4 (exact `memory.stat[shmem]` match),
T-5 (belady selftest, 300/300 + exact floor checks), T-6
(`dedup_fetching=14114>0`), T-7 (all 8 storm threads joined inside the
120s watchdog). `record_policy_trace()` is a no-op guarded by
`if (!r->diag_policy_trace) return;` — none of T-1..T-7 pass
`--policy-trace`, so this exercises the "instrumentation present but
disabled" path, not just "instrumentation absent from the binary."

## Did not fix anything

Per explicit instruction. `select_victim`, `predict_next`,
`next_use_distance`, and the successor-chain update logic in `policy.c`
are byte-for-byte unchanged from Campaign 12. The only code changes are
purely observational: a new read-only `trace_cursor` accessor and the
`--policy-trace` recording calls, both confirmed not to alter any
decision (T-1..T-7 pass identically with and without the flag; A.2's
deterministic cells 1-4 remain deterministic with the instrumentation
built in but unused).

## What I did NOT test

- Whether the same fault-dispatch-order mechanism explains the retention
  direction reversal between items 10d and 10e — plausible per the
  campaign's own framing, not independently traced this phase (would
  require a similar `--policy-trace`-style capture of that specific
  historical comparison, out of this phase's scope).
- Chunk sizes/ratios beyond the 6 cells in A.2 and the 20-cell scan in
  A.4 — the grid was fixed by the campaign's instructions.
- A true Belady-hindsight (reference-trace-aligned) computation of each
  eviction's *exact* true-future-distance — Q3 used a self-contained,
  trace-only re-eviction-gap proxy instead, disclosed as a deliberate,
  tractable substitute, not a claim of full Belady-optimality analysis.
- Whether `--prefetch-depth > 1` or prefetch-on cells (arm E) show the
  same non-determinism — this phase traced arm D (prefetch off)
  exclusively, per the campaign's own cell definitions.

## Final check

- No number estimated or inferred — every value is a direct read from
  `results/data/policy-determinism-reproduction.csv`,
  `results/data/policy-determinism-6cell.csv`, direct computation over
  retained `--policy-trace`/reference-trace binary files
  (`scratch/analyze_c13_policytrace.py`,
  `scratch/analyze_c13_antioptimal.py`), or a direct scan of
  `results/data/synthetic-consolidated-sweep.csv`
  (`scratch/phaseA_scope_check.py`).
- No test was weakened; T-1..T-7 were re-run in full after the
  instrumentation change and all pass.
- The specific hypothesis was checked against measured values, not
  assumed: source-code review first (correcting the campaign's own
  cursor-timing framing), then A.1 (n=10, real variance shown), A.2
  (isolated the exact 3-factor combination required), A.3 (traced the
  literal divergence point and characterized, rather than assumed, the
  anti-optimality question — reporting a nuanced result, not forcing a
  clean yes/no where the data didn't support one).
- A.4's gate ("scan for other degenerate cells") was evaluated
  exhaustively across the full 20-cell arm D grid; results reported in
  full, including cells more severe than the one Phase D originally
  flagged, not just cells matching or below it.
- Nothing was fixed. `select_victim` and every policy decision function
  are unchanged; only observational instrumentation was added.
