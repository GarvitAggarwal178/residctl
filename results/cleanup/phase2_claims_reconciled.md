# CLEANUP Phase 2 — CLAIMS.md reconciled

**Box:** 45 min. **Documentation only** — no code, no measurements.

`results/overnight/CLAIMS.md` carried a "FINAL SESSION amendments" header at the
top with claim bodies below it that still said pre-final-session things. A reader
who reads a body and not the header gets stale numbers. This pass rewrote every
body to be correct standalone, deleted the header, kept (and expanded) every
`Superseded prior claims` field, and added Claim 10.

---

## Edits, per claim

### Header
- **Deleted** the "FINAL SESSION amendments (2026-08-28) — read these first"
  block (lines 9–55). Its content is now distributed into the claim bodies and
  `Superseded prior claims` fields.
- **Added** a one-paragraph "Policy-default note": on the real model
  (`residctl_llama`) `layer_order_declared` runs `protect_current` **on** (its
  notify lags); on the synthetic driver (`replay_main`) `--consumption-signal
  all-threads` makes it redundant so `--protect-current` defaults **off**. This
  is the single fact most claims now depend on.

### Claim 1 (kernel LRU thrashes)
- Caveat "WP2 would have confirmed — not run (BLOCKER 1)" → **replaced** with
  real-model evidence: arm C reads 134–144 GB at every ratio on Qwen2.5-3B,
  r=0.75 included, byte-identical run to run. Added Figure 6.
- `Superseded`: added the retired "synthetic-only" caveat.

### Claim 2 (app-authoritative residency scales)
- Body rewritten: split into synthetic evidence (`phase2_sweep.csv`,
  `all-threads`+off, D/OPT 1.00 at 8 MiB / 1.00–1.08 at 128 MiB, both computes,
  deterministic) and real-model evidence (`phase1_equal_budget.csv`,
  `protect_current` on, D beats C by 12–68 % and A by 2–48 %).
- Removed the "compute=400000 non-determinism" caveat (fixed).
- `Superseded`: kept the learned-policy 1.07–1.78 row; added the session-1
  "declared non-deterministic / 1.9× OPT" row.

### Claim 4 (OPT is computable; distance is regime-dependent)
- **Retitled** from "…both policies are measurably distant from it" to
  "…how far a policy sits from it depends on the policy and the regime."
- Body: three explicit distances (learned 1.06–1.53; declared synthetic
  1.00–1.08; declared real 1.09–1.14). Added `wp2_opt.c` unequal-chunk method.
- `Superseded`: unchanged in substance.

### Claim 6 (prefetch)
- **Retitled** to "…trades bytes for latency, and only at a loose enough budget;
  it is a net loss at a tight budget."
- Body: added real-model evidence — r ≥ 0.5 E reads 7–13 % more (bytes-for-
  latency); r ≤ 0.375 E is a net loss (livelocks with `protect_current` on;
  completes but 37–43 % worse with it off).
- Caveat "not run (BLOCKER 1)" → **deleted**; added the arm-D operating
  recommendation.
- `Superseded`: added (b) the retired BLOCKER-1 caveat and (c) the "hard
  deadlock" framing → livelock.

### Claim 7 (declared order)
- **Retitled** to "…given an accurate consumption signal, reaches the Belady
  optimum."
- Body rewritten: synthetic (all-threads, all six A.2 cells deterministic incl.
  cell 3, D/OPT ≤ heuristic everywhere) vs real model (`protect_current` on,
  D/OPT 1.09–1.14).
- Caveats rewritten: (1) determinism only where the signal is exact; the
  real-model heuristic is load-bearing. (2) arm E is a net loss / livelocks at
  r ≤ 0.375.
- `Superseded`: expanded to three rows — (a) session-1 Claim 7, (b) the
  session-2 "128 MiB 1.06 → 1.15 regression" (removed by all-threads on the
  synthetic path), (c) the session-2 "heuristic is unnecessary" (true synthetic,
  **not** real — reverted in Cleanup Phase 1).

### Claim 8 (real model)
- Body: added the final session's Phase 1 (equal budget) alongside WP2. D beats
  A at every ratio; throughput scales.
- Caveat (2) "arm D ≈ arm A at r=0.25" → **moved to `Superseded`** (it was the
  +256 MiB margin).
- Caveat (3) "arm E collapsed at r=0.25" → reworded (livelock; operating limit).
- `Superseded`: added (b) WP2's "D ≈ A" and (c) Phase 3's "hard deadlock".

### Claim 9 (throughput scaling)
- Body: noted the numbers are `protect_current` on (the real-model default).
  Otherwise unchanged — Phase 1 measured this cleanly.

### Claim 10 — **NEW**
"Two individually-correct mechanisms … combine into a pathological failure at a
tight budget, found by instrumentation and diagnosed from thread state."
Evidence: the Cleanup Phase 1 420 s counter trace + gdb. States it is a
**livelock** (not the hard deadlock Phase 3 reported), the mechanism, the
robustness fix that was retained (`pager_abandon_fetch` + FETCHING watchdog,
T-1..T-7 PASS), and that the livelock itself is mitigated by config, not fixed.
`Superseded`: Phase 3's "orphaned FETCHING slot / hard deadlock".

---

## WRITEUP_PACKAGE.md re-check (§1 and §3, plus §2/§5/§6)

| location | change |
|---|---|
| §1 "synthetic final policy config" row | added "(synthetic path only; the real model keeps `protect-current on`)" |
| §2 narrative point 5 | added the synthetic-vs-real signal distinction and the +67–78 % real-model figure |
| §2 narrative point 7 | "its default config deadlocks" → "with `protect_current on` it livelocks … with it off it reads 37–43 % more"; added Claim 10 ref |
| §3 dead list | "arm E collapsed = wrong mechanism / Phase 3 latent deadlock" row rewritten to "livelock, ~90× amplification, steady progress"; **new row**: "the WP0/protect-current heuristic is unnecessary" is synthetic-only |
| §3 dead list | "declared worse than learned" row — scoped to the synthetic path |
| §5 limitation 4 | rewritten: determinism guarantee holds only where the signal is exact; real-model heuristic load-bearing |
| §5 limitation 5 | "deadlocks" → "livelocks (~90× amplification, not a hard deadlock)"; noted the watchdog fix is not a fix for the livelock |
| §6 negative result 5 | "combine into a deadlock" → "combine into a livelock"; added the "SIGUSR1 dump misread idle-vs-churning" lesson |

Numbers checked and **unchanged** (they were already right): the 12–68 % /
2–48 % byte reductions, D/OPT 1.09–1.14 (real) and 1.00–1.08 (synthetic), the
2.1× throughput scaling, arm C's 134–144 GB, the reclaim counters, the model
hash. No abstract number moved — only the framing of the arm-E failure and the
scope of the "heuristic unnecessary" claim.

---

## Final check

- Every claim body is now correct without the deleted header.
- Every `Superseded prior claims` field survived and several grew.
- No measurement was taken; no code changed.
- The one substantive correction (arm-E failure is a livelock, not a hard
  deadlock; the real-model heuristic is load-bearing) is carried consistently
  into CLAIMS.md, WRITEUP_PACKAGE.md, and `phase1_deadlock_fix.md`.
