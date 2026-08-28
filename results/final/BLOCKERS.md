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

---

## FINDING 1 (Phase 3) — latent deadlock: prefetch + pinned-retention + protect-current

**When:** Phase 3, characterising arm E's r=0.25 collapse. **Recorded, not
pursued** (spec: "if a task reveals something new, record it and do not pursue
it"; Phase 3 caps at the pre-decided mitigation).

**Signature** (4 independent SIGUSR1 dumps at the hang, `protect_current=on`):
`resident_bytes` 148–403 MiB against a **526 MiB budget** (budget not binding);
2–3 RESIDENT + unpinned chunks present (victims available); `stat_infeasible=0`,
`stat_pin_broken=0`; **1 chunk stuck in `FETCHING` with every fetch worker parked
in `futex_do_wait`**; main thread blocked forever in `handle_userfault`.

**Mechanism (diagnosed, not fixed):** a demand fault deduped against an in-flight
prefetch waits on a `FETCHING` slot no worker owns, while `--prefetch-retention
pinned` holds 2 chunks pinned and `--protect-current on` shields the live chunk.
The `FETCHING` chunk never completes; the fault never resolves.

**Mitigation applied (pre-decided):** `--protect-current off` (Phase 2's new
default) alone resolves it; `--prefetch-retention none` resolves it with the
lowest `read_bytes`. See `phase3_arm_e_collapse.md`.

**For the next session:** a 7th concurrency-class issue (cf. the 6 in CLAUDE.md).
A fix would make the dedup / `ensure_budget` path escalate to `infeasible` or
reclaim the orphaned `FETCHING` slot instead of spinning. Out of scope here.
