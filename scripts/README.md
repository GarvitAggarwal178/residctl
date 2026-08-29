# Scripts

The reproducible sweeps. Each runs from the repo root, checks machine
exclusivity before and after, and writes to `results/data/` (or, for the
livelock probes, prints a table). `scripts/historical/` holds ~53
closed-campaign one-offs, kept as evidence — do not expect those to run against
the current tree without path fixes.

**Prerequisites:** `make` in `src/` (synthetic binaries); for the real-model
scripts, [`../docs/06-reproduce.md`](../docs/06-reproduce.md) (the model file,
llama.cpp, `build-real-model-integration.sh`).

## Correctness — run these first

| script | what it checks |
|---|---|
| `run-correctness-harness.sh` | T-1…T-5 (data integrity, punch-refetch, storm, accounting, Belady self-test) |
| `run-storm-t6-t7.sh` | T-6 (dedup branches fire) + T-7 (no fault lost in a 60 s storm) |
| `run-learned-policy-gate.sh` | `layer_order_learned` reproduces the Campaign 12 Phase D numbers byte-for-byte |
| `run-real-model-correctness-gate.sh` | `mmap` load vs `residctl` load — byte-identical tokens |

## Synthetic sweeps

| script | produces |
|---|---|
| `run-declared-vs-learned-sweep.sh` | `data/declared-vs-learned-policy.csv` |
| `run-policy-determinism-grid.sh` | `data/policy-determinism-grid.csv` |
| `run-consumption-signal-sweep.sh` | `data/consumption-signal-comparison.csv`, `-determinism-grid.csv` |
| `run-policy-trace.sh` | `--policy-trace` captures for a determinism divergence |
| `run-livelock-synthetic-recheck.sh` | the byte-identical re-check of the synthetic path after A-14 |

## Real-model sweeps

| script | produces |
|---|---|
| `setup-llama-cpp.sh` / `build-real-model-integration.sh` | one-time setup / build of `wp2_gen` |
| `run-real-model-smoke.sh` | a single quick generation, sanity |
| `run-real-model-arms-sweep.sh` | `data/real-model-arms.csv` (WP2 grid) |
| `run-real-model-equal-budget.sh` | `data/real-model-bytes-by-budget.csv` (arm A + the final-session grid) |
| `run-real-model-opt.sh` | `data/real-model-opt-bound.csv` |
| `run-livelock-real-model.sh` | `data/livelock-real-model-arms.csv` — arms C/D/E, all four A-14 fixes |
| `run-livelock-protect-on-probe.sh` | the decisive test: arm E, `protect_current on`, at the ratios it livelocked |
| `run-livelock-arm-d-protect-on.sh` | the arm-D `protect_current` one-variable cell |
| `run-signal-audit.sh` | the Phase 0 consumption-signal diagnostic |
| `run-prefetch-collapse-probe.sh` | the final-session arm-E fallback-config probe |

Analysis / figure scripts are in [`../tools/`](../tools/).
