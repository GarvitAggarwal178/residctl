# Overnight Session Summary — Session 2 (2026-08-28)

Session 2 followed the session-2 prompt: WP0 (model + consumption-signal
fix) → WP2 (llama.cpp) → WP3 (refresh). All commits pushed to `origin/main`
(`2b5b2ee` → `c844a75`, 8 commits). Machine exclusive throughout; final
`uptime` load 0.11, 923 GiB disk free, no stray processes or cgroups.

**Session 1's summary is preserved below this section.**

---

## What completed

### WP0 — model acquisition + consumption-signal fix
Spec file `docs/overnight/WP0_FIX_AND_MODEL.md` **does not exist on disk**
(BLOCKER 2). Acted on the decidable parts:

| Part | Status |
|---|---|
| §0.1 model download | **complete** — `models/model.gguf` = Qwen2.5-3B-Instruct Q4_K_M, 2,104,932,768 B (1.96 GiB), sha256 `626b4a66…62d`, via `curl` from Hugging Face. Resolves session-1 BLOCKER 1. |
| consumption-signal fix | **implemented, evidence-driven** (commit `8c15d8b`) — no spec, so the minimal version: `lo_declared_dist()` returns 0 for the actively-consumed chunk `seq[pos]` and `seq[pos-1]`. Touches **only `layer_order_declared`**. Forced by WP2's data (arm D deterministically read 216.6 GB at r=0.25, 1.5× arm C, pre-fix). |

### WP2 — llama.cpp integration — **COMPLETE (all 5 phases)**
Report: `results/overnight/wp2_llamacpp.md`

| Phase | Status | Result |
|---|---|---|
| 2.0 model + build | complete | llama.cpp CPU-only; `llama-bench` reference 87.9 pp / 13.2 tg tokens/s. Gate passed. |
| 2.1 chunk table | complete | 41 chunks from 435 tensors / 36 layers; **finding: GGUF tensors in name-lex not layer order, layer 21 split across 2 chunks**. `fetch.c` gained the §6.1 EOF-slack path (first exercise); `budget.c`/retention needed no change. `wp2_opt.c`: OPT bytes = Σ missed chunk sizes, floor-checked. |
| 2.2 wire pager into loader | complete | `llama-mmap.cpp` patched (~35 lines, dlsym hook). **CORRECTNESS GATE: PASS** — byte-identical tokens `--load-mode mmap` vs `residctl`. Re-verified post WP0 fix. |
| 2.3 measurement | complete | Sweep A/C/D/E × {0.25,0.5,0.75}, n=2 (time box). See "New results". |
| 2.4 synthetic vs real | complete | Real per-layer compute ≈ 51 k ns/MiB — ~1/30 of the synthetic heavy setting. |

### WP1 §1.4 — re-swept with the WP0 fix
Report: `wp1_declared_order.md` (session-2 amendment box + new §1.4 section).
The fix makes `layer_order_declared` **Belady-optimal (D/OPT = 1.000) and
deterministic at all six 8 MiB cells** (session 1: 1.9× OPT + non-det at
compute=400000). T-1..T-7 re-run after the fix — all PASS. WP1 §1.2 gate
unchanged.

### WP3 — figures/tables refresh — **COMPLETE (6 of 6 figures)**
Report: `wp3_figures.md`. Figures 1–2 regenerated from post-fix WP1 data;
**Figure 6 produced** (WP2 real model); Table 1, Table 2, `CLAIMS.md`
updated (Claims 7 & 8 now both supported).

## What did not, and why

- **`docs/overnight/WP0_FIX_AND_MODEL.md` / `WP2_LLAMACPP.md` /
  `WP3_FIGURES.md` do not exist** (BLOCKER 2). Proceeded from the
  session-2 prompt's stated intent + the existing `WP2.md` / `WP3.md`.
- **Arm E collapsed at budget ratio 0.25** on the real model — 360 s
  timeout, both reps. Prefetch pinned-retention + the WP0 current-chunk
  protection over-constrain the tightest budget. Recorded; not re-run.
  (BLOCKERS.md.)
- **WP2 n=2 not n=3** — the arm-E-at-r=0.25 collapse and cold arm-A loads
  were eating the time box; arms C/D are perfectly deterministic, A varies
  ~10%.
