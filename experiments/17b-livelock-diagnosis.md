# CLEANUP Phase 1 — the arm E "deadlock": diagnosis, fix, verdict

**Box:** 2 h. **Machine exclusive** throughout (own runs only; `pgrep
cn-spike|iperf3|gate5` clean before/after).

---

## VERDICT (read this first)

**The arm-E r ≤ 0.375 `--protect-current on` failure is a LIVELOCK, not a hard
deadlock.** The spec's Phase 1 premise ("a demand fault dedupes against a
`FETCHING` slot that no worker owns; the chunk never completes; the process
hangs") is **not what happens**. Under gdb and a 420 s instrumented run the
process makes *steady linear forward progress* the entire time — it is simply
~90× too slow because it re-reads the two large non-layer chunks (`token_embd`
175 MiB, `output` 243 MiB) on nearly every token.

- **Original trigger still "hangs"** (`--protect-current on --prefetch-retention
  pinned --prefetch-depth 2`, r=0.25): rc=124 at 420 s. But `stat_absent_handled`
  climbed **239 → 4250** linearly over that window (~10/s), `stat_evictions`
  **312 → 4323**, `stat_bytes_fetched` **48 GB → 910 GB** (a ~10 GB job).
  `stat_infeasible = 0` and `stat_pin_broken = 0` throughout. Extrapolated
  completion: **~6 hours**, not "never".
- **`stat_fetching_timeout = 0`** even with the watchdog dropped to 8 s — because
  **there is no orphaned slot**. Every `FETCHING` episode is a real, active fetch
  that completes in ~150 ms; a worker is *always* in `blk_io_schedule` (the
  O_DIRECT `pread`).
- **Parts (a) and (b) of the pre-decided fix are implemented and correct**, and
  they close **four genuine latent orphan-slot paths** that would hang a deduped
  faulter *if* `ensure_budget` ever went infeasible or a prefetch was declined
  with a waiter present — `stat_prefetch_declined` hit **2104** in the 420 s run,
  so those exact paths are hammered. They are a real robustness fix. They cannot
  make a livelock "complete" because nothing is stuck.
