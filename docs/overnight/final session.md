# FINAL SESSION — Close the remaining gaps, produce the paper table, stop

Work in `/root/residctl/`. Read `results/PROJECT_STATE.md`,
`results/overnight/OVERNIGHT_SUMMARY.md`, `results/overnight/wp2_llamacpp.md`,
and `results/overnight/BLOCKERS.md` first.

**§0 non-negotiable rules apply in full and are not restated.**

**This is the last measurement session.** After it, the project moves to writeup.
Nothing here opens a new line of investigation — every task closes something
already identified. If a task reveals something new and interesting, **record it
and do not pursue it.**

Session rules as before: run every phase, decide nothing beyond what each phase
specifies, write each phase's file as it completes, commit after every phase,
push at the end of each phase, check machine exclusivity before and after each
phase, resume from a verified point if interrupted.

Estimated total: 4-6 hours.

---

## PHASE 1 — Equal-budget baseline · box: 90 min · HIGHEST PRIORITY

### The defect

`wp2_llamacpp.md` records that WP2's arm A ran with
`memory.max = budget + 256 MiB` while arm D ran at `budget`. The arms did not
have the same memory.

Two consequences, opposite in sign:

- The **24% and 51% byte wins at r=0.5 and r=0.75 are conservative** — arm D beat
  a baseline that had *more* memory than it did. This strengthens the result once
  stated correctly.
- The **r=0.25 "D ≈ A" result is void.** At a 4-chunk budget, 256 MiB is a large
  fraction of the total and the comparison is uncontrolled.

### Task

Re-run WP2's measurement sweep with `memory.max` **exactly equal to the budget**
for every arm, no margin, at every ratio.

- Arms A, C, D, E.
- Ratios {0.25, 0.375, 0.5, 0.625, 0.75} — five points, not three. The two new
  ratios cost little and make the central figure complete.
- **n=3**, not WP2's n=2.
- Same model, same prompt, same seed, 64 tokens.
- `memory.swap.max = 0` (I-3). `drop_caches` before every arm A run with the
  Campaign 12 Phase A guard active.

If arm A cannot run at all at `memory.max = budget` — the process needs headroom
for the binary, libc, and the KV cache beyond the weight region — **report the
exact failure and then use the smallest margin that lets every arm run, applied
identically to every arm.** State the margin in the report and in every table
that uses these numbers. Do not use a different margin per arm.

Report per cell: `read_bytes`, total fetches, demand faults, tokens/s,
time-to-first-token, p99 inter-token latency, `memory.peak`, and for arm A the
achieved bandwidth with a flag if it exceeds 3396 MiB/s.

### Pre-registered expectations

1. Arm D beats arm A on bytes at every ratio, including r=0.25 where the
   confounded comparison previously showed parity.
2. Arm C remains at ~100% miss at every ratio.
3. The tokens/s scaling result holds: arms A and C flat with budget, arm D
   scaling with it.
4. Arm D's byte advantage at r=0.5 and r=0.75 is **larger** than WP2's 24%/51%,
   since the baseline no longer has extra memory.

**Write `results/final/phase1_equal_budget.md`.**

---

## PHASE 2 — The exact consumption signal · box: 90 min

### Why

Session 2's WP0 fix was derived from evidence, not from a spec (BLOCKER 2 — the
spec file never reached the machine). It protects `seq[pos]` and `seq[pos-1]`
from eviction. It works, and it has a measured cost: D/OPT regresses 1.06 → 1.15
at 128 MiB/r=0.25, because protecting 2 of only 4 budget chunks is expensive.

The intended fix was different, and addresses the same root cause from the other
end.

**The diagnosis:** `pager_notify_access()` fires **once per reference from driver
thread 0 only**. With N driver threads, a chunk is not consumed when thread 0
finishes it — it is consumed when all N have. So the cursor advances early, the
policy correctly identifies the chunk at the old cursor as furthest-future, and
evicts a chunk the other N-1 threads are still reading.

**The policy was behaving correctly on incorrect input.**

The driver already has the exact signal: A-10's lookahead window tracks
`completed[s]`, the count of threads that have finished step `s`.

### Task

Add `--consumption-signal {tid0,all-threads}`, default `tid0` (current
behaviour, so nothing changes until asked).

Under `all-threads`, `pager_notify_access()` for step `s` fires when
`completed[s] == n_threads` — from whichever thread performs the increment that
reaches it. Exactly once per (pass, chunk); assert this.

At `--driver-threads 1` the two modes must be identical.

Then test the four combinations:

| Mode | `lo_declared_dist` protection | Purpose |
|---|---|---|
| `tid0` | on (current) | Current default, the baseline for this comparison |
| `tid0` | off | Session 1's behaviour, the known-broken case |
| `all-threads` | off | **The intended fix, alone** |
| `all-threads` | on | Both together |

Add `--protect-current {on,off}` to control the WP0 heuristic, default `on`.

**Grid:** WP1 §1.3's A.2 six cells (n=5, determinism), plus WP1 §1.4's grid at
8 MiB and 128 MiB × r{0.25, 0.5, 0.75} × compute{0, 400000}, arm D, n=3.

Report `absent_handled` per rep for the determinism cells, and `read_bytes` and
D/OPT for the sweep.

### Pre-registered expectations

1. `all-threads` + protection **off** is deterministic at all six A.2 cells.
   This is the claim: the exact signal alone is sufficient.
2. `all-threads` + protection off recovers the D/OPT regression at
   128 MiB/r=0.25/c=0 (1.15 → ~1.06 or better).
3. `all-threads` + protection off is at or below `tid0` + protection on on bytes
   at every cell.