- **The driver-side consumption-signal fix** ("notify = finished", broader
  blast radius across arm E and `replay.c`) was not tried — the policy-side
  fix was chosen for minimal blast radius (BLOCKER 2).
- **No hard-stop conditions triggered** — no T-7 failure, no OPT below
  floor (`wp2_opt` floor-checks pass), no `reconcile()` divergence, no
  unfixed build failure, disk never below 10 GiB.

## New results, one line each

- **Correctness gate PASS** — 32 tokens, `--load-mode mmap` vs `residctl`,
  byte-identical (`wp2_gate_log.txt`). The pager serves correct weight data
  for a real transformer.
- **Kernel LRU (arm C) thrashes on the real model at every budget ratio
  including 0.75** — ~100% miss, 134–144 GB / 64 tokens, ~1 tokens/s
  (`wp2_sweep.csv`). Exactly the synthetic degeneration.
- **Arm D (declared order) reads 24% / 51% fewer bytes than the kernel
  (arm A) at r = 0.5 / 0.75** on the real model; D/OPT = 1.09–1.14
  (`wp2_sweep.csv`, `wp2_opt.csv`, `figures/figure6_llamacpp.csv`).
- **Arm E reads 7–9% MORE bytes than arm D at every ratio** but halves
  demand faults and p99 inter-token latency — prefetch trades bytes for
  latency on the real workload (`wp2_sweep.csv`).
- **The synthetic "prefetch beats D on bytes under a compute phase"
  finding does not transfer** — real per-layer compute (~51 k ns/MiB) is
  ~30× lighter than the synthetic heavy setting (`wp2_llamacpp.md` §2.4).
- **The WP0 fix makes `layer_order_declared` Belady-optimal (D/OPT =
  1.000) and deterministic at all six 8 MiB WP1 §1.4 cells**, both compute
  levels — reversing session 1's compute=400000 regression
  (`wp1_sweep.csv`, `wp1_sweep_analysis_after_fix.txt`).
- **The WP0 fix eliminates the Campaign 13 Phase A non-determinism** —
  WP1 §1.3 cells 5–6: non-deterministic (79–90 / 1316–1388) → deterministic
  (55 / 768, the exact 8 MiB/r=0.5 Belady floor) (`wp1_determinism.csv`).
- **The real GGUF's tensors are stored in name-lexicographic, not layer,
  order; layer 21 is split across two non-contiguous file chunks**
  (`wp2_tensor_inventory.txt`).

## What changed in the code

| File | Change | Kind |
|---|---|---|
| `src/policy.c` `lo_declared_dist()` | protect `seq[pos]` and `seq[pos-1]` (return 0). | **fix** (WP0) — `layer_order_declared` only |
| `src/region.h` / `src/region.c` | `residctl_chunk_spec_t` + `explicit_chunks` config + `build_chunk_table_explicit()`; `model_file_size` + `model_fd_buf`. | **new feature** (WP2) |
| `src/fetch.c` `fetch_read()` | O_DIRECT bulk + buffered sub-4096 tail + zero-fill for a chunk whose aligned end runs past EOF (§6.1 slack). Guarded so the replay path is unchanged. | **new feature** (WP2) |
| `src/residctl_llama.{c,h}` | GGUF parser, per-layer chunk table, pager bring-up, per-layer/role notify, stats. | **new** (WP2) |
| `src/wp2_gen.cpp` | minimal greedy generation + per-layer eval callback + timing/IO. | **new** (WP2) |
| `src/wp2_opt.c` | offline OPT for unequal chunk sizes over a declared sequence. | **new** (WP2) |
| `third_party/llama.cpp/src/llama-mmap.cpp` | ~35-line dlsym hook (`src/wp2_llama_mmap.patch`). | **instrument** (WP2) |
| `src/test_policy.c` | declared-policy tests + Belady cross-check updated to the protect-current semantics. | **test** |
| `src/run_wp2_{gate,sweep,smoke}.sh`, `setup_wp2_llama.sh`, `build_wp2.sh` | new scripts. | **instrument** |
| `results/overnight/make_wp3.py` | Figure 6, post-fix data, updated flags/captions. | **instrument** (analysis) |

