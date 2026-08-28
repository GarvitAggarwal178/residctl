# FINAL SESSION — Summary

The last measurement session. After this the project moves to writeup;
`results/final/WRITEUP_PACKAGE.md` is the reference for that.

Spec: `docs/overnight/final session.md` (the prompt named `docs/final/FINAL_SESSION.md`;
it does not exist at that path — BLOCKERS.md NOTE 1). Commits `aff3e02` → `<P5>`,
all pushed to `origin/main`. Machine exclusive throughout (own sweeps + a
lightweight 90 s-poll supervisor; no foreign workloads; `pgrep cn-spike|iperf3|gate5`
clean before/after every phase).

---

## What completed

| Phase | Box | Status | Report |
|---|---|---|---|
| 1 — equal-budget baseline | 90 min | **complete** (~2 h; arm E's 360 s r=0.25 timeouts) | `phase1_equal_budget.md` |
| 2 — the exact consumption signal | 90 min | **complete** (~110 min) | `phase2_consumption_signal.md` |
| 3 — arm E's collapse | 60 min | **complete** (~90 min) | `phase3_arm_e_collapse.md` |
| 4 — figures, tables, claims | 90 min | **complete** | `phase4_figures.md` |
| 5 — writeup data package | 60 min | **complete** | `WRITEUP_PACKAGE.md` + PROJECT_STATE §§1–6 |

Every phase overran its box (background-task interrupts forced frequent
resume-from-checkpoint cycles) but stayed within 2×. No phase consumed the
session.

## What did not, and why

- **No hard-stop condition triggered.** No T-7 failure; no OPT below the cyclic
  floor (every `wp2_opt` / `belady_main` run floor-checked and passed); no
  `reconcile()` divergence; no unfixed build failure; disk never below 10 GiB
  (923 GiB free throughout).
- **The arm E deadlock is not fixed** — Phase 3 characterised it and applied the
  pre-decided mitigation (`--protect-current off`, now the default); the spec
  capped Phase 3 at "apply the pre-decided mitigation, do not design a new one".
  Recorded as BLOCKERS.md FINDING 1 (a 7th concurrency-class issue) for a future
  session.
- **The `--fetch-trace` / `--policy-trace` files are empty for the collapsed
  runs** — they flush only in `residctl_llama_teardown()`, which `timeout`'s
  SIGTERM skips. The SIGUSR1 residency dump (straight to stderr) carried every
  quantity Phase 3 needed, so this did not block the characterisation.
- **Phase 1 was measured with `--protect-current on`** (the pre-Phase-2 default).
  Phase 3 checked the flipped default on the real model at r=0.25 (arm D/E) and
  found it resolves the collapse and does not materially change arm D's bytes;
  Phase 1's arm D numbers stand. A full re-sweep of Phase 1 at the new default
  was out of scope (time box).

## New results, one line each

- **Equal budget removes WP2's confound: arm D beats arm A on bytes at every
  ratio** — D/A = 0.98 / 0.89 / 0.71 / 0.58 / 0.52 (`phase1_equal_budget.csv`).
- **D/OPT = 1.09–1.14 on the real model at all 5 ratios** (`phase1_opt.csv`).
- **Kernel arms cannot convert memory into throughput** — arm A/C flat at
  0.6–0.9 t/s across a 3× budget range; arm D scales 0.91 → 1.95 t/s (Claim 9,
  Figure 7, `phase1_equal_budget.csv`).
- **The exact `all-threads` consumption signal makes `layer_order_declared`
  deterministic at all six Campaign-13-Phase-A cells** (`phase2_determinism.csv`)
  — the session-2 heuristic never fixed cell 3.
- **128 MiB D/OPT regression recovered 1.15 → 1.08 / 1.04 / 1.00** by the exact
  signal, both compute levels (`phase2_sweep.csv`).
- **Arm E's r=0.25 collapse is a latent deadlock**, not budget pressure —
  `resident_bytes` 148–403 MiB of a 526 MiB budget at the hang, orphaned
  `FETCHING` slot (`phase3_log.txt` SIGDUMP).
- **`--protect-current off` (Phase 2's default) alone resolves the collapse**;
  arm E has no advantageous config at r ≤ 0.375 (best: 166.5 GB > arm D's
  126.1) (`phase3_arm_e.csv`).

## What changed in the code

| file | change | kind |
|---|---|---|
| `src/replay.c` / `.h` | `--consumption-signal {tid0,all-threads}`: `pager_notify_access()` fires on full-thread step completion, routed through A-10's `completed[]` counter; exactly-once asserted. | fix (behaviour) |
| `src/policy.c` / `.h` | `policy_set_protect_current()` / `_get_` — toggles the WP0 heuristic (`--protect-current`). | feature (toggle) |
| `src/replay_main.c` | parse both flags; **defaults flipped to `all-threads` + protect-off**. | fix (default) |
| `src/residctl_llama.c` | `fetchtrace=`/`policytrace=`/`protect_current=` config keys; SIGUSR1 residency dump; **`protect_current` default off**. | instrument + default |
| `src/test_policy.c` | protect-on assertions pinned explicit; new protect-off assertion block. | test |
| `results/overnight/make_wp3.py` | `figure6_final`, `figure7`, `table1_final`, `table2_final` added; Figures 3/4/5 + original tables untouched. | analysis |
| `src/run_final_phase{1,2,3}*.sh`, `phase{1,2,3}_supervisor.sh`, `smoke_p2.sh`, `p1progress.sh` | new scripts. | instrument |

No change to any mechanism decision function for `lru` or `layer_order_learned`
— verified by the WP1 §1.2 gate and T-1..T-7 (all PASS after every change, twice:
after the Phase 2 flip and at session end).

## Numbers now superseded (for PROJECT_STATE §6 — done)

- Session-2 WP0 "protect-current" heuristic → superseded by `--consumption-signal
  all-threads` (Phase 2).
- WP2 "arm D ≈ arm A at r=0.25" → superseded by Phase 1 equal budget.
- WP2's mechanism for arm E's collapse ("too few evictable chunks / `ensure_budget`
  spin") → superseded by Phase 3 (latent deadlock, budget not binding).
- PROJECT_STATE §3's "residual mild non-determinism at cell 3" → closed by Phase 2.

## What the next session should do first

1. **Fix the arm E deadlock** (BLOCKERS.md FINDING 1) — make the dedup /
   `ensure_budget` path escalate to `infeasible` or reclaim the orphaned
   `FETCHING` slot instead of spinning. Then re-run arm E at r ≤ 0.375 to see if
   prefetch can be made *neutral* there (it will not become advantageous).
2. **Re-sweep Phase 1 at the new default** (`--protect-current off`) for a fully
   consistent real-model table — Phase 3 showed it resolves the collapse and does
   not move arm D materially, but the n=3 medians should be taken under the
   shipping config.
3. **A second, larger model (7B)** — test whether D/OPT stays ~1.1 and whether
   the name-lex tensor order / split-layer quirks generalise.
4. **Supply `docs/overnight/WP0_FIX_AND_MODEL.md`** — Phase 2 implemented what the
   missing spec's "option 1" (driver-side fix) described; confirm that was the
   intent and that flipping the operational default (vs. keeping both) is
   acceptable.
5. **Bare metal** (`BARE_METAL_PLAN.md`) — the one structural gap no WSL2 session
   can close; every I/O-ceiling and host-cache caveat depends on it.

## Correctness — final

- T-1..T-7 with `--eager-reconcile`: **PASS** after the Phase 2 default flip, and
  **PASS again at session end** (`correctness_harness_log.txt`, `t6_t7_log.txt`).
  `mismatches = 0`, no lost fault, T-6 `dedup_fetching > 0`.

## Final check (per the spec)

Each phase file confirms: no number estimated or inferred; no test weakened to
pass (a test was *added*); every pre-registered expectation checked against a
measured value; every gate evaluated with failures reported as failures (arm E's
hang is reported as a hang with its mechanism; expectation 4 at r=0.75 is
reported as "did not hold").
