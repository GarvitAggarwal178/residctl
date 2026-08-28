# CLEANUP SESSION — Summary

Spec: `docs/overnight/final session.md` — no wait, that was the final session.
This session's spec is `docs/cleanup.md`. Three phases; after this the project
moves to writeup. Commits `a0e7259` (P1), `214b871` (P2), `022a154` (P3),
`<final>` (summary + PROJECT_STATE), all pushed to `origin/main`.
Machine exclusive for every phase that ran code (own runs only; `pgrep
cn-spike|iperf3|gate5` clean before/after; final load average 0.00).

---

## What was fixed / done

### Phase 1 — the arm E "deadlock" (`phase1_deadlock_fix.md`)

**The premise was wrong.** The spec's Phase 1 assumed a hard deadlock (a demand
fault deduped against an orphaned `FETCHING` slot). gdb backtraces plus a 420 s
instrumented run prove it is a **livelock**: fetch workers active the whole time,
`stat_absent_handled` climbs linearly 239 → 4250, `stat_bytes_fetched`
48 GB → 910 GB (a ~10 GB job — ~90× I/O amplification), `stat_infeasible = 0`,
would complete in ~6 hours. Cause: at r ≤ 0.375 the budget cannot retain the two
large chunks (`token_embd` 175 MiB, `output` 243 MiB) alongside the layers; the
real-model eval-callback `notify` fires *after* compute so `protect_current` on
shields the *previous* token's chunks; the two big chunks are re-fetched every
token; `protect_current` on also breaks `prefetch_admit` so the prefetcher spins
(`stat_prefetch_declined` → 2104, `stat_prefetches` frozen at 77).

**Fix implemented anyway (both pre-decided parts — they close real latent bugs):**
- **(a)** `pager_abandon_fetch()` — every `CHUNK_FETCHING → ABSENT` drop
  (4 sites: `pager.c` `handle_absent`, `prefetch_pool.c` `do_one_demand` +
  `do_one_prefetch`, `prefetch.c` `maybe_prefetch`) now issues `UFFDIO_WAKE` so a
  deduped faulter is not left blocked. Assertion added: a worker may not leave a
  chunk in `CHUNK_FETCHING`.
- **(b)** FETCHING-state watchdog in `pager_run`'s never-blocking loop —
  `chunk_t.fetching_since_ns` + `region_t.fetching_timeout_ms`
  (`--fetching-timeout-ms` / `fetching_timeout_ms=`, default 30 000) +
  `stat_fetching_timeout`. `trylock(c->lock)` so it never blocks; reclaims only a
  genuinely orphaned slot.

These are a real robustness improvement (`stat_prefetch_declined` = 2104 shows
the drop paths are hammered). They do not — and cannot — fix a livelock.

**Second finding — Phase 2's default flip regressed the real model.** The final
session's Phase 2 flipped `residctl_llama.c`'s `protect_current` default to
`off`, decided on the *synthetic* grid (which has `--consumption-signal
all-threads`). On the **real model** that flip regresses arm D by **+67–78 % at
every ratio**, deterministically (r=0.25: 217 vs 126 GB). **Reverted**
`residctl_llama.c` to default `protect_current = on`; `replay_main.c` keeps
`off`. Re-verified: arm D back to byte-identical 126.14 / 79.29 / 43.36 GB.

**Verification:**
- #1 (original trigger, must complete): **does not** — it is a livelock (see
  above). Not fixed; mitigated by config.
- #2 T-1..T-7 `--eager-reconcile`: **PASS** (mismatches = 0, no lost fault, T-6
  `dedup_fetching > 0`).
- #3 no regression on shipping default: **regression found → default reverted →
  re-verified byte-identical to Phase 1.**
- #4 arm E at r ≤ 0.375, protect-off: **completes** (no livelock) but reads
  37–43 % more than arm D — prefetch non-advantageous. `stat_fetching_timeout =
  0` on every run.

### Phase 2 — CLAIMS.md reconciled (`phase2_claims_reconciled.md`)

Every claim body in `results/overnight/CLAIMS.md` rewritten to be correct
standalone; the "FINAL SESSION amendments" header deleted; every `Superseded
prior claims` field kept and several expanded. Added a policy-default note and
**Claim 10** (the retention × protection livelock). `WRITEUP_PACKAGE.md` §§1–6
re-checked — no abstract number moved; the arm-E-failure framing corrected to
"livelock" and the "heuristic unnecessary" claim scoped to the synthetic path.

