# residctl — project memory

Application-authoritative page residency for large read-only working sets
(userfaultfd + memfd_create + cgroup v2), built on top of the feasibility spike
at `/root/spike/`. That spike answered "can this work at all" (yes — see
`/root/spike/results/SPIKE_REPORT.md`, `SPIKE_ADDENDUM.md`,
`SPIKE_ADDENDUM2.md`). This project answers "build the thing."

**This is the agent working file. The readable project narrative is
[`docs/project-log.md`](docs/project-log.md); the results are
[`results/findings.md`](results/findings.md).** Notes below the "Build order"
section predate the 2026-08 restructure and use pre-restructure paths.

## Source of truth

- **`docs/02-design.md`** — the implementation contract as it stands now:
  invariants I-1..I-10, the measured constants, the architecture, the policies,
  T-1..T-7. Code that violates an invariant is wrong even if it runs and even if
  it's faster. Read it before touching `src/`. The 14 amendments that got it
  there are in `docs/design-history.md`.
- **`/root/spike/results/`** — where every number in `docs/02-design.md` §2 came
  from. Do not re-derive them; if a number is needed that isn't there, measure
  it and record where.

## Rules carried forward from the spike (still binding)

- Never fabricate, estimate, or extrapolate a number. If untested, say
  "NOT MEASURED."
- Never weaken a test to make it pass. Report failures with the exact
  error/errno/command.
- Never conclude success from absence of a crash — check the stated pass
  criteria (T-1..T-5 in MECHANISM_SPEC §13, per-arm criteria in §11).
- Don't silently fix the environment. No kernel rebuilds, no package installs,
  no .wslconfig edits. Report what's missing and stop.
- Machine exclusivity during any measurement run: check for other workloads
  before and after (the spike was contaminated by exactly this once). If found,
  don't kill it — stop and report.
- Time-box; if exceeded, stop, record partial results, move on.

## Build order (fixed, do not reorder — see MECHANISM_SPEC §12)

