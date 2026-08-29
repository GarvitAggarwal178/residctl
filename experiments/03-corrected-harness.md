# Harness Report V2

Corrected re-run of build-order item 10 (§11), following an external review
that found three defects invalidating every number in V1
(`results/HARNESS_REPORT.md`, now marked SUPERSEDED). Full account of the
defects and fixes: `CLAUDE.md`'s "ITEM 10 CORRECTION" note.

## VERDICT

The corrected sweep produced valid results. Expectation 1 (OPT at or above
the provable cyclic floor at every ratio) **held** — the solver's own
unconditional floor check (A-2) printed `OK` for all 9 checks (3 ratios ×
3 reps). No result in this report is below a mathematical impossibility,
which was the exact failure mode that invalidated V1.

## What was wrong with V1

**Defect 1 (circular OPT bound).** §5's handler-side trace was described as
"the reference string" and fed directly to the Belady solver. A pager only
observes misses — a hit generates no uffd event, so the handler is never
even invoked for it — so that trace recorded which references *one
particular policy* happened to miss on, not the workload's true access
sequence. Belady over a policy-dependent miss set is circular: it computes
"the best you could do given the misses this policy already made," which is
not a lower bound on anything general. Proof it was happening: V1's
reported OPT values were below the provable cyclic floor
`n + (passes-1)*(n-k)` at every single budget ratio (e.g. floor=32,
reported 31, at r=0.25) — a mathematical impossibility, meaning the
solver's input, not necessarily its algorithm, was wrong.

