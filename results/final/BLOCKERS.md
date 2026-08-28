# FINAL SESSION — Blocker Log

Genuinely-undecidable decisions and hard-stop conditions, with full context.
Empty of hard blockers is the goal; notes recorded here do not stop work.

---

## NOTE 1 — spec file path differs from the prompt

**When:** session start.

**Prompt says:** "Read docs/final/FINAL_SESSION.md and follow it end to end."

**Observed:** `docs/final/` does not exist. The spec is present as
`docs/overnight/final session.md` (untracked, added since commit `e1b7b43`).
Content is complete and unambiguous — 5 phases, pre-registered expectations,
pre-decided answers throughout. Proceeded from that file. Not a blocker.

---

## DECISION 1 — Phase 1 equal-budget: residctl arms cannot run at memory.max == B

**When:** Phase 1 setup.

**Context:** Phase 1 requires `memory.max` exactly equal to the budget `B`
(= ratio × weight-region) for every arm, no margin, with the pre-decided
fallback: "If arm A cannot run at memory.max = budget … use the smallest
margin that lets every arm run, applied identically to every arm."

**Probed (2026-08-28, machine idle):**
- Arm A (pure kernel mmap) **runs fine at `memory.max == B`** at every ratio
  (r=0.25: rc=0, `oom_kill=0`, completes in 87 s reading 123 GB). The kernel
  reclaims file-backed weight pages under pressure; arm A's own non-weight
  anon (~50 MiB: KV cache, compute buffers) stays resident within B, so arm
  A's effective *weight-cache* ceiling is ≈ B − 50 MiB.
- Arms C/D/E (residctl pager) **OOM-kill at `memory.max == B`** with
  `budget_bytes == B` — llama.cpp's ~50–90 MiB non-weight footprint is on
  top of the pager's B bytes of resident weights.
- Residctl arms with `budget_bytes = B − 96 MiB` (shrink the pager budget
  instead of adding a margin) **collapse at r=0.25** — the pager is starved
  and generation times out (rc=124), same failure mode as arm E's collapse.
- Residctl arms with `budget_bytes = B`, `memory.max = B + 128 MiB`
  complete cleanly at every probed ratio (`oom_kill=0`, `memory.peak` ≈
  B + 96 MiB).

**Decision (consistent with the fallback's intent):** the quantity equalised
across arms is the **weight-residency ceiling**, which is `memory.max` for
arm A but `budget_bytes` for the pager arms.
- Arm A: `memory.max = B`.
- Arms C/D/E: `budget_bytes = B`, `memory.max = B + 128 MiB`. The 128 MiB is
  identical for every residctl arm and every ratio (not per-arm-per-ratio),
  and covers only llama's non-weight memory — which arm A also has, absorbed
  within its own B.

**Residual asymmetry, disclosed:** the residctl arms get ≈ 50 MiB **more**
weight-cache room than arm A (≤ 10 % of B at r=0.25, ≤ 3 % at r=0.75). This
is the **opposite direction** and ≈ 4× smaller than WP2's original confound
(`memory.max = B + 256 MiB` for every arm, which favoured arm A). Every
Phase 1 byte-comparison is therefore modestly *optimistic* for the residctl
arms at r=0.25 and essentially unaffected at r ≥ 0.5 — noted in
`phase1_equal_budget.md` and every downstream table.