4. `tid0` + protection off reproduces session 1's broken numbers, confirming the
   comparison is wired correctly.

**If expectation 1 holds, the heuristic is unnecessary** — report that, set the
default to `all-threads` + protection off, and re-run T-1..T-7. If it does not
hold, keep both and report what each contributes. **Do not invent a third
mechanism either way.**

**Write `results/final/phase2_consumption_signal.md`.**

---

## PHASE 3 — Arm E's collapse at tight budget · box: 60 min

### Why

`wp2_llamacpp.md` records arm E timing out at 360 s at r=0.25 on the real model,
both reps, with no completion. The diagnosis offered: prefetch pinned-retention
plus the WP0 current-chunk protection leave too few evictable chunks, and demand
fetches spin in `handle_absent`'s `ensure_budget` retry loop.

A hang is not an acceptable failure mode and it currently has no measurement
behind it.

### Task

**Characterise first, then apply the pre-decided mitigation. Do not design a new
one.**

1. Reproduce the collapse with `--fetch-trace` and `--policy-trace` active, plus
   a 120 s watchdog dumping `/proc/PID/task/*/wchan` on timeout, as item 10b did.
   Report: how many chunks are pinned at the hang, how many are RESIDENT and
   unpinned, `stat_pin_broken`, `infeasible`, and what the threads are blocked
   on.

2. Then test these three configurations at r=0.25, n=3, and report which complete
   and at what cost:
   - `--prefetch-retention none`
   - `--prefetch-depth 1`
   - `--prefetch-retention none --prefetch-depth 1`

3. If Phase 2 made `--protect-current off` the default, test whether that alone
   resolves it.

**Pre-decided outcome:** whichever configuration completes with the lowest
`read_bytes` becomes arm E's configuration at tight budgets, and that is recorded
as a **stated operating limit of the prefetch mechanism**, not hidden. If none
completes, report that arm E has no viable configuration at r=0.25 on this model
and say so plainly in the results table.

**Write `results/final/phase3_arm_e_collapse.md`.**

---

## PHASE 4 — Final figures, tables, and claims · box: 90 min

Regenerate everything from the final data. `make_wp3.py` already exists — extend
it, do not rewrite it.

### Figures

Figures 1-6 as specified in `WP3.md`, regenerated with:
- Phase 1's equal-budget arm A numbers.
- Phase 2's final policy configuration.
- Five ratios where available.
- Phase 3's arm E configuration at tight budgets, marked as a different
  configuration on the figure.

**Add Figure 7 — throughput scaling.** X: budget ratio. Y: tokens/s. Lines for
arms A, C, D, E on the real model. This is the project's strongest single result
— arms A and C flat while D scales 1.1 → 2.8 — and it currently exists only as a
bullet in a summary. It deserves its own figure.

### Tables

Table 1 (main results) and Table 2 (environment) regenerated. Every cell that is
excluded, contaminated, non-deterministic, or measured under a different
configuration carries a footnote key.

### CLAIMS.md

Update all eight claims plus:

- **Claim 9:** the kernel cannot convert additional memory into throughput on a
  cyclic layer scan; an application-authoritative pager can. Evidence: Phase 1's
  tokens/s column, Figure 7.

For every claim, the Caveats field must state honestly: WSL2-only, one model,
CPU-only, host-cache contamination where it applies, and any non-deterministic or
excluded cells.

**Write `results/final/phase4_figures.md`** as an index.

---

## PHASE 5 — Writeup data package · box: 60 min

The project stops measuring after this. Produce what the report needs.

Write `results/final/WRITEUP_PACKAGE.md` containing:

1. **The abstract's numbers**, each with its source cell: byte reduction range,
   throughput scaling, D/OPT, arm C miss rate, the reclaim-authority counters.
2. **A results narrative skeleton** — the order the results should be presented
   in for the argument to build, one line per result, no prose. Start from kernel
   LRU thrashing, end at throughput scaling.
3. **Every number that must NOT be cited**, with why. Pull from PROJECT_STATE §6.
   Someone writing at 2am needs one list of what is dead.
4. **The methodology section's content**, as bullet points: the arm design, the
   OPT bound and why it is computable, the metrics and why `read_bytes` is
   primary, the correctness harness, the pre-registration discipline.
5. **The limitations section's content**, as bullet points, from PROJECT_STATE §3
   — deduplicated, ordered by how much a reviewer would care.
6. **The five negative results worth reporting**, with what each taught: the
   admission rule declining nothing, the hit-rate metric artifact, the synthetic
   prefetch finding not transferring, the barrier making async untestable, and
   arm E's collapse.

Also update `results/PROJECT_STATE.md` §1, §2, §3, §5, §6 with this session's
results, following the existing structure.

---

## WHAT NOT TO DO

- Do not open a new investigation. If something surprising appears, record it in
  the phase report and move on.
- Do not add any mechanism: no new policy, prefetch variant, retention mode, or
  admission rule.
- Do not change arm definitions, the reference-trace format, or T-1..T-7.
- Do not attempt bare metal or a second model.
- Do not modify anything under `/root/spike/`.

## CORRECTNESS

Re-run T-1..T-7 with `--eager-reconcile` after Phase 2's changes and again at the
end of the session. A T-7 failure is a hard stop.

## FINAL CHECK

Each phase file confirms: no number estimated or inferred; no test weakened to
pass; every pre-registered expectation checked against a measured value; every
gate evaluated with failures reported as failures.

End the session with `results/final/FINAL_SUMMARY.md` — what completed, what did
not, what changed, and what remains open and permanently unaddressed.