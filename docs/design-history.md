# Design history

How the mechanism in [`02-design.md`](02-design.md) got to be what it is. The
design document reads as a current-state contract; this one carries the
history — every spec amendment, why it was made, and which experiment record
establishes it — plus the seven latent concurrency bugs found along the way,
and a deep-dive on the last decision (consumption-signal timing).

---

## 1. Spec amendments A-1 … A-14

The original `MECHANISM_SPEC.md` was written against the feasibility spike.
Fourteen amendments followed, each forced by a measurement.

| # | area | what changed | why | experiment |
|---|---|---|---|---|
| **A-1** | trace / OPT | the reference trace is workload-authored (`TRACE_TYPE_REFERENCE`), never the handler's fault trace; the header distinguishes them; the solver aborts on a fault trace | V1's OPT values fell *below* the provable cyclic floor — the handler's own trace is circular input to Belady | [`02`](../experiments/02-first-harness-superseded.md), [`03`](../experiments/03-corrected-harness.md) |
| **A-2** | OPT | replaced the approximate `(1−r)·W` check with the exact provable floor `n + (passes−1)·max(n−k, 0)` | the approximate formula rests on a flawed derivation (demand paging can't permanently pin `k` items) | [`03`](../experiments/03-corrected-harness.md) |
| **A-3** | reconcile | `reconcile()` runs on every eviction and every 16th fetch otherwise (was: every fetch); `--eager-reconcile` restores per-fetch | a fresh `memory.stat` open/read/close per fetch is a real un-amortized cost, dominant at small chunk sizes | [`04`](../experiments/04-io-pipelining-diagnostic.md) |
| **A-4** | harness | the replay driver and arms A/B consume **every page** of a chunk per reference, not one byte | 1-byte touches made arm A read ~250× less than the pager arms, invalidating the wall-clock comparison and `MADV_RANDOM` finding | [`03`](../experiments/03-corrected-harness.md) |
| **A-5** | handler | handler thread is dispatch-only (never blocks on I/O); the fetch moves to a shared worker pool. `--sync-handler` restores the old path | the synchronous design made `dedup_fetching` structurally unreachable and serialized every concurrent fault behind one blocking fetch | [`05`](../experiments/05-async-handler.md) |
| **A-6** | prefetch | prefetch admission: decline an eviction unless the victim is strictly colder than the prefetch target (`next_use_distance` added). `--prefetch-admission {always,guarded}` | 14–27 % prefetch hit rate, flat across depth — hypothesized cause was unconditional eviction | [`05`](../experiments/05-async-handler.md); **superseded as the primary mechanism by A-9** — declined 0/36 cells |
| **A-7** | startup | §4 step 10 rewritten: dispatcher + worker pool, not "one thread" | companion to A-5 | [`05`](../experiments/05-async-handler.md) |
| **A-8** | harness | the replay driver is multi-threaded (`--driver-threads N`), hard `pthread_barrier_t` at chunk boundaries | device-busy was flat regardless of `--fetch-workers` — the single-threaded driver never generated cross-chunk concurrency | [`06`](../experiments/06-concurrent-demand.md); **barrier superseded by A-10** |
| **A-9** | prefetch | prefetch target **retention**: pinned from arrival until consumed or evicted from a bounded FIFO; demand-fetch pin-break override (`stat_pin_broken`). `--prefetch-retention {none,pinned}` | A-6's admission guard was redundant with `layer_order`'s own victim selection; the real waste was a prefetched chunk evicted before its turn | [`06`](../experiments/06-concurrent-demand.md) |
| **A-10** | harness | hard barrier replaced by a bounded lookahead window (`--lookahead-window W`, counting-based, provably bounds `W+1` chunks in flight) | the barrier made the async handler's throughput benefit untestable by construction | [`07`](../experiments/07-lookahead-window.md) |
| **A-11** | prefetch | `stat_pin_broken` documented as the regime-boundary indicator for retention's byte-cost effect; `r_c` is depth-dependent, not a constant | item 10d scored retention "did not hold" without seeing the budget-tightness split `pin_broken` reveals | [`07`](../experiments/07-lookahead-window.md) |
| **A-12** | policy | the policy interface accepts a **declared access sequence** from the workload (`policy_declare_sequence`, `on_access`/`declare_sequence`). `layer_order` split into `layer_order_declared` (new default; distance is a lookup into the declared sequence from a position advanced only by `pager_notify_access()`) and `layer_order_learned` (formerly `layer_order`, byte-for-byte unchanged, retained as the comparison arm). Later extended: a `--consumption-signal all-threads` driver signal, then the `protect_current` heuristic as a fallback for an inexact signal | §1 claims the application knows its access order in advance while the kernel infers it from the past; `layer_order` did the latter | [`13`](../experiments/13-declared-access-order.md), [`16`](../experiments/16-consumption-signal.md) |
| **A-13** | handler | every `CHUNK_FETCHING → ABSENT` drop path pairs the transition with `UFFDIO_WAKE` (`pager_abandon_fetch()`, 4 sites); a FETCHING-state watchdog in the never-blocking dispatch loop (`--fetching-timeout-ms`, default 30 000, `stat_fetching_timeout`) reclaims a genuinely orphaned slot via `trylock` | the 7th concurrency-class issue — a deduped faulter left blocked forever when a drop path forgot the wake | [`17b`](../experiments/17b-livelock-diagnosis.md) |
| **A-14** | policy / integration | four application-side fixes, no pager-mechanism change: (1) `lo_declared_dist()` scan origin is **signal-mode-aware** (`d` starts at 0 pre-consumption, 1 post); (2) the eval callback fires on the **pre-compute** pass and matches the `"embd"` graph node; (3) a startup audit aborts if any declared chunk gets zero consumption signals in the first two decode passes; (4) a declined prefetch backs off 100 ms per chunk. **Default change:** `residctl_llama.c` `protect_current` → **off** (both paths now off) | the arm-E "livelock" (mislabelled three times — over-constrained budget → hard deadlock → livelock from two correct mechanisms) was really an off-by-one distance origin fed by a cursor that lagged a full layer, with `token_embd` unsignalled | [`18`](../experiments/18-signal-audit.md), [`19`](../experiments/19-livelock-fix.md), [`20`](../experiments/20-livelock-synthetic-recheck.md), [`21`](../experiments/21-livelock-real-model.md) |

The original `MECHANISM_SPEC.md` build order and cut ladder are preserved in the
archived spec (`/root/residctl-archive/` is not on GitHub; the spike report it
was built from lives in `/root/spike/results/`).

---

## 2. The seven latent concurrency bugs

Found by instrumentation, not by crashes — each produced silently degraded
behaviour or a hang that only appeared under a specific race.

1. **`RECONCILE FAILED` false-positive range** (experiment [`04`](../experiments/04-io-pipelining-diagnostic.md)) — the divergence tolerance was tighter than `reconcile()`'s own one-chunk operational threshold.
2. **Commit-before-`RESIDENT` deadlock** ([`04`](../experiments/04-io-pipelining-diagnostic.md)) — marking a chunk `RESIDENT` before its budget reservation commits makes it look evictable to a concurrent `ensure_budget()` while the fetching thread still holds the chunk lock and is about to need the budget lock. Fixed in `budget.c:commit_reserved_and_pin()`.
3. **`commit_reserved_and_pin()` atomicity** ([`04`](../experiments/04-io-pipelining-diagnostic.md)) — the reservation commit and the pin had to be one critical section.
4. **Unlocked `latency_hist_record`** (experiment [`05`](../experiments/05-async-handler.md)) — a histogram updated from multiple fetch workers without its lock.
5. **Unlocked stat counters** ([`05`](../experiments/05-async-handler.md)) — several `stat_*` increments raced once more than one worker could run them.
6. **Three unlocked `pin` operations in `do_one_prefetch`** (experiment [`06`](../experiments/06-concurrent-demand.md)).
7. **Four `CHUNK_FETCHING → ABSENT` drop paths issued no `UFFDIO_WAKE`** (experiment [`17b`](../experiments/17b-livelock-diagnosis.md)) — a deduped faulter that hit `handle_fault`'s `CHUNK_FETCHING` branch was then never woken. Latent (needs `ensure_budget` to decline with a waiter present — heavily exercised at tight budget, `stat_prefetch_declined` hit 2104). Fixed by A-13. Distinct from the arm-E livelock, which is a policy/eviction pathology (fixed by A-14), not a lost wakeup.

Campaigns 11–13 found zero new bugs of this class.

---

## 3. Consumption-signal timing — per-path signal mode, not one uniform rule

**Context.** `layer_order_declared`'s next-use distance
(`policy.c:lo_declared_dist`) is a lookup into the workload's declared access
sequence from the current consumption position `pos`. The distance the loop
assigns to `seq[pos]` — the chunk the cursor points at — depends on whether the
workload's consumption signal (`pager_notify_access()`) fires **before** or
**after** the read it announces:

- **fires after the read** (post-consumption): `seq[pos]` was just fully
  consumed; its next use is a whole cycle away → the scan starts at `d = 1` and
  `seq[pos]` gets distance `seq_len` (furthest → top eviction victim). Correct
  Belady for this timing.
- **fires before the read** (pre-consumption): `seq[pos]`'s use is imminent →
  the scan starts at `d = 0` and `seq[pos]` gets distance 0 (protected).

The original code scanned `for d = 1..seq_len` unconditionally — post-consumption
semantics applied to every caller. That is correct for the synthetic
`--consumption-signal all-threads` path (the driver advances `pos` only once
every thread has finished a chunk) but an off-by-one for the real model, where
`wp2_gen.cpp:eval_cb()` now fires the signal on the eval callback's **pre**-compute
pass (LIVELOCK FIX Defect 2).

**Decision.** `lo_declared_dist` takes a **signal mode**
(`policy_set_signal_mode`, default post = the pre-fix behaviour byte-for-byte).
Each caller sets it from how its own notify is wired:

| caller | notify timing | mode |
|---|---|---|
| `replay_main` + `--consumption-signal all-threads` (default) | after full step consumption | post |
| `replay_main` + `--consumption-signal tid0` | before the read | pre |
| `residctl_llama` (real model, Defect 2 in) | before compute of the layer | pre |

**Road not taken — fire notify pre-consumption everywhere.** The cleaner
uniform design is to make *every* path fire the signal before the read and drop
the mode switch entirely: one rule, `d` always starts at 0, and the
`--protect-current` heuristic disappears with it. It was **not adopted here**
because of its blast radius on the synthetic results. The synthetic
`all-threads` path is post-consumption by construction (the "exact" signal the
final measurement session was built around), and switching it to pre would move
every arm-D and arm-E number in `results/final/phase2_*.csv` and every figure
and claim derived from them. This session's Phase 2 expectation was explicitly
"the synthetic path is unchanged; any change there is a regression to
investigate," so a uniform pre-consumption rewrite is out of scope. It is the
right direction for a future engine-integration milestone (item 11) where the
synthetic driver is retired.

**Deliberate deviation — the serial `replay_cyclic` path.** `replay_cyclic`
(used for `--driver-threads 1`) fires `pager_notify_access()` immediately
before the read loop — genuinely pre-consumption — but `replay_main` sets the
mode from `consumption_signal_all` only, which defaults to 1 (all-threads =
post). So the serial path runs on **post** semantics under the default, even
though a clean-slate design would put it on pre. This is intentional: the
WP1 §1.3 determinism grid's serial cells (1 and 2) are a fixed baseline, and
holding them byte-identical is worth more than the local correctness of a
non-default driver mode. An explicit `--consumption-signal tid0` with
`--driver-threads 1` does select pre for that path, which is correct.

**Also settled here.** `--protect-current`'s one-step lookback (forcing
`seq[pos-1]` to distance 0) is redundant on the pre-consumption path once
Defect 2 lands: `seq[pos]` is already 0 by construction, and `seq[pos-1]` is the
*previous* layer, whose next use is a full lap away — protecting it is
counter-productive.

**Resolved (Phase 3c / A-14).** `residctl_llama.c`'s `protect_current` default
flips to **off** — both paths now default off. Phase 3c measured arm D with
`--protect-current on` and all four fixes: on vs off moves arm D by ≤ 1.8 % at
every ratio (`results/data/livelock-arm-d-protect-on.csv`). The cleanup
session's "off ⇒ arm D +67–78 %" — the sole reason it was `on` — was an artifact
of the two bugs this session fixes (post-compute notify + the unsignalled
`token_embd`). `--protect-current` and its unit tests stay for a caller whose
consumption signal is genuinely inexact.
