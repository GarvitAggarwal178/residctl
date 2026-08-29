# LIVELOCK FIX — Phase 4: propagate the changed numbers

The LIVELOCK FIX moved real-model numbers (Phase 3) and flipped the
`residctl_llama.c` `protect_current` default to `off` (A-14). The synthetic path
is byte-identical (Phase 2). This phase propagates the deltas into every
downstream artifact.

## Code

`src/residctl_llama.c`: `g_protect_current` default `1 → 0`. Comment rewritten
(the cleanup-session "+67–78 % for protect-off" is a Defect-2/Defect-4 artifact;
Phase 3c: on vs off ≤ 1.8 %). Rebuilt `wp2_gen`.

**Flip smoke**: arm D, r=0.5, config with **no** `protect_current` key → uses
the new default. `pager_bytes_fetched = 78 895 448 064` — byte-identical to
Phase 3's explicit `protect_current=off` run. Startup audit: "all 40 declared
chunks signalled within 2 passes". PASS.

**Regression gates** (post-flip): `test_policy` PASS (both signal modes);
T-1..T-5 (`run_correctness_harness.sh`) PASS; T-6/T-7 (`run_t6_t7.sh`) PASS,
`mismatches = 0`.

## Figures & tables

| artifact | change | source |
|---|---|---|
| Figure 1, 2 | **unchanged** — synthetic; Phase 2 proved the `layer_order_declared` all-threads path byte-identical | (not regenerated; `git diff` empty) |
| Figure 3, 4, 5 | **unchanged** — chunk-size sweep / reclaim authority / synthetic prefetch; the fix touches none of them | (not regenerated) |
| Figure 6 | arms C/D/E repointed to `phase3_real_model.csv` (all four fixes, `--protect-current off`); arm A stays `phase1_equal_budget.csv` (no pager). Arm E r=0.25 is now a normal completing point — the "fallback / collapses" asterisk is gone. D/OPT 1.09–1.14 → **1.08–1.13** | `make_wp3.py figure6_final` |
| Figure 7 | same repoint. Arm D **1.12 → 2.85 t/s (2.5×, monotone)**; arm C **~1.0 t/s**; arm E now has an r=0.25 point | `make_wp3.py figure7` |
| Table 1 | same repoint; arm-E r ≤ 0.375 note: "completes (was a livelock with protect_current on); prefetch non-advantageous vs D here" | `make_wp3.py table1_final` |
| Table 2 | policy-default row → `--protect-current off` on both paths; prefetch-arm-E row → "completes at every ratio (LIVELOCK FIX)" | `make_wp3.py table2_final` |

`make_wp3.py` gained `_p3rows()` / `_p3cell()` and an argv selector so individual
figures can be regenerated without touching the synthetic ones. OPT is the
canonical **65-pass** column throughout (prompt scan + 64 decode scans) — the
Phase 3 analyze script was corrected from 64-pass to match.

## Docs

- **`CLAIMS.md`**: Policy-default note rewritten (`protect_current off` both
  paths, load-bearing story retired). Claims 2, 4, 7, 8, 9 carry the new
  real-model numbers. Claim 6: arm E completes at r ≤ 0.375, reads ~13 % more
  than D (latency win, not a loss); livelock fixed. **Claim 10 retitled and
  rewritten** — no longer "two individually-correct mechanisms combine into a
  pathological failure"; it is an off-by-one distance origin plus a post-compute
  consumption signal, both fixed, verified against the exact prior-livelock
  config (Phase 3b). A-13 kept as the independent fix for the orphaned-slot
  class. Every superseded number retained in its `Superseded prior claims`
  field.
- **`WRITEUP_PACKAGE.md`**: §1 abstract table; §2 narrative points 5 and 7; §3
  dead list (three rows rewritten: the protect-current "load-bearing" row, the
  arm-E-collapse row); §4 methodology (T-1..T-7 + the startup audit); §5
  limitations 4 and 5; §6 negative result 5.
- **`PROJECT_STATE.md`**: new §1 LIVELOCK FIX session block; §3 both
  `layer_order_declared` bullets; §5 spec amendment **A-14**; §6 three new
  old → new → why rows.
- **`docs/design-history.md`**: the "settled in Phase 4 / A-14" line resolved.

## What did NOT move

- The synthetic path (`replay_main`): every `layer_order_declared` all-threads
  cell byte-identical to `results/final/phase2_*.csv` (Phase 2).
- Arm A on the real model (no pager; `phase1_equal_budget.csv` retained).
- Claims 1, 3, 5 and Figures 2–5; Table 2's environment rows other than the two
  policy rows.
- `layer_order_learned`, `lru`, `select_victim`'s comparison logic,
  `INT64_MAX` semantics — untouched (spec constraint).