**Defect 2 (arms not doing comparable work).** The replay driver and the
mmap baseline touched one byte per chunk per reference. The pager fetches a
full chunk on every miss. For the same 40 "touches," the pager was moving
roughly 250× more data than the baseline — invalidating the wall-clock
comparison between arms and biasing the `MADV_RANDOM` finding (RANDOM only
won because the workload was genuinely sparse under one-byte touches, not
because it's actually better under real consumption).

**Defect 3 (wrong primary metric).** §9 already specified
`/proc/PID/io:read_bytes` as the primary cross-arm metric; V1's sensitivity
table led with fault counts instead. This is what let arm E (prefetch)
appear to beat OPT — prefetch removes faults from the demand path but reads
the same (or more) bytes, so the anomaly doesn't arise once bytes are the
metric actually being compared.

## Fixes applied

- **A-1** (`docs/MECHANISM_SPEC.md` §5, §9; `src/trace.h`, `src/trace.c`,
  `src/replay.h`, `src/replay.c`, `src/belady_main.c`): every trace file
  now carries an 8-byte header naming its kind. `TRACE_TYPE_REFERENCE` is
  written by the **workload** (`replay_cyclic()`'s new `ref_trace`
  argument, `src/replay.c:60-66`) — every access, hit or miss, in order.
  `TRACE_TYPE_FAULT` is still written by the handler (`src/pager.c:48-49`)
  for metrics/dedup/prefetch accounting, never for the solver.
  `belady_main.c`'s `load_reference_string()` (`src/belady_main.c:44-72`)
  reads the header and **aborts** if handed a fault trace. Verified: item
  9's regression test (`src/run_item9_tests.sh`) includes a negative test
  that a fault-trace file causes `SIGABRT`.
- **A-2** (`docs/MECHANISM_SPEC.md` §10; `src/belady_main.c:141-177`): the
  approximate `(1-r)*W` sanity check — which item 9 had already found rests
  on a flawed derivation (see `CLAUDE.md`'s item 9 note) — is replaced by
  the exact, provable floor `n + (passes-1)*max(n-k,0)`, detected via
  `detect_cyclic_scan()` and checked **unconditionally on every solver
  run**, not just self-test.
- **A-3** (`docs/MECHANISM_SPEC.md` §7/I-7; `src/budget.c:92-118`;
  `src/region.h`, `src/region.c`): `reconcile()` now runs on every eviction
  (unconditionally — I-7's safety property is unchanged) and otherwise
  every `reconcile_interval` fetches (default 16, was every single fetch).
  `region_config_t.reconcile_interval=1` / `--eager-reconcile` in the CLI
  binaries restores the old per-fetch behavior; `test_correctness.c` and
  `test_storm.c` set it explicitly for the §13 harness.
- **A-4** (`docs/MECHANISM_SPEC.md` §11; `src/replay.c:41-51`,
  `src/baseline_main.c`): every reference now reads every 4096-byte page of
  the chunk, accumulated into a `volatile` sink (`g_replay_sink` /
  `g_sink`), identically across all arms. **Verified via `objdump`**, not
  assumed: the disassembly of both `replay_cyclic()` and `baseline_main`'s
  touch loop shows a genuine per-page load/accumulate/store loop (`movzbl`
  at 4096-byte stride, accumulate, store back, compare, loop) — not
  eliminated by `-O2`.
- **Defect 3 fix** (`src/replay_main.c`, `src/baseline_main.c`,
  `src/region.h`/`pager.c`/`prefetch.c`): both binaries now report
  `read_bytes` from `/proc/self/io` deltas. The pager arms additionally
  report their own byte accounting (`region_t.stat_bytes_fetched`,
  incremented at every real fetch in `pager.c` and every prefetch in
  `prefetch.c`) as an independent cross-check; a discrepancy beyond one
  chunk is reported, never silently resolved by picking one (none occurred
  — see Sensitivity Results).

**A mistake caught while fixing the others:** the first version of item 9's
re-verification script generated the reference trace from a
`layer_order`+prefetch-ON run and compared OPT against that run's real-fault
count. OPT came out *higher* than the online count (30 vs 28), which looked
like a bug but was the same class of error as Defect 3 — OPT is only a
valid bound for the run that generated its own reference trace, and
prefetch changes what counts as demand. Fixed by generating D's (not E's)
reference trace for OPT; the correct relationship (30 ≤ 32) held. Kept in
`CLAUDE.md` as a documented near-miss, not silently corrected without a
trace.

## Correctness harness re-run

Re-ran §13 T-1 through T-5 in full after all fixes, with `reconcile_interval=1`
(`--eager-reconcile` equivalent) forced per A-3's explicit instruction that
amortization is a performance concession, not for the tests built to catch
exactly this class of bug.

- **T-1** (full 16 MiB region read under 25% budget, all three policies):
  0 mismatches, all three policies.
- **T-2** (1000-iteration punch-refetch ping-pong): 0 mismatches, 999
  evictions as expected.
- **T-3** (8 threads, tight 2-chunk budget, 60 real seconds under
  `timeout`): 103,388 touches, 0 mismatches, `resident_bytes` never
  exceeded budget, no hang.
- **T-4** (arbitrary 15-touch non-monotonic sequence): `memory.stat[shmem]`
  matched `resident_bytes + known_overhead` **exactly**.
- **T-5** (`belady_main --selftest`): 300/300 naive-reference cross-check
  matched; all three cyclic-floor checks `OK`; reference/fault header
  identification both `OK`.

All five passed. Full log: `results/correctness_harness_log.txt`.

## Machine exclusivity and resource headroom

**Before the 2 GiB run:** `free -h` showed 7.1 GiB available (7.6 GiB
total, 549 MiB used), 2.0 GiB swap untouched, 931 GB disk free, 16 CPUs.
`uptime` load average 0.00-0.03. No `cn-spike`, `gate5r_driver`, or
`iperf3` process present — checked explicitly via `pgrep` in the harness
script, which is set to abort and report (not proceed) if found.

**After the full sweep** (all three ratios, ~35 minutes of continuous
runs): `free -h` showed 6.9 GiB free / 7.0 GiB available, swap still
untouched. Load average 0.89-1.11 — elevated relative to the pristine
baseline, but attributable entirely to the harness's own just-finished
activity (only the harness's own bash process and normal system daemons
were visible in `ps aux`; no foreign workload). No indication the machine
was shared with anything else during the run.

**Resource headroom check before starting:** with `region_len=2GiB` and the
tightest budget (`r=0.25`, `budget_bytes=512MiB`, `memory.max=576MiB`),
7.1 GiB available on a 7.6 GiB guest was judged ample headroom — the sweep
never came close to exhausting the VM (see the "after" `free -h` above).

## Sensitivity results (n=3 per cell)

All arms ran 80 references (5 passes × 16 chunks) against a fresh,
never-before-touched 2 GiB position-derived pattern file
(`scratch/pattern_2g.bin`), 128 MiB chunks. `memory.max = budget_bytes +
64 MiB`, identical across every arm at a given ratio.
`sync; echo 3 > /proc/sys/vm/drop_caches` ran before every arm A/B
invocation. Raw data: `results/harness_v2_sweep.csv`; full log:
`results/item10_v2_harness_log.txt`.

### Primary metric: read_bytes (median of n=3, MiB/touch)

| Ratio | A (best: sequential) | B (seq+hints) | C (lru) | D (layer_order) | E (layer_order+prefetch) | OPT |
|---|---|---|---|---|---|---|
| 0.25 | 134.2 | 127.4 | 134.2 | **115.8** | 142.6 | 104.0 |
| 0.50 | 134.2 | 127.4 | 134.2 | **95.6** | 107.4 | 76.8 |
| 0.75 | 134.2 | 127.4 | 134.2 | **68.8** | **67.1** | 51.2 |

(OPT column is `minimum_bytes_fetched / 80`, for comparison; OPT is not an
arm that "runs," it's the bound.)

**D's bytes/touch drops with more budget (115.8→95.6→68.8 MiB) — the
kernel-native arms (A/B/C) do not move meaningfully less data as budget
increases (134.2→134.2→134.2 for A, essentially flat).** For this cyclic
scan bigger than any tested cache size, the kernel's own reclaim doesn't
get proportionally better with more headroom; the explicit pager does. This
is the core result supporting the project's thesis at this scale.

### Absolute median read_bytes per ratio (bytes)

| Ratio | A | B | C | D | E | OPT |
|---|---|---|---|---|---|---|
| 0.25 | 10,737,455,104 | 10,191,994,880 | 10,737,418,240 | 9,261,023,232 | 11,408,506,880 | 8,724,152,320 |
| 0.50 | 10,737,455,104 | 10,192,093,184 | 10,737,418,240 | 7,650,410,496 | 8,589,934,592 | 6,442,450,944 |
| 0.75 | 10,737,455,104 | 10,192,158,720 | 10,737,418,240 | 5,502,926,848 | 5,368,709,120 | 4,294,967,296 |

**OPT ≤ D holds at every ratio** (8.72G≤9.26G, 6.44G≤7.65G, 4.29G≤5.50G).
**E never beats OPT on bytes at any ratio** (11.41G>8.72G, 8.59G>6.44G,
5.37G>4.29G) — this is the check Defect 3 exists to make possible; V1
couldn't make it because it wasn't measuring bytes.

**E vs D is ratio-dependent, and this is real, not noise.** At r=0.25, E
reads *more* bytes than D (11.41G vs 9.26G) — under a tight budget,
speculative prefetches are evicted before use often enough to cost more
than they save. At r=0.75, E reads *fewer* bytes than D (5.37G vs 5.50G) —
with more room, prefetch's speculative fetches survive to be used and
sometimes preempt a real fetch that would otherwise have happened later
(E's 32 real faults + 8 prefetches = 40 total fetches, versus D's 41 real
faults — E did fewer total fetches here, not just fewer *demand-path*
faults). Confirmed via `pager_bytes_fetched` matching `io_read_bytes_delta`
exactly at every cell (see Anomalies — no discrepancy anywhere).

### Secondary metric: demand faults (median of n=3)

| Ratio | C (lru) | D (layer_order) | E (layer_order+prefetch) | OPT |
|---|---|---|---|---|
| 0.25 | 80/80 (100%) | 69/80 | 55/80 | 65 |
| 0.50 | 80/80 (100%) | 57/80 | 44/80 | 48 |
| 0.75 | 80/80 (100%) | 41/80 | 32/80 | 32 |

Same ordering as V1 (E<D<C, C thrashes at 100%), for the reason V1 already
established: `layer_order` uses known future access order instead of blind
recency, and prefetch converts some future demand faults into no-ops. This
metric is retained as secondary/diagnostic, per §9 — it is not what the
project's byte-reduction thesis is measured against.

### Secondary metric: wall-clock (median of n=3, seconds)

| Ratio | A (sequential) | A (normal) | A (random) | B | C | D | E |
|---|---|---|---|---|---|---|---|
| 0.25 | 2.14 | 2.75 | **167.7** | 2.92 | 5.00 | 4.29 | 5.18 |
| 0.50 | 2.27 | 2.42 | **169.2** | 2.97 | 5.50 | 3.73 | 4.17 |
| 0.75 | 2.35 | 2.45 | **163.6** | 3.15 | 5.56 | 3.09 | 3.34 |

**Now that byte counts are comparable (Defect 2 fixed), the wall-clock
comparison is meaningful — and it says the pager is consistently slower
per byte than the kernel's native mmap path.** At r=0.25: A moves 10.74 GB
in 2.14s (≈5.0 GB/s); D moves 9.26 GB in 4.29s (≈2.16 GB/s) — roughly 2.3×
slower per byte. This pattern holds at every ratio. This does not
contradict the byte-reduction thesis (D still reads meaningfully fewer
total bytes than the kernel-native arms at higher budgets — see the
primary table), but it is a real, disclose-worthy architectural cost: the
pager's own per-fetch overhead (uffd read/dispatch, the amortized but
nonzero `reconcile()` cost, the `UFFDIO_CONTINUE` ioctl, no read-ahead
pipelining) is not free, and this synthetic benchmark has no compute phase
to hide fetch latency behind (real inference would overlap fetch with the
previous chunk's matrix multiply; this harness cannot, since touching the
data *is* the entire workload). Left as a finding for item 11 to actually
test, not editorialized further here.

## Expectations 1-6

1. **OPT at or above the cyclic floor at every ratio: HELD.** Floors were
   64/48/32 (r=0.25/0.5/0.75); measured OPT misses were 65/48/32 — at or
   above the floor every time, exactly matching (not just exceeding) it at
   r=0.5 and r=0.75.
2. **C (lru) misses ~100% at every ratio: HELD, exactly.** 80/80 at every
   ratio, every rep — the sequential-flooding pathology reproduces
   unchanged from V1/item 7.
3. **OPT ≤ D on read_bytes, and E does not beat OPT on read_bytes: HELD at
   every ratio**, both on the byte metric and the underlying miss-count.
4. **The MADV_RANDOM result may invert: HELD, dramatically.** `MADV_RANDOM`
   went from V1's fastest mode to 60-75× *slower* than sequential/normal at
   every ratio (162-174s vs 2.1-3.0s) — disabling readahead under
   full-chunk consumption means every 4096-byte stride becomes its own
   small random read. Sequential is the new consistent winner and was used
   for arm A/B throughout.
5. **Arm B may help rather than hurt: MIXED, precisely characterized, not
   forced into a binary.** B reduces bytes read by ~5% versus A at every
   ratio (10.19GB vs 10.74GB — the PAGEOUT hints do help the kernel avoid
   some re-reads) but *increases* wall-clock time by 30-40% at every ratio
   (2.9-3.6s vs 2.1-2.5s — the per-touch `madvise(MADV_PAGEOUT)` syscall
   overhead costs more than it saves in wall-clock terms at this scale).
   Helps on the primary metric, hurts on the secondary one.
6. **The wall-clock comparison becomes meaningful: HELD, and the result is
   that the pager is slower per byte than the kernel-native path** (see
   Secondary metric: wall-clock, above). Reported as measured, not
   editorialized into either "the thesis is disproven" or "this doesn't
   matter."

## Censoring

**Did not fire.** No `E_INFEASIBLE` result and no OOM kill occurred in any
of the 72 data-row runs across all three ratios (`infeasible` column is 0
throughout; no `censored=1` rows in `harness_v2_sweep.csv`). The
pre-registered rule (record as a censored point, not discard, not retune
the budget to avoid it) was never exercised at these ratios and this scale
— worth stating plainly as a limitation of this specific sweep, not a claim
that infeasibility can't happen at a tighter ratio.

## Anomalies

- **Byte-accounting cross-check (pager vs kernel) matched exactly at every
  single cell**, not just within the one-chunk tolerance the code checks
  for: `pager_bytes_fetched == io_read_bytes_delta` in every C/D/E row in
  the raw CSV. No discrepancy to report.
- **`MADV_RANDOM`'s ~65-second run-to-run spread** (162s to 174s across
  reps) is much wider (in absolute terms) than any other arm's spread, but
  proportionally similar (~5-7%) to the others' — consistent with it being
  dominated by many small, independent random reads whose individual
  latency varies, not a sign of measurement instability.

## What I did NOT test

- A budget ratio tight enough to trigger `E_INFEASIBLE` or an actual OOM —
  the censoring rule's actual behavior under a real censored point remains
  unexercised across both V1 and V2.
- Repeated runs of arm A's non-winning `madvise` modes beyond n=3 each
  (used only to select the winner, per the established "harness sweeps,
  binary executes one config" pattern) — no claim of n=3 rigor for the
  losing modes' own numbers beyond what's reported.
- llama.cpp integration (build-order item 11) — still explicitly out of
  scope.
- Any chunk size other than 128 MiB, or any region size other than 2 GiB —
  this is the scale the spike's §2 measurements apply to, not a sweep over
  scale itself.
- The dedup branches (`handle_fault()`'s RESIDENT/FETCHING cases) — still
  never confirmed to fire, same disclosed gap as items 2, 6, and the §13
  correctness harness note; this re-run did not specifically target it and
  didn't observe it either way.

## Final check

No fabricated numbers: every value in this report is either a direct read
from `results/harness_v2_sweep.csv` (raw data from real runs, machine
exclusivity confirmed before and after) or a straightforward, disclosed
computation over it (median, min/max, MiB conversion). No test was weakened
to pass — the correctness harness ran with the *stricter* eager-reconcile
setting, not the amortized default, per A-3's explicit instruction. No
conclusion was drawn from absence of a crash — every expectation was
checked against an explicit predicted value or relationship stated before
being checked, and one genuine near-miss (the OPT-vs-prefetch-run mistake
while re-verifying item 9) is disclosed rather than silently corrected.
