# Overnight Session Summary

Session ran 2026-08-27 ~21:36 onward. Unattended. Work order WP1 → WP2 →
WP3 as instructed. All commits pushed to `origin/main`
(`622bbd8` → `16368a8`, 6 commits). Machine exclusive throughout — no
foreign workload at any check; final `uptime` load 0.00, 926 GiB disk
free.

## What completed

### WP1 — Declared access order (box 3h) — **COMPLETE, all 5 sub-phases**
Report: `results/overnight/wp1_declared_order.md`

| Phase | Status | Result |
|---|---|---|
| 1.1 Declaration interface | complete | `policy_declare_sequence` + `on_access`/`declare_sequence` hooks; `layer_order_declared` derives next-use distance from the declared sequence; Belady cross-check agrees at all positions. |
| 1.2 Naming + verification gate | complete | `layer_order` → `layer_order_learned` (+ alias); new `layer_order_declared` is default. **Gate PASS**: learned reproduces Campaign 12 Phase D arm D exactly (57 / 49 / 7,650,410,496), 3/3 reps. |
| 1.3 Determinism check | complete | Expectation 1 **DID NOT HOLD.** Declared deterministic at A.2 cells 1–4 **at the Belady floor (48)**; non-deterministic at cells 5–6 (same three-factor trigger as learned). `--policy-trace`: first divergence at identical resident set, cursor differs by one. |
| 1.4 Learned vs declared sweep | complete | 180 pager rows, all `rc=0`. **compute=0: declared arm D reads exactly OPT at all 6 cells** (learned 1.06–1.28× OPT). **compute=400000: declared reads more than learned at 5 of 6 cells** (up to 1.9× OPT, 1.6× arm C). |
| 1.5 Correctness | complete | T-1..T-7 re-run with `--eager-reconcile`, T-1 over 4 policies, T-4 over both. **All PASS.** No T-7 failure. |
Spec: `MECHANISM_SPEC.md` §8 Amendment **A-12** added.

### WP2 — llama.cpp integration (box 6h) — **NOT STARTED**
Report: `results/overnight/wp2_llamacpp.md`. Stopped at the Phase 2.0 GATE:
`models/model.gguf` is absent (the `models/` directory does not exist; no
GGUF anywhere under the project). Per WP2.md Phase 2.0 the entire work
package is skipped when the model is absent. Recorded as BLOCKER 1. No
llama.cpp build attempted.

### WP3 — Figures and final data package (box 2h) — **COMPLETE (5 of 6 figures)**
Report: `results/overnight/wp3_figures.md`. Generator:
`results/overnight/make_wp3.py`.

| Deliverable | Status |
|---|---|
| Figure 1 — bytes/work by ratio | done (`figures/figure1_bytes_per_work.{png,csv}`) — declared arm D on the OPT line |
| Figure 2 — miss rate vs OPT | done — C flat at 1.000, D on OPT |
| Figure 3 — chunk-size trade-off | done — bytes vs wall-clock diverge |
| Figure 4 — reclaim authority | done — spike S3d/S3e two-bar |
| Figure 5 — prefetch total fetches | done — replaces hit rate |
| Figure 6 — llama.cpp | **omitted** — WP2 not run |
| Table 1 — main results | done (`.md` + `.csv`), all non-clean cells flagged |
| Table 2 — environment | done |
| CLAIMS.md | done — 8 claims |

## What did not, and why

- **WP2 in full** — hard stop at Phase 2.0 GATE, `models/model.gguf`
  absent (BLOCKER 1). This is the pre-flight step 4 the human was to do
  before the session. Not recoverable by the agent (model hosts are not in
  the sandbox allowlist).
- **Figure 6** (WP3) — depends on WP2 output.
- **No time-box overruns.** WP1 finished well inside 3h; WP2 consumed
  ~0 time (gate); WP3 inside 2h.
- **No hard-stop conditions triggered** (no T-7 failure, no OPT below
  floor, no `reconcile()` divergence, no unfixed build failure, disk never
  below 10 GiB).
- **Pre-registered expectations 1, 2, 4, 5 of WP1 did not hold** — these
  are findings, not failures; reported in full in
  `wp1_declared_order.md` with the mechanism.

## New results, one line each

- `layer_order_declared` is **Belady-optimal** (D/OPT = 1.000) on the
  deterministic cyclic workload at every compute=0 cell —
  `wp1_sweep.csv` / `wp1_declared_order.md` §1.4; `figure1_bytes_per_work.csv`.
- `layer_order_learned` reproduces Campaign 12 Phase D arm D
  byte-for-byte (57 / 49 / 7,650,410,496) — the rename is behaviour-neutral —
  `wp1_gate_log.txt`.
- Declared order is **deterministic at the Belady floor (48 fetches)** at
  A.2 cells 1–4 vs learned's 57 — `wp1_determinism.csv`.
- Declared order is **still non-deterministic** under
  `--driver-threads>1` ∧ `--lookahead-window>0` ∧ `--compute-ns-per-mib>0`
  (cell 5: absent_handled 79–90; cell 6: 1316–1388) — the timing
  dependence moved from chain construction to the consumption-position
  signal — `wp1_determinism_log.txt`, `wp1_policytrace_log.txt`.
