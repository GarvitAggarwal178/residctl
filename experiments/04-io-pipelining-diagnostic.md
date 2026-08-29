# Diagnostic Report — I/O pipelining, prefetch depth, dedup

Item 10b. V2 (`experiments/03-corrected-harness.md`) is accepted and unchanged; this
is diagnosis on top of it, not a correction. Three independent tasks, run in
the order given.

## VERDICT

Task A's data supports **bandwidth-limited**, not serialization: device-busy
fraction 0.82-0.86, per-request bandwidth within 85-95% of the spike's
O_DIRECT median, inter-fetch dead time negligible (<1% of cycle time). Task
B's expectation 4 (a depth exists at r=0.75 where wall-clock approaches arm A
while `read_bytes` stays below it) **partially held**: the gap to arm A closed
by roughly half (2.75s → 2.43s at best) but did not close fully at any tested
depth, and Task A's bandwidth-limited finding explains why — with a single
fetch already near the device's real ceiling, concurrency has little idle
capacity left to fill.

## Machine exclusivity

Checked before Task A and after Task C. Before: `uptime` load average
0.06-0.08, no `cn-spike`/`gate5r_driver`/`iperf3` process running (an initial
`pgrep -f` "match" turned out to be the check command matching its own
argument string — confirmed via `pgrep -af`, which showed only the check
itself; investigated rather than assumed, per instructions). After: load
average 0.21-0.46, decaying, attributable entirely to this session's own
just-finished background sweeps (`ps aux` showed only this session's
processes and normal system daemons). No foreign workload at any point.

## Task A — Fetch time breakdown

Instrumented `fetch_chunk()` with `--fetch-trace <path>` (one record per
fetch: handler entry, read start/end, `UFFDIO_CONTINUE` end, handler exit,
bytes), preallocated in-memory, flushed once at exit (never inside the
handler). Ran arm D (`layer_order`, prefetch off) at all three V2 budget
ratios, n=3, at V2's exact parameters (2 GiB region, 128 MiB chunks, 5
passes). Full data: `experiments/logs/task_a_log.txt`.

| Ratio | Read duration (median, ideal 52.2ms) | Effective BW (median) | Device-busy fraction | Inter-fetch dead time (median) | `UFFDIO_CONTINUE` (median, spike expects ~2.5ms) | Handler overhead (median) |
|---|---|---|---|---|---|---|
| 0.25 | 55.1ms (p10=41.7, p90=68.3) | 2324 MiB/s | 0.816 | 0.55ms | 2.07ms | 9.48ms |
| 0.50 | 59.6ms (p10=47.2, p90=74.8) | 2147 MiB/s | 0.844 | 0.55ms | 2.03ms | 9.25ms |
| 0.75 | 60.4ms (p10=48.3, p90=78.2) | 2119 MiB/s | 0.862 | 0.58ms | 2.03ms | 9.26ms |

**Interpretation: bandwidth-limited, with a real but secondary handler-overhead
contribution — not serialization.**

- **Ruling out serialization**: its defining signature is "meaningful
  inter-fetch dead time." Measured dead time is 0.55-0.58ms against a
  ~66-70ms total per-fetch cycle (<1%). The pager is not idling between
  fetches waiting for the workload to consume data.
- **Supporting bandwidth-limited**: per-request bandwidth (2119-2325 MiB/s)
  sits at 85-95% of the spike's measured single-threaded O_DIRECT median
  (2450 MiB/s) and, critically, well below the spike's own measured *maximum*
  (3396 MiB/s) — this is a real device read, not a cache artifact. Device-busy
  fraction (0.82-0.86) accounts for the large majority of wall-clock time.
- **The remaining ~14-17% gap in device-busy fraction is fully explained by
  measured components, not unaccounted-for time**: handler overhead
  (9.25-9.48ms, ~14% of cycle time) + `UFFDIO_CONTINUE` (~2ms, ~3%) sums to
  almost exactly the observed non-busy fraction. `UFFDIO_CONTINUE` itself
  matches the spike's ~2.5ms expectation closely (2.0-2.1ms measured). The
  9.3ms handler overhead is real but secondary — not "overhead-dominated" by
  the task's own criterion (it doesn't dominate per-fetch time), but not
  zero either. Traced to cause: D's fetches almost always coincide with an
  eviction (e.g. at r=0.75, 29 of 41 fetches also triggered an eviction), and
  A-3's `reconcile()` runs unconditionally on every eviction regardless of
  the periodic amortization counter — so the amortization introduced by the
  item 10 correction provides less benefit for D specifically than for a
  workload with more budget headroom (fewer evictions).
