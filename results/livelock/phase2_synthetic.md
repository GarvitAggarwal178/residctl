# LIVELOCK FIX — Phase 2: synthetic re-measurement

**Revised expectation (user):** the synthetic `--consumption-signal all-threads`
path must be **unchanged** vs the final measurement session. Post-mode Defect 1
(`d0 = 1`) is a no-op refactor of the old `for d = 1..seq_len` loop; **any
change on this path is a regression to investigate, not a finding.**

Machine exclusive (own runs only; `pgrep cn-spike|iperf3|gate5` clean — the
"WARNING other workload" line in the console is `pgrep` matching its own
command string, not a real workload; the byte-identical baseline reproduction
confirms exclusivity).

Scripts: `src/livelock_phase2.sh`, `src/livelock_phase2_analyze.py`.
Data: `phase2_determinism.csv` (30 rows), `phase2_sweep.csv` (108 rows).
Baseline: `results/final/phase2_{determinism,sweep,opt}.csv`.

---

## VERDICT: synthetic path **UNCHANGED**. No regression.

Every `layer_order_declared` + `all-threads` cell reproduces the final session
**exactly** (`absent_handled`, `evictions`, `pager_bytes_fetched` all identical),
with `signal_mode=post` confirmed on every run.

### A. Determinism grid — 6 A.2 cells, all-threads, `--protect-current off`, n=5

| cell | threads/window/compute | mine (absent_handled, evictions, pager_bytes) | vs `phase2_determinism.csv[allthreads_off]` |
|---|---|---|---|
| 1 | 1 / 0 / 0        | 48, 40, 6 442 450 944 | **MATCH**, deterministic |
| 2 | 1 / 0 / 400000   | 48, 40, 6 442 450 944 | **MATCH**, deterministic |
| 3 | 8 / 0 / 0        | 48, 40, 6 442 450 944 | **MATCH**, deterministic |
| 4 | 8 / 1 / 0        | 50, 42, 6 710 886 400 | **MATCH**, deterministic |
| 5 | 8 / 1 / 400000   | 50, 42, 6 710 886 400 | **MATCH**, deterministic |
| 6 | 8 / 1 / 400000 (8 MiB) | 768, 640, 6 442 450 944 | **MATCH**, deterministic |

All 5 reps identical within every cell. Expectation 1 (determinism holds with
protection off) — **met**.

### B. Sweep — `layer_order_declared`, arm D, all-threads, protect {off, on}, n=3

Every cell matches `phase2_sweep.csv` (`allthreads_off` / `allthreads_on`)
exactly. `signal_mode=post` on all.

**128 MiB D/OPT** (against `phase2_opt.csv`):

| ratio | protect **off** (mine / baseline) | protect **on** (mine / baseline) |
|---|---|---|
| 0.25 | **1.077** / 1.08 | 1.231 / 1.23 |
| 0.5  | **1.042** / 1.04 | 1.167 / 1.17 |
| 0.75 | **1.000** / 1.00 | 1.125 / 1.13 |

**8 MiB D/OPT** = **1.000** at every ratio, both protect settings, both
computes (baseline: 1.000).

- Expectation 2 (8 MiB stays 1.000) — **met**.
- Expectation 3 (**revised**: 128 MiB D/OPT unchanged, *not* improved) —
  **met**. 1.077 / 1.042 / 1.000, identical to the final session. The original
  "expect improvement" was wrong: on the all-threads path the signal is
  post-consumption, `seq[pos]` is genuinely a full cycle from its next use, and
  the old `d = 1` origin was already correct Belady. Defect 1 changes nothing
  here by construction.
- Expectation 4 (protect-on now costs rather than saves) — **met**. protect-on
  is worse at every 128 MiB cell (1.231 / 1.167 / 1.125 vs 1.077 / 1.042 /
  1.000) — the heuristic pins two chunks for no benefit once the exact signal
  is in hand. (Unchanged from the final session; this was already true.)

### C. `layer_order_learned` sweep (regression check — "both policies")

Not in the final-session Phase 2 (which swept `layer_order_declared` only), so
no exact baseline. Defect 1 does not touch `layer_order_learned`. Observed:

- `compute = 0` cells: **deterministic** at 128 MiB (e.g. r=0.5 → 57 / 57 / 57
  faults). At 8 MiB, mild variation (1089 / 1090 / 1093) — the known
  fault-dispatch-order sensitivity of the learned chain at fine granularity.
- `compute = 400000` cells: **non-deterministic** (e.g. 128 MiB r=0.5 →
  69 / 76 / 79 faults) — exactly the concurrency-dependent behaviour WP1 §1.3
  documented and that `layer_order_declared` was built to avoid. Not a
  regression; it is the comparison arm behaving as characterised.

The WP1 §1.2 gate (`layer_order_learned` at the deterministic cell) passed
byte-exact in Phase 1, which is the binding regression check for `learned`.

---

## Conclusion

Post-mode `lo_declared_dist` (`d0 = 1`) is byte-for-byte the pre-fix loop. The
synthetic all-threads path — determinism grid and sweep, `layer_order_declared`
and `layer_order_learned`, protect off and on — is **unchanged**. No figure or
claim derived from `results/final/phase2_*` moves. Proceeding to Phase 3 (the
real model, where `signal_mode=pre` and Defect 1 actually bites).