- Under a concurrent compute phase, declared order reads **more** than the
  learned policy (5 of 6 cells, up to +47%, up to 1.6× arm C) — its
  confident "evict the just-consumed chunk" rule loses to concurrent
  stragglers — `wp1_sweep.csv` / `analyze_wp1_sweep.py`.
- OPT for the WP1 grid (`belady_main`, all at or one above the cyclic
  floor): 8 MiB {8.59, 6.44, 4.29} GB; 128 MiB {8.72, 6.44, 4.29} GB at
  r = {0.25, 0.5, 0.75} — `wp1_sweep_opt.csv`.
- WP3 Figure 4 quantifies the reclaim-authority result for the first time:
  `swap.max=0` → `pgscan=pgsteal=0`, `high=37`; swap available →
  `pgscan≈120,687`, `pgsteal≈60,141`, `high=512–515`, ~236 MiB reclaimed —
  `figures/figure4_reclaim_authority.csv`.

## What changed in the code

| File | Change | Kind |
|---|---|---|
| `src/policy.h`, `src/policy.c` | `layer_order` split into `layer_order_learned` (unchanged) + `layer_order_declared` (new); `on_access`/`declare_sequence` hooks; `policy_declare_sequence()`. | **new feature** (A-12) + rename |
| `src/pager.c` | `pager_notify_access()` advances the declared policy's position for every reference, regardless of `--prefetch-retention`. | **new feature** wiring |
| `src/replay_main.c` | `--policy` accepts `layer_order_learned` / `layer_order_declared` (+ `layer_order` alias); default is now `layer_order_declared`; driver declares its cyclic sequence at startup. | **new feature** wiring |
| `src/test_policy.c` | `layer_order_declared` unit tests + standalone Belady next-use cross-check. | **test** |
| `src/test_correctness.c` | T-1 over 4 policies; T-4 over learned + declared with `pager_notify_access`. | **test** |
| `src/test_prefetch.c`, `test_storm.c`, `test_t6.c`, `test_t7.c` | `policy_layer_order_create` → `policy_layer_order_learned_create`. | rename only |
| `src/run_wp1_{gate,determinism,sweep,policytrace}.sh` | new sweep scripts. | **instrument** |
| `docs/MECHANISM_SPEC.md` | §8 Amendment A-12. | spec |
| `results/overnight/make_wp3.py` | figure/table generator. | **instrument** (analysis) |

No change to any mechanism decision function (`select_victim` /
`next_use_distance` / eviction / budget / reconcile) for the **learned**
policy — verified by the §1.2 gate and T-1..T-7.

## Numbers that are now superseded (for PROJECT_STATE.md §6)

- **Campaign 12 Phase C / Campaign 13 Phase C: D/OPT 1.070–1.775.** Still
  correct for `layer_order_learned` and for every compute=400000 cell.
  Superseded at compute=0 by `layer_order_declared` (WP1), which reads
  **exactly OPT** (D/OPT = 1.000) at all six compute=0 cells in the WP1
  grid.
- **"`layer_order` is the informed policy" / "the project has one informed
  policy".** WP1 / A-12: there are now two — `layer_order_learned`
  (retained comparison arm) and `layer_order_declared` (default).
- **"the application knows its access order in advance" is not
  implemented** (Campaign 13's own framing). Now implemented
  (`layer_order_declared`).
- **Nothing else.** The 5 non-deterministic Campaign 13 Phase A arm D cells
  remain non-deterministic (declared order does not fix them — same
  trigger); arm C's 1.000 miss rate, the OPT values, the chunk-size and
  reclaim findings are all unchanged.

## What the next session should do first

1. **Run WP2.** Place a 1.5–3 GiB GGUF at `/root/residctl/models/model.gguf`
   and follow `docs/overnight/WP2.md` end to end. This is the single
   biggest open structural question — every finding is still synthetic-only
   — and WP1 found the declared/learned policy ordering *flips* on the
   compute axis, which is exactly what WP2's real per-layer compute time
   would pin down. Highest value.
2. **Decide whether `layer_order_declared` should be the default given it
   is non-deterministic under the 3-factor trigger.** WP1 showed it is
   Belady-optimal when deterministic but worse than the learned hedge under
   concurrent stragglers. Options: keep declared as default (best common
   case), revert default to learned, or make the choice conditional on
   `--driver-threads`/`--compute-ns-per-mib`. A measurement question WP2's
   real workload would inform.
3. **Investigate the declared policy's straggler regression.** The
   `--policy-trace` shows the mechanism (cursor races concurrent faults).
   A candidate: have `select_victim` avoid evicting any chunk within
   `window+1` positions *behind* the cursor (not just at it). Would need a
   new determinism sweep + T-1..T-7.
4. **Extend Figure 1/2 to r ∈ {0.375, 0.625} for the declared policy** —
   a 12-run addition to `run_wp1_sweep.sh`'s grid, for a complete central
   figure.
5. **Fill Table 1's arm-A "best mode" selection** with the exact
   median-rep method Campaign 12 Phase D's report used (currently
   approximated as median-of-mode); minor, for the paper table's
   reproducibility.