- **This directly explains V2's finding, correcting its stated cause.** V2
  attributed the 2.3x throughput gap between arm A and arm D to "per-fetch
  overhead," citing spike numbers that in fact rule that out (this task's own
  context section pointed this out in advance, and the data confirms it).
  Arm A's V2 throughput (4785 MiB/s) exceeded the spike's own measured
  *maximum* O_DIRECT bandwidth (3396 MiB/s) — consistent with arm A having
  been served by the Windows VHDX host cache, which guest `drop_caches`
  cannot reach, not by genuine device I/O. Arm D's per-fetch bandwidth here
  (2119-2325 MiB/s) sits right where genuine O_DIRECT reads on this machine
  should: close to the spike's median, nowhere near arm A's number. The gap
  is largely apples-to-oranges (real I/O vs. host-cache-served I/O), not
  pager inefficiency.

## Task B — Prefetch depth sweep

Added `--prefetch-depth N` (default 1, item 8's original inline
single-threaded path, completely unchanged) and, for N>1, a fixed pool of N
worker threads (`prefetch_pool.c`) — the one place multi-threading enters the
pager; the fault handler itself stays single-threaded. Swept depth ∈
{1,2,4,8} × 3 budget ratios × n=3 at V2 scale. Full data:
`results/data/historical/task-b-sweep.csv`, `experiments/logs/task_b_log.txt`.

**Three real concurrency bugs found and fixed while building this, not
reasoned out in advance — each reproduced as an actual hang/incorrect-abort
before being fixed:**

1. **False-positive `RECONCILE FAILED`** at depth>1: a worker's `pwrite()`
   into `map_b` populates real shmem pages *while the fetch is still in
   flight*, before `commit_reserved()` moves its bytes from `reserved_bytes`
   into `resident_bytes`. Another thread's `reconcile()` landing in that
   window saw the kernel's `memory.stat[shmem]` already partway toward an
   in-flight fetch that `resident_bytes` alone didn't yet know about. Fixed:
   `reconcile()` now checks that `shmem` falls within
   `[resident_bytes, resident_bytes+reserved_bytes]` (±one chunk slack), not
   a single-point comparison. At depth==1, `reserved_bytes` is always 0
   whenever `reconcile()` runs, so this reduces to exactly the original
   check — no behavior change there.
2. **Deadlock (reproduced, not hypothetical) at `--prefetch-depth 8`**:
   setting `state = CHUNK_RESIDENT` before `commit_reserved()` (which needs
   `budget_lock`) left a window where another thread could select the
   chunk as a victim (it now looks evictable) and block acquiring its
   chunk-lock, while the original thread — still holding that same
   chunk-lock — blocked acquiring `budget_lock` to finish committing.
   Classic circular wait. Fixed by committing before marking RESIDENT in
   all three call sites (`pager.c`, `prefetch.c`, `prefetch_pool.c`).