### Phase 3 — RELATED_WORK.md (`RELATED_WORK.md`, ~900 words)

Six sections: application-controlled memory management (Young 1987; Appel & Li
1991; Harty & Cheriton 1992; exokernel 1995) — the idea is old and not ours;
what Linux shipped and why `madvise`/`fadvise`/`mlock` are insufficient;
a positioning table (kernel replacement, `cache_ext`/`sched_ext`, DAMON,
vLLM/FlexGen/Pie, weight-streaming work); `userfaultfd`'s established uses
(post-copy migration, CRIU) and the residency-control gap (+ the Jan 2026
`vm_uffd_ops` RFC); the contribution in 4 sentences; what we do not claim.

---

## What the verification showed (the numbers)

| check | result |
|---|---|
| arm E r=0.25 protect-on, 420 s | rc=124; counters linear; `stat_bytes_fetched` 48→910 GB; **livelock** |
| T-1..T-7 with the fix | PASS |
| arm D, protect **off** (post-Phase-2 default), real model | r0.25/0.5/0.75 = 216.6 / 141.1 / 72.4 GB — **+72 / +78 / +67 %** vs Phase 1 |
| arm D, protect **on** (reverted default), real model | 126.14 / 79.29 / 43.36 GB — **byte-identical to `phase1_equal_budget.csv`** |
| arm E r≤0.375, protect off | completes; 173 / 140 GB — 37–43 % worse than arm D |
| `stat_fetching_timeout` | 0 in every run (no orphaned slot exists — it is a livelock) |

---

## What remains open (permanently, for this project)

1. **The arm-E livelock is not fixed** (BLOCKERS.md FINDING 1). A fix would make
   the dedup / `ensure_budget` path escalate to `infeasible` rather than spin, or
   change `select_victim` to protect the just-consumed chunk when the signal
   lags — both are policy/mechanism changes the cleanup spec forbade. Mitigation
   is config: run arm D (not arm E), `protect_current on`, at r ≤ 0.375.
2. **The real-model consumption signal lags** (eval callback fires post-compute).
   The synthetic `--consumption-signal all-threads` fix has no real-model
   equivalent; the `protect_current` heuristic compensates and is load-bearing
   there. A real engine integration (item 11) could fire `notify` *before* each
   layer's compute and close this — not done.
3. Everything already listed in `results/final/FINAL_SUMMARY.md` "what the next
   session should do first" — larger model, bare metal, WP0 spec.

---

## Documents edited this session

| file | change |
|---|---|
| `src/pager.{c,h}` | `pager_abandon_fetch()`; FETCHING watchdog; `fetching_since_ns` on all transitions |
| `src/prefetch_pool.c`, `src/prefetch.c` | drop paths → `pager_abandon_fetch`; not-FETCHING assertions |
| `src/region.{c,h}` | `chunk_t.fetching_since_ns`; `region_t.fetching_timeout_ms` / `stat_fetching_timeout`; config field |
| `src/replay_main.c` | `--fetching-timeout-ms`; `stat_fetching_timeout` in output |
| `src/residctl_llama.c` | `fetching_timeout_ms=` config key; SIGDUMP2; **`g_protect_current` default reverted to 1** |
| `results/overnight/CLAIMS.md` | full body reconcile; Claim 10; header deleted |
| `results/final/WRITEUP_PACKAGE.md` | §§1–6 re-check; livelock framing; heuristic-scope |
| `results/cleanup/*.md` | this summary + the three phase reports |
| `results/PROJECT_STATE.md` | §3, §4, §5 amended (below) |

## Correctness — final

T-1..T-7 with `--eager-reconcile`: **PASS** after the Phase 1 code changes.
`mismatches = 0`; T-7 no lost fault; T-6 `dedup_fetching = 15019 > 0`.

## Final check (per the spec)

No number estimated or inferred — every value is a measured counter, a SIGUSR1
dump, a gdb backtrace, or a byte count from a CSV. No test weakened (an assertion
was added). Every verification checked against a measured value; the one that did
not hold (#1 — "the original trigger must complete") is reported as a livelock
with the 420 s counter trace as evidence.