| # | Item | Est. | Status |
|---|---|---|---|
| 1 | Region setup + startup assertions (§4) | 2h | done |
| 2 | Handler loop + chunk table + state machine (§3, §5) | 4h | done (see note) |
| 3 | Fetch path with alignment (§6.1–6.2) | 3h | done, built together with item 2 |
| 4 | Eviction + budget + reconcile (§7) | 2h | done (temporary victim selector, see note) |
| 5 | Trace recorder + metrics (§9) | 2h | done |
| 6 | Trace-replay driver | 2h | done |
| 7 | `lru` and `layer_order` policies (§8) | 2h | done |
| 8 | Prefetch (§6.3) | 1h | done (inline, not a separate thread -- see note) |
| 9 | Belady solver (§10) | 2h | done (spec's own sanity formula turned out wrong -- see note) |
| 10 | Harness, arms, sweep (§11) | 3h | V1 SUPERSEDED (3 defects, see note below) -- corrected, see experiments/03-corrected-harness.md |
| 11 | llama.cpp integration (separate spec, stretch) | 7h | out of scope for now |

The replay driver (item 6) comes before any engine integration so there is
always a complete, runnable result even if integration runs long.

**Deviation from the fixed order, disclosed rather than silently done:**
items 2 and 3 were implemented and committed together. §5's handle_absent()
pseudocode calls fetch_chunk() directly, so the handler loop cannot be
tested in isolation from the fetch path -- there's no meaningful "state
machine works" checkpoint that doesn't also exercise §6. Everything else in
the fixed order stays separate.

**Known gap in item 2/3's own test** (`test_pager.c`): the dedup branches in
`handle_fault()` (I-8's RESIDENT/FETCHING cases) are implemented per spec and
believed correct by inspection, but a deliberate barrier-synchronized,
8-thread race onto a single cold chunk produced exactly one uffd message in
5/5 observed runs, not several -- the race window wasn't hit. This is
reported, not silently fixed by adding a test-only delay hook into
production fetch code. §13's T-3 (sustained concurrent storm with real
eviction cycling, once item 4 exists) is the right place to actually confirm
this path fires.

**§13 correctness harness (T-1..T-5) -- run and PASSED before item 10's
performance numbers were trusted, per the spec's own ordering.** T-1 (full
16 MiB region read through mapping A under a 25% budget, every 4096-byte
sample checked, all three policies): 0 mismatches. T-2 (1000-iteration
ping-pong punch-refetch cycle): 0 mismatches, 999 evictions as expected.
T-3 (8 threads, tight 2-chunk budget, 60 real seconds, `layer_order` +
prefetch under contention): 104,314 touches, 0 mismatches, resident_bytes
never exceeded budget, no hang, no abort -- ran under `timeout` deliberately
so a real hang fails loud. T-4 (arbitrary 15-touch non-monotonic sequence):
`memory.stat[shmem]` matched `resident_bytes + known_overhead` EXACTLY (not
just within reconcile()'s one-chunk operational tolerance). T-5 is item 9's
`belady_main --selftest`. **Notable non-finding:** even T-3's 60s of heavy
8-thread contention produced `dedup_resident=0, dedup_fetching=0` -- the
same gap items 2 and 6 already disclosed (the RESIDENT/FETCHING dedup
branches in `handle_fault()` have still never been observed to fire) is now
backed by much stronger negative evidence and is being accepted as a known,
understood limitation for v1 rather than chased further.

**CAMPAIGN 11 — five-phase closing session. Reports:
`experiments/07b-existing-data-analysis.md`, `experiments/07c-platform-io-microbenchmark.md`,
`experiments/08-compute-phase.md`, `experiments/09-chunk-size-sweep.md`,
`experiments/09b-consolidated-6arm-sweep.md`, and `results/PROJECT_STATE.md` (the
consolidated reference document — headline results, open questions,
limitations, spec amendments A-1..A-11, and a superseded-results list,
all in one place). Explicit brief: run every phase, decide nothing,
report measurements only. No spec amendment produced (findings are
measurements/reversals, not fixes).**

Phase 0 (no new runs): arm E (prefetch on) DOES show real fetch overlap
(median 1-3, unlike arm D's flat 1.00) — item 10e's null result was
arm-D-specific. Item 10e's dramatic gate-run wall-clock improvement does
not reproduce in Sweep B's own data — a `policy=default` vs
`policy=layer_order` confound in the gate script, not a real effect.
Independently re-confirmed the fetch-path lock inventory (per-chunk lock
only, no global lock, at every `pread()` call site).

Phase 1 (platform microbenchmark, no pager): neither "scales" nor
"serialises" holds cleanly for raw `O_DIRECT` concurrency — aggregate
throughput rises 1→2 threads then plateaus at a ~3400-4300 MiB/s ceiling
through 8 threads. `fd-per-thread` does not lift the ceiling over
`shared-fd`, ruling out item 10e's single-`model_fd` hypothesis directly.

Phase 2 (calibrated compute phase added to the driver,
`--compute-ns-per-mib`): STOP-AND-REPORT gate passed. Prefetch hit rate
FELL with heavier compute in 14/18 series — the opposite of the
pre-registered hypothesis. Arm E beat arm D's `read_bytes` at r=0.5 and
r=0.75 under heavy compute — the first time in this project's history.
Found and investigated (not fully resolved) a 2-3× calibration
achieved-vs-requested discrepancy, most likely CPU/cache contention among
concurrent compute threads. Hit a real machine-exclusivity contamination
(a Gradle daemon) mid-sweep; per instruction, did not kill it, discarded
ambiguously-contaminated partial results, restarted clean.

Phase 3 (chunk-size sweep, 32/64/128/256 MiB): OPT ≥ floor gate passed at
all 12 combinations. Arm D's bytes and wall-clock are BOTH monotonically
increasing with chunk size at every ratio — minimum is always the
smallest size tested (32 MiB), not 128 MiB. Handler-overhead fraction did
NOT shrink at larger chunk sizes as predicted. Arm A's `read_bytes` read
exactly 0 in 11/12 cells — the WSL2 host-cache confound, reported as
NOT MEASURABLE rather than estimated.

Phase 4 (consolidated 6-arm sweep, 5 ratios × 2 compute levels): OPT ≤ D
held at all 10 cells. E beat D's `read_bytes` at exactly r=0.5 and
r=0.75, only under heavy compute — independently replicating Phase 2's
finding on a separate grid. `lru` still thrashes at 100% miss everywhere.
Backfilled an omitted `--fetch-trace` flag (disclosed oversight, not
hidden) with a labelled n=1 supplementary pass.

Phase 5: `results/PROJECT_STATE.md` — see that file directly rather than
this summary for the full consolidated headline-result list, open
questions, limitations, spec-amendment index, and superseded-results
table.

**ITEM 10e — lookahead window driver + retention regime boundary. Full
report: `experiments/07-lookahead-window.md`.** Task A fixes a spec defect
introduced in item 10d (A-8's hard barrier made async's throughput benefit
untestable by construction — item 10d's own report diagnosed this
correctly but didn't yet have the fix). Task B follows up item 10d's Task
C finding that retention's effect was regime-dependent (bytes fell in some
cells, rose in others, tracking `stat_pin_broken`) rather than a clean
"did not hold." Nothing from item 10d is retracted — its Task A
verification gate, `dedup_fetching` scaling, and retention numbers all
still stand as measurements of the barrier-driven driver they were taken
under.

**Task A (A-10, supersedes A-8's barrier) — bounded lookahead window.**
`--lookahead-window W` (default 0) replaces the `pthread_barrier_t` with a
global-step completion counter (condition-variable-gated, not a weakened
approximation): thread *t* may begin chunk *i+1* once ALL threads have
completed chunk *(i−W)*, provably bounding concurrent chunks in flight to
`W+1` (proof in `replay.c`'s `lookahead_wait_to_start()` comment, from
each thread's own strictly sequential per-step progression). Verification
gate (`W` ∈ {0,1,2}, `--driver-threads 8`, ratio 0.5): reference traces
identical across all three (80/80 records, `seq`/`chunk_id` match),
`bytes_touched` identical (10,737,418,240), OPT identical
(`minimum_misses=48`, `minimum_bytes_fetched=6,442,450,944`) — **PASS on
all three**. `W=0` reproduces item 10d's own gate numbers EXACTLY
(`pager_bytes_fetched=6,979,321,856 absent_handled=52 evictions=44`) —
the required regression check, confirmed before proceeding to Task B.
`objdump` on the new `replay_thread_main` confirms the read loop and its
atomic sink-add (`lock add`) both survive `-O2`.

**Task B — sync vs async with overlap actually possible.** Arm D, V2
scale, 3 ratios × n=3 × `--lookahead-window` {0,1,2} × `--driver-threads`
{1,8} × handler {sync, async}, 108 runs, `results/data/historical/task-e-sweep-b.csv`. Of
6 pre-registered expectations: **1 and 5 held; 6 held after investigation
of an apparent violation; 3 held trivially; 2 and 4 did not hold as
intended.** Expectation 1 (W=0 regression) holds exactly at all 12
(ratio, threads, handler) combinations checked. Expectation 6 (`read_bytes`
identical per ratio) initially looked violated (real variance appeared for
the first time on arm D, e.g. r=0.25 spanning 9.26G–10.47G) — investigated
per the spec's own stop-and-report instruction, not waved through: zero
`DISCREPANCY`/`RECONCILE FAILED`/`FAIL` lines in the log, every value an
exact integer multiple of one chunk-size apart from its neighbors, and
critically **zero variance at `W=0` specifically** (exactly one value,
matching item 10d), with variance growing monotonically with `W` (3
distinct values at W=1, 5 at W=2) — traced to genuine timing-dependent
eviction ordering from real cross-chunk concurrency (the same class of
non-determinism item 10d's Anomalies section already disclosed for
prefetch-enabled runs, now appearing on arm D itself for the first time,
precisely because A-10 is what finally introduced real overlap to a
previously fully-serialized driver). Not a bug; reported as investigated
and cleared.

The headline finding: **`stat_dedup_fetching` DID change with the window
(confirming real cross-chunk contention now exists — e.g. r=0.25/threads=8/
async: 379→125→136 medians at W=0/1/2, materially different fault-dedup
behavior from a flat barrier), but the median number of CONCURRENTLY-
OUTSTANDING FETCHES (computed directly from `--fetch-trace` read-interval
overlap, new this item) was exactly 1.00 in EVERY one of the 36 (ratio,
window, threads, handler) cells — including W=2/threads=8/async.**
Expectation 2 (concurrently-outstanding rises with W under async, toward
2 at W=1 and 3 at W=2) **did not hold — not partially, not at any cell.**
Expectation 3 (sync stays ~1.0) holds, but non-distinguishingly: async
ALSO stayed at exactly 1.0 throughout, so this isn't evidence of a
handler-specific limitation. Device-busy fraction showed only a small,
secondary rise with `W` under async at threads=8 (e.g. r=0.75: 0.834→
0.843→0.851) — consistent with reduced dispatch-gap idle time between
back-to-back fetches, not genuine I/O overlap (which concurrently-
outstanding=1.0 rules out) — so expectation 4 did not hold as intended
even though the raw busy-fraction number ticked up slightly. Wall-clock
at W=2/threads=8 (expectation 5) held at 2 of 3 ratios (r=0.25:
4.556s<4.642s; r=0.5: 3.931s<4.048s; r=0.75: 2.732s vs 2.713s, marginally
higher). `fetch.c`'s `fetch_chunk()` was inspected as part of this finding
(no lock of its own; a single, process-wide `r->model_fd` is shared by
every fetch-pool worker, `pread()`-per-worker with explicit offsets) —
consistent with, but not proof of, serialization somewhere below the
dispatch layer (the WSL2 virtualized disk backend and/or `O_DIRECT`
behavior on this specific filesystem are plausible candidates; not
isolated further, per the spec's explicit instruction not to chase a
further mechanism when the device ceiling looks genuinely hard).

**Task C — retention regime boundary, five-ratio sweep
(0.25/0.375/0.5/0.625/0.75), arm E, `--prefetch-retention` {none,pinned} ×
depth {2,4}, n=3, async, `--fetch-workers 4`, `--driver-threads 8`,
`--lookahead-window 1`, 75 runs, `results/data/historical/task-e-sweep-c.csv`.** Of 4
pre-registered expectations: **3 held (9/10 cells, one exact tie); 1
held with a depth-dependent boundary rather than one project-wide `r_c`;
4 mostly held with one small exception; 2 did NOT hold — a genuine
reversal from item 10d, reported as such.**

`stat_pin_broken` cleanly separates two regimes, but the boundary is
**depth-dependent**: nonzero at r=0.25 for both depths, ALSO nonzero at
r=0.375 for depth=4 specifically, zero everywhere else. So
`r_c(depth=2)` falls between 0.25 and 0.375; `r_c(depth=4)` falls between
0.375 and 0.5 — a deeper retained set needs more budget headroom before
it stops contending with demand, which is the mechanism, not an
anomaly. `infeasible` tracks the same depth-dependent boundary almost
exactly (large counts, 19-44, at the cells where `pin_broken` fires at
depth 4; one small exception — `infeasible=[0,1,1]` also appeared at
r=0.25/depth=2, where `pin_broken` was firing too, so the "only at depth
4" half of expectation 4 has one minor, disclosed exception, not a clean
zero).

**The reversal:** under item 10d's barrier-driven sweep, `pinned`'s
`read_bytes` fell below `none`'s at ratios where `pin_broken` was 0 (the
regime claim this item set out to confirm). Under real overlap
(`--lookahead-window 1`), `pinned` read MORE bytes than `none` at every
single (ratio, depth) cell tested except one exact tie (r=0.25/depth=2)
— including at r=0.5/depth=2 and r=0.75/depth=4, the exact two cells item
10d's report highlighted as retention "nearly eliminating prefetch's byte
penalty." Verified not a bug (zero `DISCREPANCY`/`FAIL`/`RECONCILE
FAILED` lines across all 75 runs) before reporting it as a real finding.
Hit rate under `pinned` still exceeded `none` at 9 of 10 cells (one exact
tie at r=0.375/depth=2, 0.25=0.25) — retention still measurably avoids
real faults under real concurrency; it does not follow that it reduces
total bytes moved once genuine cross-chunk overlap exists. Not chased
into a further retention mechanism, per instruction — reported as the
finding and stopped.

**Concurrency bugs found, consolidated across items 10b-10e (per this
item's own framing note — six bugs, one list, not scattered across four
reports):**
1. **Item 10b — false-positive `RECONCILE FAILED`.** In-flight `pwrite()`s
   populated real shmem before `commit_reserved()` ran; fixed by widening
   `reconcile()`'s check to a range
   `[resident_bytes, resident_bytes+reserved_bytes]`.
2. **Item 10b — deadlock from marking `RESIDENT` before the budget
   reservation committed.** Made the chunk look evictable to a concurrent
   `ensure_budget()` while still holding the chunk's own lock; fixed by
   reordering commit-before-`RESIDENT` at all three fetch call sites.
3. **Item 10b — flaky deadlock from pinning one step too late relative to
   becoming visible as `RESIDENT`.** Fixed via `commit_reserved_and_pin()`,
   landing the pin atomically with the commit under one `budget_lock`
   critical section.
4. **Item 10c — unlocked `latency_hist_record()`.** Only the single
   synchronous handler thread ever called it before item 10c's async fetch
   pool made it genuinely concurrent; fixed with an internal mutex on
   `latency_hist_t`.
5. **Item 10c — unlocked `region_t` stat counters** (`stat_bytes_fetched`,
   `stat_absent_handled`, `stat_prefetches`, `stat_prefetch_infeasible`) in
   `prefetch_pool.c`'s worker functions; fixed with `__sync_fetch_and_add`.
6. **Item 10d — three unlocked `target->pin++`/`pin--` operations in
   `prefetch_pool.c`'s `do_one_prefetch()`.** Harmless at item 10b's
   original prefetch-only-pool scale, a real race once item 10c made
   multiple prefetch workers run concurrently by default; fixed with the
   existing `unpin_chunk()` and a new, symmetric `pin_chunk()`, both under
   `budget_lock`.

All six are the same class: **shared mutable state reached from a code
path that was previously reachable from only one thread at a time**, made
live by an architecture change elsewhere in the same or a later item (not
by the fix that found them). Three of the six (4, 5, 6) were unreachable
before item 10c's async architecture existed at all. All six were found
by code inspection before a test failure revealed them, not by
reproduction of an observed hang or corruption (unlike 1-3, which item
10b found via actual reproduced deadlocks). Item 10e itself introduced no
new bugs of this class — Task A's lookahead window was built with an
explicit boundedness proof (not just tested for absence of hangs) and
Task C added no new code (pure sweep over item 10d's existing retention
mechanism); 183 sweep runs plus the gate's 3 runs, zero hangs, zero
`DISCREPANCY`/`FAIL`/`RECONCILE FAILED` lines.

**ITEM 10d — concurrent demand workload + prefetch target retention. Full
report: `experiments/06-concurrent-demand.md`.** Item 10c produced two null
results, both correctly explained, and both explanations pointed at a
specific unrun experiment. This item runs them. Nothing from item 10c is
retracted — T-6's `dedup_fetching=14494` still stands as proof the async
handler works; this item makes that result measurable on the benchmark
arms rather than only in the correctness harness.

**Task A (A-8) — multi-threaded replay driver.** Added
`--driver-threads N` (default 1, `replay_cyclic()` unchanged byte-for-byte)
and `replay_cyclic_mt()`: N threads execute the SAME reference sequence,
partitioned WITHIN each chunk's page range (disjoint page stride per
thread) with a barrier at every chunk boundary — models parallel compute
over one layer, then advancing together, not N independent workers racing
across chunks. Verification gate (`--driver-threads` 1 vs 8, same
parameters): reference traces field-identical (excluding the unavoidably
non-deterministic wall-clock `timestamp_ns` — belady_main itself never
reads that field either), total bytes read identical
(`pager_bytes_fetched`, `absent_handled`, `evictions` all exactly equal),
OPT identical (`minimum_misses=48`, `minimum_bytes_fetched=6442450944`
both traces) — **PASS on all three**, confirmed via `objdump` that the
atomic-accumulated read loop survives `-O2` (`lock add` instruction
present). Also wired in Task C's `pager_notify_access()` "consumed" call
(see below) — doesn't affect the gate since it only touches internal pin
bookkeeping, never the sink/bytes/trace.

**Task B (no spec amendment needed — uses A-8's driver) — sync vs async
under real concurrency.** Arm D (`layer_order`, prefetch off), V2 scale, 3
ratios × n=3 × `--driver-threads` {1,2,4,8} × handler {sync, async
`--fetch-workers 4`}, 72 runs, `results/data/historical/task-d-sweep-b.csv`. Of 5
pre-registered expectations: **1, 2, and 5 held; 3 and 4 did not.**
`stat_dedup_fetching` is exactly 0 at `--driver-threads 1` in both handler
modes and under `--sync-handler` at every thread count, and rises steeply
with thread count under async (e.g. r=0.25: 0→66→201→428 medians at
threads 1/2/4/8) — expectation 1 holds cleanly. Device-busy under sync
stays flat (0.808-0.864 across the whole grid) — expectation 2 holds.
**Device-busy under async does NOT rise with driver threads** (also
0.808-0.864, statistically indistinguishable from sync at matching
cells) — expectation 3 does not hold, reported as the plain negative
result the spec required, with the mechanism traced (not hand-waved): the
barrier structure A-8 requires means all N driver threads are always on
the SAME chunk at once, so more threads create more concurrent demand for
that ONE chunk (exactly what dedup_fetching measures) but never concurrent
demand for DIFFERENT chunks, which is what the fetch-worker pool would
need to actually overlap I/O. Wall-clock at 8 threads was NOT
consistently lower under async (expectation 4): true at r=0.5
(3.646s<3.701s) but false at r=0.25 (4.434s>4.421s) and r=0.75
(3.144s>2.685s) — consistent with expectation 3's finding: with no extra
I/O overlap to gain, async's added dispatch/enqueue/dequeue hop (median
13-18µs, newly measurable via extended `--fetch-trace` fields) is a small
net cost, not a win, on this specific arm. `read_bytes` was **exactly
identical across all 24 cells at every ratio** — expectation 5 (the
correctness check) holds with zero exceptions.

**Task C (A-9) — prefetch target retention.** `budget.c` gained
`prefetch_retain_on_resident()`/`prefetch_retain_release()`/
`pin_break_select_victim()`, and `region_t` a bounded FIFO
(`pinned_prefetch_queue`, capacity `prefetch_depth`). A prefetch target's
existing fetch-time pin is now KEPT (not released) on becoming RESIDENT
under the new default `--prefetch-retention pinned`, until either
`pager_notify_access()` (pager.h; called by the workload right before it
reads a chunk, hit or miss — the only way to observe "consumed" given a
resident-chunk touch generates no uffd event at all) or the FIFO cap
evicts the oldest entry first. `ensure_budget()`'s eviction loop (demand
fetches only, never `ensure_budget_prefetch()`) now breaks the COLDEST
pinned target's pin rather than going infeasible when no unpinned victim
exists, counted as `stat_pin_broken` — "a speculative chunk must never
starve a real one." Found and fixed a third latent unlocked-pin race while
touching this code (matching item 10c's two): `prefetch_pool.c`'s
`do_one_prefetch()` had THREE bare `target->pin++`/`pin--` with no lock at
all (pre-fetch pin, infeasible-abandon unpin, post-fetch unpin) — harmless
at item 10b's original scale, a real race once multiple prefetch workers
run concurrently (item 10c). Fixed with the existing `pin_chunk()`
(new)/`unpin_chunk()` helpers, same pattern as items 10b/10c's fixes.

**Correctness gate (T-1..T-7, `--eager-reconcile`, unmodified test
binaries picking up the new default automatically):** all PASS. T-3's
`infeasible` count (27314) is statistically unchanged from the
pre-Task-C baseline (27036, same test, prior commit) — the huge count is
a pre-existing property of the bounded 200×2ms retry loop under this
specific 2-chunk-budget storm, not a regression from retention. T-6/T-7
both clean (0 mismatches, all 8 threads joined within T-7's 120s
watchdog) even with `prefetch_depth` defaulting to 1 chunk of the 2-chunk
budget permanently retainable — a real, meaningful stress test of the
pin-break override under contention, not just a smoke test.

**Sweep C:** arm E, `--prefetch-retention` {none,pinned} × 3 ratios ×
depth {2,4} × n=3, `--driver-threads 8`, `--fetch-workers 4`, 36 runs,
`results/data/historical/task-d-sweep-c.csv`. Of 4 pre-registered expectations: **3
held; 1 partially held; 2 and 4 did not.** Hit rate rose under `pinned`
relative to `none` at **12/12** (ratio, depth) reps compared directly
within this sweep (e.g. r=0.75/d4: 0.32→0.46; r=0.5/d2: 0.11→0.27), and
clearly exceeded item 10b's original 14-27% ceiling at 2 of 6 (ratio,
depth) cells (r=0.75/d2: 0.32, r=0.75/d4: 0.46) — expectation 1 is a
genuine, disclosed partial hold, not glossed into a clean win: at the two
tighter ratios (0.25, 0.5) `pinned`'s hit rate mostly lands at or just
inside the old band (0.23-0.27), not above it. `read_bytes` did NOT
consistently fall (fell in 3/6 (ratio,depth) cells, rose in 3/6) and arm E
still exceeds arm D's read_bytes at r=0.25 under `pinned` at every depth
(12.2G/14.4G vs D's 9.26G) — expectation 2 does not hold, the second
attempt (after item 10c's admission rule) at the same outcome via a
different mechanism, reported as such. `stat_pin_broken` is non-zero at
r=0.25 as predicted (depth=2: 4-9 per run; depth=4: 23-29 per run,
alongside a real infeasible count of 38-44 — the tightest cell genuinely
strains the budget) and exactly zero at r=0.5/r=0.75 (never needed) —
expectation 3 holds cleanly. Demand fault count did NOT consistently fall
(fell in 2/6 cells, rose in 4/6) — expectation 4 does not hold.

Two process notes: both Sweep B (72 runs) and Sweep C (36 runs) completed
within a single background-task invocation each (no 10-minute cap hit this
time, unlike item 10c's Sweep 2/3); machine exclusivity checked clean
before and after both.

**ITEM 10c Task A — async dispatch-only handler (in progress; Task B/C and
`experiments/05-async-handler.md` not yet done as of this entry).** Direct
consequence of item 10b Task C's finding: `dedup_fetching` wasn't just
empirically zero, it was *structurally* unreachable under the synchronous
handler, which is a spec defect, not a coverage gap. Spec amendments A-5
(§5, `docs/design-history.md`) and A-7 (§4 step 10) applied: the handler
thread now only dispatches (lookup, lock, state-machine decision, enqueue);
`prefetch_pool.c` (item 10b Task B's prefetch-only pool) is generalized into
a single shared fetch pool for BOTH demand and speculative fetches, with
demand strictly prioritized in the queue (a worker always drains the demand
queue before the prefetch queue). `pager_run()` owns the pool's lifecycle
itself when `async_handler` is true (the new default) — it starts
`fetch_workers` (default 4, `--fetch-workers N`) workers at the top and
stops them at the bottom, so every existing test/driver that calls
`pager_run()` gets the async path for free without needing its own changes.
`--sync-handler` restores the pre-10c synchronous path byte-for-byte
(verified: `test_pager`, `test_eviction`, `test_prefetch` etc. did not need
any changes and still pass under either path).

Two bugs found and fixed while wiring this up, not discovered by test
failure but by inspection before anything ran:
1. **`latency_hist_record()` was never thread-safe.** Under the old design
   only the single synchronous handler thread ever called it. Once fetch
   workers call it concurrently (item 10c: every demand fetch, from
   potentially `fetch_workers` threads at once), the un-locked
   bucket/count/sum/min/max updates would race. Fixed by adding an internal
   mutex to `latency_hist_t` (`metrics.h`/`.c`) — callers don't need to know
   it became concurrent.
2. **Several `region_t` stat counters (`stat_bytes_fetched`,
   `stat_absent_handled`, `stat_prefetches`, `stat_prefetch_infeasible`)
   were incremented with plain `++`/`+=` inside `prefetch_pool.c`'s worker
   functions.** This was already a latent race in item 10b's
   prefetch-only pool at `--prefetch-depth > 1` (never caught — small
   counters losing an occasional increment doesn't crash or fail an
   assertion, it just silently under-reports, exactly the "silently wrong
   result" class of bug this project's rules exist to catch), and item
   10c's much higher fetch concurrency (`fetch_workers` demand workers, not
   just `prefetch_depth` prefetch workers) makes it far more likely to
   actually corrupt a count. Fixed with `__sync_fetch_and_add`, matching
   the pattern already used for `fault_seq`.

**Correctness gate, run before Task B per the spec's explicit instruction:**
§13 T-1..T-5 re-run via `run_correctness_harness.sh` (unmodified from item
10b) — all still PASS under the new async default (test binaries don't set
`--sync-handler`, so they now exercise the async path; T-1/T-2/T-4 exact,
T-3 no hangs/mismatches, T-5 unaffected). Notably, T-3's own (unmodified,
per instructions) counters now read `dedup_resident=21644 dedup_fetching=13793`
on a 60s run — a direct, incidental confirmation that the dedup branches
fire routinely under the new architecture, where they were 0/0 every single
time under the old one (item 2 through item 10b, 6+ independent
confirmations of the old zero). Two NEW tests per the item 10c spec:
- **T-6** (`test_t6.c`): same load shape as T-3, explicit gate
  `stat_dedup_fetching > 0`. PASS: `dedup_resident=22498 dedup_fetching=13808`
  on a clean 60s run, 0 mismatches, resident_bytes never exceeded budget.
- **T-7** (`test_t7.c`): same storm, plus a 120s internal watchdog thread
  that dumps `/proc/PID/task/*/wchan` and `/status` for every thread and
  hard-exits nonzero if joining the storm threads after stop doesn't
  complete in time (the same live-hang diagnostic technique used to
  originally find item 10b's two deadlocks, now built into the test itself
  rather than applied by hand). PASS: all 8 threads joined well within the
  watchdog, 0 mismatches, `dedup_fetching=13987`.

Both scripts run via `run_t6_t7.sh`, same `fresh_cgroup`/`cleanup` pattern as
the rest of the correctness harness. Full log: `experiments/logs/t6_t7_log.txt`.

**ITEM 10c Task B — prefetch admission rule (A-6).** `policy_t` gained
`next_use_distance(region, chunk)` (§8): `layer_order`'s
`layer_order_select_victim` and the new `layer_order_next_use_distance`
both call one shared `layer_order_compute_dist()` walk (refactored out of
`select_victim`'s body, not reimplemented) so the two can never silently
disagree about what "next use" means; `lru` returns `INT64_MAX`
unconditionally, matching the spec's explicit requirement (moot in
practice since `lru`'s `predict_next` is always -1, so nothing ever calls
into the admission path for it). New `budget.c` function
`ensure_budget_prefetch()` mirrors `ensure_budget()`'s reconcile/evict/
reserve loop but checks each candidate eviction with `prefetch_admit()`'s
distance guard first (§6.3 pseudocode) before allowing it — decline (not
force) if the victim isn't strictly colder than the prefetch's own target.
Wired into both prefetch call sites: `prefetch_pool.c`'s `do_one_prefetch`
(used by the async default at every depth, and by `--sync-handler` at
depth>1) and `prefetch.c`'s `maybe_prefetch` (the `--sync-handler`
depth==1 inline path). Demand fetches are untouched — they still call the
unconditional `ensure_budget()`. `--prefetch-admission {always,guarded}`,
default `guarded`; new `stat_prefetch_declined` counter, reported in
`replay_main`'s summary and `ARM_CSV` line.

Regression: full §13 T-1..T-7 re-run clean under the new `guarded` default
(all existing test binaries pick it up automatically, same as async did for
Task A). Visible, expected effect even on old tests that were never touched:
T-3's `prefetches` count dropped from ~12000 to ~8300 on the same 60s storm
— the admission rule is declining roughly a third of what would have been
forced evictions, exactly the kind of measurable change item 10b's 14-27%
hit-rate finding predicted was needed. `test_prefetch` (item 8) and
`test_pager` (items 2/3) still pass unmodified; `test_pager`'s own dedup
counters, which item 2 could never get to fire (see the item 2/3 "known
gap" note above) now read `dedup_fetching=7` on the same barrier-release
scenario under the new async default — a second, independent confirmation
(beyond T-3/T-6/T-7) that the item 10c Task A fix generalizes beyond the
one storm test it was built against.

**ITEM 10c Task C — three re-run sweeps. Full report:
`experiments/05-async-handler.md`.** Sweep 1 (arm D, sync vs async at
`--fetch-workers` 1/2/4): reproduced item 10b's device-busy numbers exactly
under `--sync-handler` (0.831/0.826/0.859 vs item 10b's 0.82-0.86 band);
device-busy did NOT rise above that band at any worker count, explained
(not just observed) by arm D being single-threaded with prefetch off, so
there is structurally never more than one outstanding demand fault for
extra workers to overlap with. Sweep 2 (full 6-arm sweep, async default,
`--fetch-workers 4 --prefetch-depth 2 --prefetch-admission guarded`):
OPT≤D and E-never-beats-OPT still hold at every ratio (unchanged from V2),
but E does NOT beat D at any ratio here (worse at r=0.25/0.5, tied at
r=0.75) — contradicts one of item 10c's own pre-registered expectations.
Sweep 3 (arm E, `guarded` vs `always` admission, depth 2/4): the two
admission modes are statistically indistinguishable at every one of 12
cells — `stat_prefetch_declined=0` everywhere despite 30-100+ evictions
per run. Traced to a real mechanism, not a bug: `layer_order_select_victim`
already picks the single farthest resident chunk, which by construction is
essentially always farther than a prefetch's own near-term target, so the
admission guard is close to redundant with what the policy's existing
victim selection already guarantees for this workload. All 4 of Task B's
pre-registered expectations failed to hold, reported as such with the
mechanism traced, not glossed over.

Two genuine process notes, both handled and disclosed rather than hidden:
(1) both Sweep 2 and Sweep 3 exceeded the harness's 10-minute
background-task cap and were killed mid-run; both were resumed from a
verified exact point (row counts checked against the CSV before resuming),
appending rather than restarting. (2) a system `updatedb.plocate` indexing
job overlapped with the first attempt at Sweep 2's ratio=0.75 arm E cell —
caught by the post-run exclusivity check (a `pgrep -x` false negative
delayed catching it by one check), waited out properly via `pgrep -f`, and
that one cell was redone cleanly after removing the contaminated rows.

T-1 through T-7 all re-run clean under every configuration exercised in
this phase. `experiments/05-async-handler.md` has the full sensitivity tables,
mechanism writeups, and final check.

**ITEM 10b — I/O pipelining diagnostic, prefetch depth, dedup instrumentation
(diagnosis on top of accepted V2, not a correction).** Full report:
`experiments/04-io-pipelining-diagnostic.md`.
- **Task A**: added `--fetch-trace` (per-fetch timing, in-memory,
  flushed once at exit). Arm D is **bandwidth-limited, not
  serialization-limited**: device-busy fraction 0.82-0.86, per-fetch
  bandwidth within 85-95% of the spike's O_DIRECT median, inter-fetch dead
  time negligible (<1%). This directly corrects V2's stated cause for the
  arm-A-vs-D wall-clock gap ("per-fetch overhead," which the spike's own
  numbers already ruled out): arm A's V2 throughput (4785 MiB/s) exceeded
  even the spike's measured *maximum* O_DIRECT bandwidth (3396 MiB/s),
  consistent with arm A having been served by the Windows VHDX host cache
  (unreachable by guest `drop_caches`), not genuine device I/O. Also found
  a real, secondary ~14% handler-overhead contribution, traced precisely to
  `reconcile()` running on nearly every fetch via the eviction trigger
  (A-3's periodic amortization helps less when budget is tight enough that
  most fetches also evict).
- **Task B**: added `--prefetch-depth N` (default 1 = item 8's original
  inline path, byte-for-byte unchanged) and a fixed N-worker pool for N>1
  (`prefetch_pool.c` — the only place multi-threading enters the pager).
  **Found and fixed three real concurrency bugs via reproduction, not in
  advance**: (1) a false-positive `RECONCILE FAILED` from in-flight
  `pwrite()`s populating real shmem before `commit_reserved()` runs --
  fixed by widening reconcile's check to a range
  `[resident_bytes, resident_bytes+reserved_bytes]`; (2) a deadlock
  (100% reproducible at depth 8 before the fix) from marking a chunk
  RESIDENT before committing its reservation -- fixed by reordering
  commit-before-RESIDENT in all three fetch call sites; (3) a second, flaky
  deadlock from pinning the just-resident chunk one step too late relative
  to when it became visible as RESIDENT -- fixed via
  `commit_reserved_and_pin()`, landing the pin atomically with the commit
  under one `budget_lock` critical section, plus a bounded retry (200x2ms)
  for real fetches that go infeasible specifically due to prefetch
  reservations (not real residents) -- unreachable at depth==1. All fixes
  verified via 40+ stress iterations at the worst-case depth with zero
  hangs, plus the full existing regression suite (items 1-9, §13) passing
  unchanged, including item 8's exact historical numbers byte-for-byte at
  depth 1.
  Sweep result (depth 1/2/4/8 x 3 ratios x n=3): expectations 2 and 3 held
  (read_bytes/infeasible worsen with depth at tight budget; device-busy
  fraction rises with depth at r=0.25/0.5, flat at r=0.75); expectations 1
  and 4 did NOT cleanly hold (wall-clock vs depth is non-monotonic at
  r=0.75, and the gap to arm A closes only about half-way at best) --
  directly explained by Task A's bandwidth-limited finding: with a single
  fetch already near the device ceiling, added concurrency has little idle
  capacity left to fill. Reported as such, not summarized into "prefetch
  helps."
- **Task C**: dedup counters (existing since item 2) still read 0/0 on an
  unmodified T-3 (5th independent confirmation). `dedup_fetching` is now
  **provably unreachable** (source-level proof, not just empirical): the
  handler drains uffd messages strictly sequentially, one fully processed
  before the next is read, so no execution context can ever observe a
  chunk mid-FETCHING from `handle_fault()` -- and this stays true even with
  Task B's worker pool, since prefetch workers dispatch through a separate
  internal queue, never through `handle_fault()`. `dedup_resident` remains
  theoretically reachable but empirically unhit at this scale/thread-count
  -- reported as a finding about T-3's coverage, T-3 left unmodified, per
  instructions.

**ITEM 10 CORRECTION (supersedes the note below and `experiments/02-first-harness-superseded.md`,
which is marked SUPERSEDED in place, not deleted). Full account in
`experiments/03-corrected-harness.md`.** V1's two "methodology bugs" below were
real but incomplete -- an external review of V1 found three further,
deeper defects that made every V1 number void:

1. **Defect 1 (circular OPT bound).** §5's original text called the
   handler's fault trace "the reference string" and fed it straight to the
   solver. A pager only observes misses (a hit generates no uffd event), so
   that trace is a function of whichever policy produced it, not the
   workload's true access sequence -- feeding it to Belady is circular.
   Proof it was happening: V1's reported OPT values were BELOW the
   provable cyclic floor `n + (passes-1)*(n-k)` at every ratio (e.g.
   floor=32 at r=0.25, reported 31) -- a mathematical impossibility. Fixed
   by spec Amendment A-1: the trace now carries a header (`TRACE_TYPE_REFERENCE`
   vs `TRACE_TYPE_FAULT`); the **workload** (`replay_cyclic()`, via a new
   `ref_trace` argument) writes the ground-truth reference trace; the
   solver (`belady_main.c`) aborts if handed a fault trace. See `trace.h`,
   `replay.c`, `belady_main.c`.
2. **Defect 2 (arms not doing the same work).** The replay driver and
   baseline touched one byte per chunk. The pager fetches a full chunk per
   miss, so the pager was moving ~250x more data than the baseline for the
   "same" 40 touches -- invalidating both the wall-clock comparison and
   the `MADV_RANDOM` finding (RANDOM only won because the workload was
   genuinely sparse under 1-byte touches). Fixed by spec Amendment A-4:
   every reference now reads every 4096-byte page of the chunk, identically
   across all arms, accumulated into a `volatile` sink and verified via
   `objdump` to not be compiler-elided (confirmed: see `replay.c`'s and
   `baseline_main.c`'s disassembly, a genuine per-page load/accumulate/store
   loop, not optimized away).
3. **Defect 3 (wrong primary metric).** §9 already named
   `/proc/PID/io:read_bytes` as the primary cross-arm metric; the harness
   reported fault counts instead. This is what let arm E appear to beat
   OPT (prefetch removes faults but reads the same bytes). Fixed: both
   `replay_main.c` and `baseline_main.c` now report `read_bytes` from
   `/proc/self/io` deltas, and the pager arms additionally report their own
   byte accounting (`region_t.stat_bytes_fetched`, incremented in
   `pager.c`/`prefetch.c`) as an independent cross-check -- a discrepancy
   beyond one chunk is reported, not silently resolved by picking one.

**Spec amendments applied to `docs/design-history.md`:**
- **A-1** (§5, §9): reference trace is workload-authored, never the
  handler's; header distinguishes the two; solver aborts on a fault trace.
- **A-2** (§10): replaced the approximate `(1-r)*W` sanity check --
  which item 9 already found rested on a flawed derivation, see the item 9
  note below -- with the exact, provable cyclic floor
  `n + (passes-1)*max(n-k,0)`, checked unconditionally on every solver run.
- **A-3** (§7, I-7): `reconcile()` now runs on every eviction and every
  16th fetch otherwise (was: every single fetch), amortizing a real,
  measured cost (a fresh `memory.stat` open/read/close per fetch). A
  `region_config_t.reconcile_interval` field (`--eager-reconcile` in the
  CLI binaries) restores per-fetch checking; the §13 correctness harness
  (`test_correctness.c`, `test_storm.c`) now sets it explicitly.
- **A-4** (§11): the replay driver's (and baseline's) access pattern must
  consume full chunks, identically across every arm.

Repeating a mistake while fixing another one, caught during this
correction: the first attempt at item 9's re-verification script compared
the new OPT (computed over a `layer_order`+prefetch-ON reference trace) to
that same run's 28 real faults, and OPT came out *higher* (30 > 28) --
which looked like a bug but was the same class of error as Defect
3/V1's original E-vs-OPT confusion: OPT is only a valid bound for the run
that generated its reference trace, and prefetch changes what counts as
demand. Fixed by generating the reference trace from a prefetch-OFF
(`layer_order`, arm-D-equivalent) run instead; 30 <= 32 holds correctly.
Left in as a reminder that this specific confusion is easy to reintroduce.

---

**Item 10 V2 results (the corrected, valid sweep).** Full report:
`experiments/03-corrected-harness.md`. Realistic scale (2 GiB region, 128 MiB
chunks, 16 chunks, 5 passes), 3 budget ratios (0.25/0.5/0.75), n=3 reps/cell,
fresh 2 GiB pattern file, `drop_caches` before every A/B run, machine
exclusivity and resource headroom checked before/after. Headline results:
- OPT at/above the exact cyclic floor at every ratio (65/48/32 vs floors
  64/48/32) -- the check that would have caught V1's Defect 1 immediately.
- OPT <= D and E-never-beats-OPT hold on the primary metric (`read_bytes`)
  at every ratio -- the check Defect 3 exists to make possible.
- C (`lru`) still thrashes at 100% miss at every ratio, unchanged from V1/item 7.
- `MADV_RANDOM` inverted hard: V1's fastest mode is now 60-75x *slower*
  than sequential under full-chunk consumption (disabling readahead turns
  every 4096-byte stride into its own small random read). Sequential wins
  now.
- Arm B (hints): reduces bytes read ~5% at every ratio, but costs 30-40%
  more wall-clock time (per-touch `madvise` overhead) -- a genuine mixed
  result, not forced into "helps" or "hurts."
- D's bytes/touch drops with more budget (115.8->95.6->68.8 MiB); the
  kernel-native arms stay flat (~134 MiB/touch) regardless of ratio -- the
  core result supporting the byte-reduction thesis at this scale.
- Now that byte counts are finally comparable across arms (Defect 2 fixed),
  the pager is measurably slower per byte than the kernel's native mmap
  path (~2.3x at r=0.25) -- a real, disclosed architectural cost (uffd
  dispatch, amortized `reconcile()`, no read-ahead pipelining, no
  fetch/compute overlap in this synthetic benchmark), not glossed over.
- Censoring rule did not fire at any ratio tested (no OOM, no
  `E_INFEASIBLE`) -- still unexercised for real, same limitation as V1.

**Item 10 V1 note (historical, kept for the record -- see the correction
above for what superseded it).** Two methodology bugs were caught and
fixed while building the original harness:
1. Arms A/B (mmap baseline) initially showed microsecond wall-times and
   zero `pgscan`/`pgsteal` at every budget ratio, including the tightest.
   Cause: `pattern_16m.bin` had been touched repeatedly by items 1-9's
   tests all session and sat warm in the kernel's shared page cache, so
   arm A/B were measuring page-cache-hit speed regardless of `memory.max`
   -- defeating the entire point of a baseline arm. Fixed by
   `sync; echo 3 > /proc/sys/vm/drop_caches` before every A/B run. (This
   fix is still correct and is carried forward into V2.)
2. OPT (item 9's solver) is computed over arm D's trace specifically.
   Comparing OPT numerically against arm E (which uses prefetch) is
   invalid -- prefetch changes what counts as "demand" by satisfying some
   references before they'd fault, so E can legitimately beat OPT-for-D
   without contradiction. The only relationship that must hold is
   OPT <= D, verified at all three ratios (31<=36, 17<=29, 9<=21). (This
   reasoning was correct, but the underlying OPT values were themselves
   invalid per Defect 1 above -- the comparison logic was sound, the inputs
   weren't.)

**Read `experiments/02-first-harness-superseded.md` before drawing any conclusion from this
data, especially the "what NOT to conclude" section** -- the wall-clock
comparison at this test's scale (16 MiB region, 2 MiB chunks, chosen for
fast iteration all session, not for realistic LLM weight sizes) makes the
kernel's native mmap path look faster than the pager, which would be the
wrong takeaway: per-fetch fixed overhead (uffd syscalls, an `ensure_budget`
memory.stat read on every fetch, ioctls) dominates at 2 MiB chunks and is
exactly what §2's real spike measurements (150 MiB chunks) showed becomes
negligible at realistic scale. The residency-CONTROL result (fault-count
ordering E<D<C, OPT<=D holding at every ratio, LRU thrashing at 100% miss
matching item 7) is what this harness run actually demonstrates.

**Item 9 note -- the spec's own required sanity check turned out to be
wrong, investigated rather than silently loosened or silently trusted.**
§10 requires: "on a strictly cyclic reference string at budget ratio r, the
solver must return approximately (1-r) x W bytes per pass. If it doesn't,
the solver is wrong, not the theory." Implemented that check first and it
failed: for W=20 items/cycle, K=5 slots (r=0.25), steady-state measured
15.56 misses/pass against an expected 15.00 -- small but stable and
reproducible (checked P=10 through P=1000, converges to ~15.79..15.8, not
15). Per the spec's own instruction, treated this as "solver is wrong" and
investigated by hand: traced a small W=4,K=2 example step by step and found
the naive "(1-r)*W" derivation assumes you can permanently pin K items in
cache -- false under strict demand paging, because every miss for a
"cold" item still needs a slot *right now*, which necessarily evicts one of
the supposedly-pinned items. The naive bound isn't achievable; the true
optimal is higher, matching what was measured.

Rather than trust that reasoning alone, cross-checked `belady_simulate()`
(the heap-based, lazily-deleted O(n log n) implementation) against a
deliberately naive O(n^2) reference (linear forward scan for the true next
occurrence at every eviction -- nothing clever to get wrong) across 300
random reference strings/capacities: 300/300 exact matches. That random
cross-check is now `belady_main --selftest`'s actual pass/fail gate; the
cyclic-pattern check from §10 is still computed and printed (per the
spec's letter) but as an informational diagnostic, explicitly labeled "NOT
a pass/fail bound," not asserted against the wrong formula. Also verified:
on a real trace from the item 8 scenario (layer_order+prefetch, 5 passes),
OPT reported 19 minimum misses against the online policy's actual 28 --
OPT <= online always must hold, and does.

**Item 8 note -- a design deviation disclosed up front, and a real bug
caught before it shipped.**
§6.3 says "enqueue chunk N+1 for asynchronous fetch," which could mean a
dedicated prefetch OS thread. `prefetch.c` does NOT add one: §4 step 10 is
explicit that multi-threading "must be justified by measured handler queue
depth, not assumed," and no such measurement exists, and a real second
thread would need a new region-wide accounting lock (`resident_bytes`/
`ensure_budget`/`evict_chunk` currently assume a single caller). Instead,
prefetch runs INLINE in the single handler thread immediately after the
triggering fetch completes -- still satisfies §6.3's observable requirement
(a correctly predicted chunk generates zero fault later) without new SMP
surface, and "one outstanding prefetch maximum" holds trivially since it's
synchronous by construction. Full reasoning in `prefetch.h`'s header
comment.

While building this, found a real deadlock risk before it ever ran: inside
`maybe_prefetch()`, calling `ensure_budget()` for the prefetch target lets
the policy's `select_victim()` legitimately choose the chunk that JUST
became resident (`just_resident`) as its own victim -- under `layer_order`
specifically, a chunk is "unreachable from itself" in the successor walk,
i.e. it looks infinitely far away, exactly the profile `select_victim`
prefers to evict. `just_resident`'s lock is already held by the calling
`handle_absent()` (I-6), so `evict_chunk()` locking it again would deadlock
the single handler thread against itself. Fixed by pinning `just_resident`
(the existing I-4 mechanism) for the duration of the prefetch's
`ensure_budget()` call. `test_prefetch.c` is deliberately wrapped in
`timeout` so a regression here hangs and fails loud, not silent. 3/3 clean
runs: `lru` (whose `predict_next` is always -1) never prefetches (0/0);
`layer_order` + prefetch measurably beats the item 7 no-prefetch baseline
on the identical scenario (28 real faults vs. 32, 13 successful prefetches).

**Known measurement gap, disclosed:** a touch on an already-resident chunk
(prefetched or not) generates no uffd event at all, so "prefetch hits
(faults avoided)" can't be directly counted -- `stat_prefetches` (successful
prefetch completions) is the honest proxy actually available, not a true
hit count. Whether each prefetch was later touched (a real hit) or evicted
unused (wasted) isn't tracked in v1.

**Item 7 note -- an unplanned but genuinely useful observation.**
`policy.c` implements `lru` (evict min `last_fault_seq` among RESIDENT+
unpinned -- note this is age-since-LAST-FETCH, not general access time,
because that's literally the only recency signal a fault-driven design has:
touching an already-resident page installs no PTE change and generates no
event at all, so there's nothing to observe) and `layer_order` (learns a
successor map online from the ACTUAL fetch sequence via `on_fault`, never
from a declared/assumed order per §8; `select_victim` walks the learned
chain from "now" and evicts whichever RESIDENT+unpinned chunk has the
largest next-use distance, treating never-observed chunks as infinitely far
-- the standard Belady approximation). `ensure_budget()` (item 4) now
dispatches to `r->policy->select_victim()` when a policy is set, falling
back to the old lowest-index rule when `r->policy` is NULL, so items 4-6's
tests keep passing unchanged. `test_policy.c` unit-tests both policies
directly (no memfd/uffd needed) against hand-derived cases, including one
built specifically to distinguish Belady-style prediction from "lowest
index": residents `{2,3}` with chunk 2 due next and chunk 3 due later must
evict 3, not 2. 3/3 clean.

Smoke-testing `replay_main` (8 chunks, 3-chunk budget, 5 cyclic passes)
across all three policies turned up something worth keeping: `lru` produced
40/40 faults (100% miss rate -- textbook LRU "sequential flooding": strict
recency-based eviction thrashes completely on a cyclic scan larger than the
cache, because by the time the scan wraps around, everything just got
evicted in fetched order anyway), while `layer_order` tied the `default`
fallback at 32/40. This isn't a designed correctness test, but it's a real,
reproducible demonstration of exactly the effect item 10/11's harness is
supposed to measure -- informed eviction beating recency-based eviction on
an LLM-like cyclic access pattern -- and it fell out of an ordinary smoke
test, not a rigged one. Numbers are from one WSL2 run, not a formal
benchmark; treat as illustrative until item 10 does this properly.

**Item 6 note, a test bug caught and fixed, not the implementation:**
`replay_cyclic()` (`replay.c`) sweeps chunks 0..n_chunks-1 for n_passes,
touching each once via `map_a` -- this is the synthetic "known cyclic
order" workload standing in for llama.cpp (item 11) per §12. The first
version of `test_replay.c` asserted a pure-cyclic trace (every touch faults,
chunk_id cycles 0..7 forever) and failed: 20 faults observed, not 24. This
was the TEST's bug, not `ensure_budget()`'s -- the temporary FIFO-by-
lowest-index victim selector (item 4) makes the two highest-indexed chunks
in a sweep permanently resident after the first pass (they're never the
*lowest* index among residents, so they're never picked as victims again).
Verified independently in Python before touching the test (see the
commit), then rewrote the test around a reference-oracle simulation of the
same selection rule instead of a closed-form guess. 3/3 clean runs after
the fix, oracle sequence `[0,1,2,3,4,5,6,7, 0,1,2,3,4,5, 0,1,2,3,4,5]`
matches the trace exactly. `replay_main` (the standalone CLI, reusable by
item 10's harness) smoke-tested separately and is self-consistent with the
same pattern extended to 4 passes.

**Item 5 note:** the region_t bare counters from items 2-4 (fault/dedup/
eviction) were kept as-is rather than folded into `metrics_t` -- they're
needed by items 2-4's own tests even when `metrics` is NULL. `metrics_t`
(`metrics.c`) adds what those didn't cover: a handler-latency histogram
(plain log2-bucketed, explicitly NOT a real HDR histogram library -- see the
comment in `metrics.h`) and queue-depth high-water. `trace.c` implements
the binary trace writer; `cgroup_stat.c` is the shared single-buffer
cgroup-file reader §9 asks for, and `reconcile()` (item 4) was refactored
onto it so there's exactly one "read a cgroup stat file safely"
implementation. `test_trace.c`: 3/3 clean runs, trace records read back
independently of process state with correct monotonic seq, chunk_id, and
fault type.

**Item 4's victim selector is a placeholder, disclosed not hidden.**
`ensure_budget()` (`budget.c`) needs `policy->select_victim()`, which is
item 7's `lru`/`layer_order` and doesn't exist yet. Until then it picks the
lowest layer_id among RESIDENT, unpinned chunks -- a deterministic FIFO-ish
choice that exists only to make the reconcile/evict/infeasible *mechanism*
testable, not to produce a real replacement policy. `test_eviction.c`
verifies this mechanism exactly (predicted vs actual resident-chunk
membership through an 8-chunk/3-slot sequence, 3/3 clean runs), including a
punch-refetch round trip and I-7's reconcile() never tripping. Item 7 will
swap the selector for the real `policy_t` call; item 4's evict/reconcile
code underneath does not change.

## Layout

```
residctl/
  CLAUDE.md              this file
  docs/
    MECHANISM_SPEC.md     the contract
  src/                    implementation, one file group per build-order item
  results/                run manifests, csv/log artifacts, correctness-harness
                           output, sensitivity curves
  scratch/                throwaway; not tracked in git
```

## Git / GitHub

- Repo is committed under the user's own name/email (git config is set
  globally to match their GitHub account), pushed to their GitHub.
- Commit at meaningful checkpoints — roughly one commit per build-order item
  once it compiles and its own local checks pass, plus doc/spec commits when
  those change. Not a commit per file edit; not one giant commit for everything.
- Do not force-push. Do not rewrite history that's already pushed.

## Design constraints inherited from the spike (do not relitigate)

- **uffd must be `O_NONBLOCK`; `EAGAIN` is the only authoritative empty-queue
  signal**, not `poll()` readiness. Hung 5/5 spike runs before this was found.
  See I-2.
- **`memory.swap.max` must be `0`** or the kernel silently evicts weights the
  pager believes are resident (S3e measured 236 MiB swapped out per run when
  swap was left available). See I-3.
- **`UFFDIO_CONTINUE`, not `UFFDIO_COPY`**: COPY costs 48% more per fetch for a
  benefit (avoiding a double-buffer) that doesn't matter here. See §2.
- Fault *type* (MISSING vs MINOR) is not a valid dispatch key — both occur in
  normal operation. Dispatch on chunk *state*. See I-8.
- `MAP_SHARED|MAP_ANONYMOUS` allocations charge to `memory.stat[shmem]` (spike
  caught a 4096-byte step from exactly this). Any such allocation in the pager
  itself must be counted in the accounting's "known overhead" term.