3. **Second deadlock (flaky, timing-dependent, reproduced at depths 4 and
   8)**: the just-resident chunk that triggers a prefetch round is
   *itself* a prime victim candidate under `layer_order` (it looks
   "unreachable from itself" in the successor walk — the same issue item 8
   found and fixed for the depth==1 case by pinning). The fix pinned it,
   but one step too late — after `state=RESIDENT` was already visible,
   leaving a narrow window. Fixed by making the pin land atomically with
   the commit (`commit_reserved_and_pin()`, one `budget_lock` critical
   section) and holding it for the chunk's entire post-processing, not as
   two separate steps. Found via a caught live hang (`/proc/PID/task/*/wchan`
   showed the driver thread blocked in `handle_userfault` with the pager
   thread idle in `poll()` — the driver's fault had been silently dropped by
   the existing, correct-by-design "infeasible, don't force, drop the
   message" path, but a REAL fault (unlike a dropped prefetch) has nothing
   that will ever wake it again). A fourth, related fix followed from the
   same finding: real fetches now get a bounded retry (up to 200×2ms) when
   `ensure_budget()` goes infeasible *specifically because prefetch
   reservations, not real residents, are occupying the budget* — reserved
   bytes are always transient (every prefetch commits or drops within one
   fetch's duration), so a short wait reliably resolves the race. This
   retry path is unreachable at depth==1.

All fixes verified: 40/5 stress iterations at the worst-case depth (8) with
zero hangs after the fixes (was reproducing in roughly 20-60% of runs before
each fix); full existing regression suite (items 1-9, §13 correctness
harness) re-run clean, including item 8's exact historical numbers
byte-for-byte at depth=1 (28 `absent_handled`, 13 `prefetches` — unchanged).

### Sensitivity table (n=3 median per cell)

| Depth | Ratio | `read_bytes` | Demand faults | Prefetches | `infeasible` | Wall-clock | Device-busy fraction |
|---|---|---|---|---|---|---|---|
| 1 | 0.25 | 10.88 GB | 55 | 30 | 0 | 5.648s | 0.824 |
| 2 | 0.25 | 11.26 GB | 43 | 45 | 0 | 4.360s | 0.860 |
| 4 | 0.25 | 13.31 GB | 35 | 69 | 12 | 4.379s | 0.884 |
| 8 | 0.25 | 16.77 GB | 40 | 89 | 45 | 5.175s | 0.904 |
| 1 | 0.50 | 8.19 GB | 44 | 20 | 0 | 4.333s | 0.830 |
| 2 | 0.50 | 9.98 GB | 42 | 36 | 0 | 4.142s | 0.860 |
| 4 | 0.50 | 9.47 GB | 35 | 39 | 0 | 3.728s | 0.872 |
| 8 | 0.50 | 10.50 GB | 27 | 55 | 0 | 3.564s | 0.903 |
| 1 | 0.75 | 5.12 GB | 32 | 8 | 0 | 2.750s | 0.861 |
| 2 | 0.75 | 5.25 GB | 27 | 14 | 0 | 2.431s | 0.873 |
| 4 | 0.75 | 6.14 GB | 26 | 22 | 0 | 2.775s | 0.865 |
| 8 | 0.75 | 6.14 GB | 26 | 22 | 0 | 2.546s | 0.861 |

(Arm A's V2 reference point: ~10.74 GB read, ~2.14s wall-clock, at every
ratio — unaffected by pager budget since it's the kernel-native baseline.)

**Prefetch hit/waste** (post-hoc, correlating `--fetch-trace` with the
ground-truth reference trace: a prefetch is a "hit" if no later fetch event
exists for that chunk before the run ends — meaning it survived, resident,
until its real next use, per Defect 1's honest observability limit; a
"waste" if a later fetch event exists — meaning it was evicted and had to be
refetched before it was ever used for free):

| Depth | Ratio | Prefetches | Hits | Wasted | Hit rate |
|---|---|---|---|---|---|
| 1 | 0.25 | 90 | 15 | 75 | 0.17 |
| 2 | 0.25 | 135 | 27 | 108 | 0.20 |
| 4 | 0.25 | 203 | 39 | 164 | 0.19 |
| 8 | 0.25 | 261 | 42 | 219 | 0.16 |
| 1 | 0.50 | 60 | 9 | 51 | 0.15 |
| 2 | 0.50 | 108 | 15 | 93 | 0.14 |
| 4 | 0.50 | 117 | 24 | 93 | 0.21 |
| 8 | 0.50 | 165 | 36 | 129 | 0.22 |
| 1 | 0.75 | 24 | 6 | 18 | 0.25 |
| 2 | 0.75 | 42 | 9 | 33 | 0.21 |
| 4 | 0.75 | 66 | 18 | 48 | 0.27 |
| 8 | 0.75 | 66 | 18 | 48 | 0.27 |

Hit rate is low throughout (14-27%) and roughly flat across depth — deeper
prefetch doesn't make individual prefetches meaningfully more or less likely
to survive to use; it just attempts more of them, so absolute wasted bytes
grow with depth. This is the direct, quantitative mechanism behind the
`read_bytes` growth in the table above, not a new finding but a confirmation
of what V2 already inferred qualitatively (E costing more bytes than D at
tight budgets).

### Expectations 1-4

1. **Wall-clock decreases with depth at r=0.75: DID NOT CLEANLY HOLD.**
   2.750s (d1) → 2.431s (d2) → 2.775s (d4) → 2.546s (d8) — non-monotonic,
   with depth 2 fastest and depth 4 briefly *worse* than depth 1. This is
   not noise papered over as a trend: Task A's bandwidth-limited finding
   directly predicts it. A single fetch already runs at 85-95% of the
   device's real ceiling; there is little idle device time left for
   concurrent fetches to fill, so added depth mostly adds thread/lock
   overhead without proportional throughput gain once the fault count has
   already dropped (32→27→26→26 across these same cells).
2. **At r=0.25, deeper prefetch increases `read_bytes` and may increase
   wall-clock, expected to worsen relative to V2: HELD, clearly.**
   `read_bytes` rises monotonically with depth (10.88→11.26→13.31→16.77 GB).
   `infeasible` appears at depth 4 (12) and worsens sharply at depth 8 (45)
   — exactly the "worsening" the expectation predicted. Wall-clock improves
   from depth 1→2 but is clearly worse at depth 8 (5.175s) than at depths
   2-4, consistent with "may increase," not a strict monotonic claim.
3. **Device-busy fraction rises with depth in every cell: HELD at r=0.25 and
   r=0.50 (clean monotonic rise, e.g. 0.824→0.860→0.884→0.904 at r=0.25);
   FLAT (not decreasing) at r=0.75** (0.861→0.873→0.865→0.861). Never
   decreases anywhere — the implementation is not broken/fake concurrency.
   The flatness at r=0.75 is explained by there being less total fetch
   volume to overlap (26-32 faults vs. 40-55 at r=0.25) and the device
   already being busy 86% of the time even at depth 1, leaving less room for
   a trend to show above measurement noise.
4. **A depth at r=0.75 where wall-clock approaches arm A while `read_bytes`
   stays below arm A's: PARTIALLY HELD.** Best wall-clock (depth 2, 2.431s)
   closes roughly half the gap to arm A's 2.14s (was 2.750s at depth 1), while
   `read_bytes` (5.12-6.14 GB across all depths) stays well under half of arm
   A's ~10.74 GB at every depth tested — the byte advantage is not just
   preserved, it's never remotely threatened. The gap to arm A's wall-clock
   does not fully close at any depth. This is not reported as the project's
   headline result, per the pre-registered instruction, because full closure
   did not occur — Task A's bandwidth-limited finding explains why: closing
   the remaining gap would require either a faster device ceiling or genuine
   parallelism across independent storage paths, neither of which more
   worker threads can manufacture once a single stream is already near the
   ceiling.

## Task C — Dedup counters

Counters already existed (`stat_dedup_resident`, `stat_dedup_fetching`,
`stat_absent_handled`, from item 2). Ran T-3 unchanged (8 threads, 2-chunk
budget, 60s): `total_touches=115736 mismatches=0 absent_handled=22991
dedup_resident=0 dedup_fetching=0`. Consistent with every prior observation
across items 2, 6, the §13 harness, and V2 — this is at least the fifth
independent 60-second run to show exactly 0/0.

**`dedup_fetching` is provably unreachable in the current architecture, not
just empirically rare.** Source inspection of `pager_run()`/`handle_fault()`
(`pager.c`): the handler drains uffd messages strictly one at a time —
`read()` one message, call `handle_fault()` **synchronously to completion**
(including the entire fetch, which blocks on real I/O), only then loop back
to `read()` the next message. For `handle_fault()` to observe a chunk in
`CHUNK_FETCHING` state, some *other* concurrent execution context would need
to be mid-`handle_absent()` for that same chunk when `handle_fault()` runs —
impossible by construction, since exactly one thread ever calls
`handle_fault()`, ever, and it never reenters itself. This remains true
**even with Task B's worker pool**: prefetch workers dispatch through a
completely separate mechanism (`prefetch_pool`'s internal queue, driven by
`do_one_prefetch()`), never through `handle_fault()` — so deeper prefetch
depth does not change this answer at all. `dedup_fetching` is dead code
under any configuration where the fault handler itself is single-threaded,
which is every configuration built so far (§4 step 10 keeps it that way by
design).

**`dedup_resident` is theoretically reachable but requires a narrow timing
window that has never been hit.** It needs two real uffd fault messages for
the *same* chunk to both be sitting in the kernel's queue before the handler
drains either of them (i.e. both faulting threads fault while that chunk is
still `CHUNK_ABSENT`/`CHUNK_FETCHING`, before the first one's full fetch —
real I/O, tens of milliseconds at V2 scale — completes). With T-3's 8
threads picking uniformly random chunks among only 2-8 candidates at high
frequency, this is not obviously impossible, but across 115,736 touches and
five independent 60-second runs it has never fired. No architectural reason
rules it out the way it rules out `dedup_fetching`; it is simply rare enough
at this scale/thread-count that it hasn't been observed.

**This is a finding about T-3, not about the mechanism, per the task's own
framing.** T-3 does not exercise the `RESIDENT`/`FETCHING` dedup branches of
`handle_fault()`, and — for `dedup_fetching` specifically — no realistic
single-handler-thread test ever could. T-3's own pass criteria (no
mismatches, no hang, budget never exceeded) are unaffected and remain valid;
they just aren't evidence for the dedup paths specifically. T-3 was not
redesigned to force this, per instructions.

## Anomalies

- The two Task B deadlocks and the reconcile false-positive were all
  timing-dependent (not 100% reproducible per run), which made them
  materially harder to find than a deterministic bug — documented in full
  above rather than summarized away.
- `infeasible` counts at r=0.25/depth≥4 (12 at depth 4, 45 at depth 8) are a
  direct, expected consequence of the bounded-retry fix: some contention
  that would otherwise have deadlocked the driver now correctly resolves to
  a transient infeasible-then-retry-succeeds sequence, but a few prefetch
  attempts (not real faults — real faults always eventually retry-succeed
  under this fix, per the regression testing above) still land as genuine
  `E_INFEASIBLE` when the budget is that tight. Not a correctness problem
  (prefetches are designed to be droppable), just a sign that depth 8 at
  r=0.25 is close to genuinely oversubscribing this budget.
- Task A's handler-overhead finding (9.3ms, traced to `reconcile()` running
  on nearly every fetch via the eviction trigger, not the periodic one)
  means A-3's amortization benefit is workload-dependent: large at low
  eviction rates (generous budget), small when budget is tight enough that
  most fetches also evict. Not evaluated further here — out of this
  diagnostic's scope, noted for whoever next tunes `reconcile_interval`.