No change to any mechanism decision function for `layer_order_learned` or
`lru` — verified by the WP1 §1.2 gate and T-1..T-7 (all PASS after every
change).

## Numbers that are now superseded (for PROJECT_STATE.md §6)

- **Session 1's WP1 "declared order is Belady-optimal at compute=0 but
  worse than the learned hedge under a concurrent compute phase" (D/OPT up
  to 1.9, non-deterministic at compute=400000).** Superseded by the WP0
  fix: `layer_order_declared` is now D/OPT = 1.000 and deterministic at
  every 8 MiB cell, both compute levels. Session-1 data preserved as
  `wp1_sweep_session1.csv`.
- **Campaign 13 Phase A's "the 5 non-deterministic arm D cells".** For the
  **declared** policy, the WP0 fix makes them deterministic (WP1 §1.3
  post-fix). The **learned** policy's 5 cells stay non-deterministic (that
  policy is unchanged).
- **PROJECT_STATE §3's "the synthetic cyclic-scan workload is the only
  workload ever tested".** Partially lifted — WP2 confirmed the core thesis
  (Claims 1, 2, 4) on Qwen2.5-3B; the prefetch-vs-compute finding (Claim 6)
  is contradicted on the real model.
- **BLOCKER 1** (model absent) — resolved.

## What the next session should do first

1. **Supply `docs/overnight/WP0_FIX_AND_MODEL.md`** (and the renamed
   WP2/WP3 files) — this session guessed at the consumption-signal fix
   from evidence; the intended fix, acceptance criteria, and whether the
   driver-side variant is preferred are unconfirmed (BLOCKER 2).
2. **Fix or characterise arm E's r=0.25 collapse** — try
   `--prefetch-retention none` or `--prefetch-depth 1` at tight budget, or
   make `handle_absent`'s `ensure_budget` retry loop break a prefetch pin
   instead of spinning. Then re-run WP2 arm E at r=0.25.
3. **WP2 at n=3 and more ratios** (0.375, 0.625) for a clean paper table;
   and a second, larger model (7B) to test whether D/OPT stays ~1.1.
4. **Decide the default policy** — `layer_order_declared` (with the fix) is
   now a clear win at realistic chunk counts; confirm the 128 MiB /
   r=0.25 / c=0 regression (D/OPT 1.06 → 1.15) is acceptable or worth a
   3-chunk-protection tweak.
5. **Re-sweep WP2 arm A with `memory.max` = D's budget** (not budget +
   256 MiB) so the arm A vs arm D comparison is at equal effective cache —
   the r=0.25 "D ≈ A" result is confounded by the margin.

---
---

# Overnight Session Summary — Session 1

## What completed

### WP1 — Declared access order (box 3h) — **COMPLETE, all 5 sub-phases**
Report: `results/overnight/wp1_declared_order.md`

| Phase | Status | Result |
|---|---|---|
| 1.1 Declaration interface | complete | `policy_declare_sequence` + `on_access`/`declare_sequence` hooks; `layer_order_declared` derives next-use distance from the declared sequence; Belady cross-check agrees. |
| 1.2 Naming + verification gate | complete | `layer_order` → `layer_order_learned` (+ alias); new `layer_order_declared` is default. **Gate PASS.** |
| 1.3 Determinism check | complete | Expectation 1 DID NOT HOLD (session 1) — declared non-deterministic at cells 5–6. **[Session 2: the WP0 fix makes cells 5–6 deterministic.]** |
| 1.4 Learned vs declared sweep | complete | Session 1: declared = OPT at compute=0, worse at compute=400000. **[Session 2: re-swept — declared = OPT at every 8 MiB cell, both computes.]** |
| 1.5 Correctness | complete | T-1..T-7 PASS. |

### WP2 — llama.cpp integration (box 6h) — **NOT STARTED (session 1)**
`models/model.gguf` absent — BLOCKER 1. **[Session 2: model acquired, WP2 completed.]**

### WP3 — Figures (box 2h) — **COMPLETE (5 of 6 figures, session 1)**
Figure 6 omitted (WP2 not run). **[Session 2: Figure 6 produced.]**

## Session 1 — new results, superseded numbers, next steps
See the git history at commit `2b5b2ee` and the section above for the
session-2 updates that supersede several of them.