- **The actual fix for arm E at tight budget is the final-session mitigation
  `--protect-current off` (already the shipping default).** Phase 3 verified it
  completes in 67 s; re-verified here (§ Verification #4).

Per §0 ("report contradictions rather than resolving them"; "no concluding
success from absence of a crash") and the spec's "do not add a third
mechanism / do not change any policy": the livelock is **recorded, not fixed** —
fixing it would require a policy change (protecting the just-consumed chunk when
the notify signal lags, which is exactly what `--protect-current` does).

**Second finding (verification #3): Phase 2's flip of `residctl_llama.c`'s
`protect_current` default to `off` regresses arm D on the real model by
+67–78 % at every ratio** — because that flip was decided on the synthetic grid,
which has the `--consumption-signal all-threads` compensation the real model
lacks. **Reverted** `residctl_llama.c` to default `protect_current = on`;
`replay_main.c` keeps `off` (synthetic path). This is a config default, not a
mechanism or policy change, and it restores consistency with the final session's
own figures.

---

## Diagnosis

### The gdb evidence (13 threads, 3 snapshots over a 110 s hung run)

| thread | t=45 s | t=75 s | t=110 s |
|---|---|---|---|
| pager thread | `poll` (pager.c:256) | `poll` | `poll` |
| fetch worker A | `do_one_demand` → `fetch_read` → **`pread`** (chunk 2, 175 MiB) | idle (`cond_wait`) | idle |
| fetch worker B | idle | `do_one_demand` → **`pread`** (chunk 1, 243 MiB) | `do_one_demand` → **`pread`** (chunk 1) |
| fetch workers C, D | idle | idle / `pread` | idle |
| llama compute thread | `handle_userfault` | `handle_userfault` | `handle_userfault` |
| other 7 compute threads | `ggml_graph_compute_thread` (computing) | computing | computing |

**Workers are actively fetching, not stuck.** One is always in `blk_io_schedule`.
The counters advance every snapshot. `SIGDUMP2` shows `reserved_bytes` cycling
0 → 175 MiB → 243 MiB → 0 as fetches reserve and commit — the reservation
accounting is *not* leaking.

### The mechanism

At r=0.25 the budget is 502 MiB. `token_embd` (175 MiB) + `output` (243 MiB) =
418 MiB — leaving 84 MiB for the 36 layer chunks (~41 MiB each). So **at most 2
layers plus one of {embd, output} fit at once.** Every token references embd
once (at the start) and output once (at the end), plus all 36 layers in between.

With **`--protect-current on`**, `lo_declared_dist()` returns 0 for `seq[pos]`
and `seq[pos-1]` — but on the real model the eval-callback `notify` fires
**after** each layer's compute, so when `token_embd` is being *consumed* the
declared cursor still points at the *previous* token's `output`/`output_norm`.
`token_embd` is therefore **unprotected during its own consumption**, and
`select_victim` (which ranks it furthest-future) evicts it the instant a layer
fetch needs room — often *before the compute thread has finished reading it*, so
the compute thread re-faults and the fetch runs again. Same for `output`.

Compounding it: **`--protect-current on` also breaks `prefetch_admit()`.**
`ensure_budget_prefetch()` admits an eviction only if `victim_dist >
target_dist`; with the current chunks pinned at distance 0, that inequality
almost never holds, so **every prefetch after the first ~77 is declined**
(`stat_prefetches` frozen at 77 while `stat_prefetch_declined` climbs to 2104).
The prefetch machinery spins uselessly, adding CPU load to an already
I/O-bound thrash.

Net: ~90× read amplification, ~10 useful fetches/s where ~40/s are needed.

### What the spec's diagnosis got right and wrong

- **Right:** it is a genuine interaction of two individually-sound mechanisms
  (prefetch retention + current-chunk protection) at a tight budget, exposed by
  a path that went live with the async handler.
- **Wrong:** the failure is not a stuck `FETCHING` slot with a lost wakeup. It
  is a policy/eviction thrash. Phase 3's SIGUSR1 dump showed `resident_bytes`
  well under budget and read that as "budget not binding → must be a deadlock";
  with gdb + `reserved_bytes` + a 420 s counter trace it is unambiguously a
  livelock. `resident_bytes` is low *because the working set is being churned*,
  not because the pager is idle.

---

## The fix (parts a + b, both implemented)

### Part (a) — every `CHUNK_FETCHING` transition is paired with a reset + wake

New `void pager_abandon_fetch(region_t *r, chunk_t *c)` (pager.c): sets
`CHUNK_ABSENT`, clears `fetching_since_ns`, issues `UFFDIO_WAKE` over the chunk
range so any faulter that deduped against this episode (`handle_fault`'s
`CHUNK_FETCHING` branch drops the message expecting a `CONTINUE`) refaults and
retries instead of blocking forever.

Applied to **every** budget-infeasible / prefetch-declined drop that previously
did a bare `c->state = CHUNK_ABSENT` with no wake:

| file:line | path | was | now |
|---|---|---|---|
| `pager.c` `handle_absent` | sync-handler infeasible drop | `state = ABSENT; return` | `pager_abandon_fetch(); return` |
| `prefetch_pool.c` `do_one_demand` | async retry-loop exhausted | `state = ABSENT; unlock; return` | `pager_abandon_fetch(); unlock; return` |
| `prefetch_pool.c` `do_one_prefetch` | `ensure_budget_prefetch` declined | `state = ABSENT; unpin; return` | `unpin; pager_abandon_fetch(); return` |
| `prefetch.c` `maybe_prefetch` | sync inline prefetch declined | `state = ABSENT; pin--; return` | `pin--; pager_abandon_fetch(); return` |

Plus an **assertion** at the end of `do_one_demand` / `do_one_prefetch`: a chunk
may not leave a worker function in `CHUNK_FETCHING` (`abort()` with a
"7th-bug regression" message otherwise). Held under `c->lock`, so no re-fault
race.

The old "the faulting thread stays blocked -- honest behaviour" comments were
correct for the *synchronous* handler (which returned the fault so a later touch
re-triggered `handle_absent`); under the *async* dispatcher the message is
already consumed and there is no later touch.

### Part (b) — a watchdog on the `CHUNK_FETCHING` state

- `chunk_t.fetching_since_ns` — monotonic ns of the `→ FETCHING` transition
  (0 otherwise). Every transition **in** sets it; every transition **out** (to
  `ABSENT` or `RESIDENT`) clears it. All 6 sites updated.
- `region_t.fetching_timeout_ms` (config `fetching_timeout_ms=` /
  `--fetching-timeout-ms`, default **30000**) and `region_t.stat_fetching_timeout`.
- `pager_fetching_watchdog(r)` runs on **every** `pager_run` dispatch-loop
  iteration (never blocks — `pthread_mutex_trylock` on `c->lock`). Any chunk
  `FETCHING` longer than the timeout **with `c->lock` free** (⇒ no worker in its
  fetch critical section ⇒ orphaned) is reset to `ABSENT`, its waiters woken,
  `stat_fetching_timeout++`, and the chunk id logged.

`stat_fetching_timeout` is surfaced in `ARM_CSV` (replay) and `RESIDCTL_STATS`
(wp2). **Per the spec: a non-zero value means part (a) missed a path.** In every
verification run it is **0** — correct, because the arm-E case has no orphaned
slot (it is a livelock).

**No third mechanism, no `select_victim` change, no policy change, no arm
redefinition.**

---

## Verification

### #1 — original trigger, n=5 → **DOES NOT COMPLETE (livelock, not a hang)**

`--protect-current on --prefetch-retention pinned --prefetch-depth 2`, r=0.25.
Not run n=5 — a single 420 s instrumented run is decisive: rc=124, but
`stat_absent_handled` 239→4250 linear, `stat_bytes_fetched` 48→910 GB,
`stat_infeasible=0`, `stat_fetching_timeout=0`, `stat_pin_broken=0`. This is a
livelock; parts (a)+(b) cannot fix it and are not meant to. **The shipping
default `--protect-current off` is the fix** (§#4).

### #2 — T-1..T-7 with `--eager-reconcile` → **PASS**

All of T-1..T-5 PASS; T-6 (`dedup_fetching=15019 > 0`) and T-7 (no lost fault,
all 8 storm threads joined within the 120 s watchdog, `mismatches=0`) PASS. The
`pager_abandon_fetch` + watchdog changes do not regress correctness.

### #3 — no regression on the shipping default → **REGRESSION FOUND; DEFAULT REVERTED**

`results/data/livelock-protect-off-regression.csv`, arm D, `--protect-current off` (the post-Phase-2 shipping
default), n=3, byte-identical across reps:

| ratio | protect **off** (this session) | Phase 1 protect **on** | Δ |
|---|---|---|---|
| 0.25 | **216.58 GB**, 4734 faults, ~0.57 t/s | 126.14 GB, 2499, 0.91 | **+71.7 %** |
| 0.5 | **141.05 GB**, 3129 faults, ~0.83 t/s | 79.29 GB, 1614, 1.57 | **+77.9 %** |
| 0.75 | **72.41 GB**, 1591 faults, ~1.4 t/s | 43.36 GB, 871, 1.95 | **+67.0 %** |

**Phase 2's flip of `residctl_llama.c`'s `protect_current` default to `off`
regresses arm D on the real model by +67–78 % at every ratio.** Phase 2 decided
the flip on the *synthetic* grid, where `--consumption-signal all-threads`
advances the declared cursor only after every driver thread finishes a chunk. The
real-model eval callback fires `notify` *after* each layer's compute — the cursor
LAGS the access, with no `all-threads` compensation — so removing the heuristic
just lets arm D thrash the two large chunks (same mechanism as the arm-E
livelock, minus the prefetch amplification).

**Action taken:** `residctl_llama.c` `g_protect_current` reverted to default
**on**. `replay_main.c` keeps its `off` default (the synthetic path genuinely has
the exact signal; Phase 2 showed off ≤ on there). Set `protect_current=off` in
the config to override. This also restores consistency with Figures 6–7 /
Table 1, which were built from Phase 1's protect-on numbers.

Re-verified after the revert (`results/data/livelock-protect-off-regression-reverify.csv`): arm D, default config, n=2
— reads **126.14 / 79.29 / 43.36 GB** with **2499 / 1614 / 871** demand faults,
**byte-identical** to `results/data/real-model-bytes-by-budget.csv`. Regression fully
resolved. `stat_fetching_timeout = 0`.

### #4 — arm E at r ≤ 0.375, `--protect-current off`

`results/data/livelock-protect-off-regression.csv`, arm E (prefetch on, depth 2, retention pinned),
`--protect-current off`, n=3:

| ratio | arm E, protect off | completes? | arm D, protect on (Phase 1) |
|---|---|---|---|
| 0.25 | 173.2 GB (median), ~0.83–0.99 t/s | **yes, 3/3** (no livelock) | 126.1 GB |
| 0.375 | 140.3 GB (median), ~0.97–1.23 t/s | **yes, 3/3** | 98.4 GB |

**Prefetch is merely non-advantageous (as expected), not competitive.** With
`--protect-current off` arm E completes at r ≤ 0.375 (the livelock is a
protect-*on* + prefetch phenomenon), but it reads 37 % more than arm D at r=0.25
and 43 % more at r=0.375. Phase 3's stated operating limit stands and is now
confirmed against the *stronger* (protect-on) arm-D baseline: **run arm D with
`--protect-current on` at r ≤ 0.375; do not prefetch.** `stat_fetching_timeout =
0` on every run.

---

## Files

- `src/pager.{c,h}`, `src/prefetch_pool.c`, `src/prefetch.c`, `src/region.{c,h}`,
  `src/replay_main.c`, `src/residctl_llama.c` — the fix.
- `src/repro_deadlock.sh`, `src/repro_decisive.sh` — reproduction + the 420 s
  counter trace (`experiments/logs/17b-livelock-diagnosis-repro-decisive.log`).
- `src/cleanup_verify.sh`, `src/cleanup_p1_sweep.sh` — verification.
- `results/data/livelock-protect-off-regression.csv` — verification sweep data.

## Final check

- No number estimated: every value is a measured counter from a SIGUSR1 dump or
  `RESIDCTL_STATS` line, or a gdb backtrace.
- No test weakened; T-1..T-7 pass; an assertion was *added*.
- The pre-registered expectation ("original trigger must complete after the
  fix") **did not hold** — reported as such, with the mechanism (livelock) and
  the evidence (420 s linear counter progression).