## What I did NOT test

- Prefetch depths other than {1,2,4,8}, or depths larger than `n_chunks`
  (16) — untested, and the `cursor == just_resident` chain-break in
  `prefetch_pool_top_up()` means depth ≥ n_chunks may enqueue slightly fewer
  than the full requested depth on a first pass (a minor, disclosed
  under-fill, not a correctness bug — verified the byte/fault accounting
  stays internally consistent regardless).
- Combining Task B's deeper prefetch with the `lru` policy — this diagnostic
  only swept `layer_order` (the only policy with a working `predict_next`;
  `lru`'s is always -1, so prefetch depth is moot for it, consistent with
  item 8's finding).
- Prefetch depth sweep on arms other than the pager (no baseline-arm
  equivalent exists for "depth").
- Whether the Task B deadlock fixes have any performance cost at depth==1
  (the extra `budget_lock` acquisitions in `commit_reserved`,
  `commit_reserved_and_pin`, `unpin_chunk`) beyond confirming the *outputs*
  are byte-for-byte identical to pre-Task-B numbers — wall-clock at depth 1
  was not specifically A/B-tested against a pre-Task-B binary.
- Real device bandwidth ceiling verification independent of the spike's
  original numbers (no fresh `O_DIRECT` bandwidth microbenchmark was run in
  this diagnostic; Task A's interpretation relies on the spike's §2 numbers
  still being representative of this machine/kernel).

## Final check

No fabricated numbers: every value above is a direct read from
`experiments/logs/task_a_log.txt`, `results/data/historical/task-b-sweep.csv`, `experiments/logs/task_b_log.txt`,
or a disclosed, straightforward computation over them (medians, percentiles,
union-interval device-busy fractions, hit/waste correlation — all via the
scripts used to produce the tables, not eyeballed). No test was weakened:
Task A's interpretation was picked from the pre-registered categories using
the actual numbers, not blended to avoid a choice, and where the
categories didn't perfectly fit (a real ~14% overhead component alongside
a bandwidth-limited primary cause) both are stated rather than the
inconvenient one being dropped. Task B's expectations are reported
held/not-held/partially-held individually, including two that did not
cleanly hold (1 and 4), not summarized into a false "prefetch helps."
Task C's zero counters are reported as a finding about the test (with a
source-level proof for one of the two, not just an assertion), exactly as
instructed, and T-3 was not modified. Three real concurrency bugs were
found via reproduction (flaky hangs, a caught live thread-state dump), not
invented for narrative effect, and are documented with the actual
mechanism, not just "fixed a race."
