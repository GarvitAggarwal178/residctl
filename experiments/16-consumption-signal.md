# FINAL SESSION — Phase 2: the exact consumption signal

**Box:** 90 min. **Actual:** ~110 min (synthetic grid, within 2× box).
**Machine exclusive** before/after.

---

## What was added

Two flags on the synthetic replay driver (`replay_main`), the platform WP1 used:

- **`--consumption-signal {tid0,all-threads}`.**
  `tid0` (was the only behaviour): `pager_notify_access()` fires from driver
  thread 0 at the *start* of a step. The declared cursor then advances while the
  other N−1 threads may still be reading that chunk — the root cause the WP0
  heuristic patched from the policy side.
  `all-threads`: `pager_notify_access()` for step `s` fires exactly once, from
  whichever thread performs the increment that makes `completed[s] == n_threads`
  — i.e. when the chunk is genuinely consumed by everyone. The driver already
  tracks `completed[]` (A-10's lookahead window). **Exactly-once is asserted**
  (`replay_cyclic_mt` aborts if `notify_count != total_steps`).
  At `--driver-threads 1` the two modes are identical (`replay_cyclic`, the
  single-threaded path, is unchanged and never consults the mode).

- **`--protect-current {on,off}`** — toggles the session-2 WP0 heuristic
  (`lo_declared_dist()` returns 0 for `seq[pos]`/`seq[pos-1]`). Default was `on`.

`layer_order_learned` with prefetch off is unaffected by **either** flag (it has
no `on_access`; `--protect-current` only touches `layer_order_declared`). The
WP1 §1.2 verification gate (learned policy) still reproduces exactly.

---

## Part A — determinism (WP1 §1.3's A.2 six cells, n = 5)

`absent_handled` per rep; a cell is DETERMINISTIC iff all 5 reps are identical.

| # | threads / window / compute / chunk | `tid0`+on (default) | `tid0`+off | **`all-threads`+off** | `all-threads`+on |
|---|---|---|---|---|---|
| 1 | 1 / 0 / 0 / 128M | 50 ✓ | 48 ✓ | **48 ✓** | 50 ✓ |
| 2 | 1 / 0 / 400k / 128M | 50 ✓ | 48 ✓ | **48 ✓** | 50 ✓ |
| 3 | 8 / 0 / 0 / 128M | **{50,51} ✗** | 48 ✓ | **48 ✓** | 55 ✓ |
| 4 | 8 / 1 / 0 / 128M | 55 ✓ | 48 ✓ | **50 ✓** | 56 ✓ |
| 5 | 8 / 1 / 400k / 128M | 55 ✓ | **{83,87,90,91} ✗** | **50 ✓** | 56 ✓ |
| 6 | 8 / 1 / 400k / 8M | 768 ✓ | **{1388,1391,1395,1398} ✗** | **768 ✓** | 768 ✓ |

- **`all-threads` + protect off is deterministic at ALL SIX cells.** Cells 5 and
  6 — where session 1's declared policy was non-deterministic (79–90 / 1316–1388)
  and where the shipped `tid0`+on default is only deterministic *because* the
  heuristic pins the live chunks — are now stable **without the heuristic**.
- **`tid0` + off reproduces session 1's broken numbers**: cell 5 spans
  {83,87,90,91} (session-1 WP1 §1.3: 79–90), cell 6 spans {1388–1398}
  (session-1: 1316–1388). Same regime, same magnitude — the comparison is wired
  correctly.
- **New observation:** the shipped `tid0` + on default is itself
  **non-deterministic at cell 3** ({50,51}) — 8 threads, no window, no compute.
  PROJECT_STATE §3 recorded "residual mild non-determinism at cell 3"; here it is
  quantified. `all-threads` + off is deterministic there too (48).

## Part B — sweep (8 MiB & 128 MiB × r{0.25,0.5,0.75} × compute{0,400000}, arm D, n = 3)

`read_bytes / OPT` (median; OPT = `belady_main` over the declared reference,
floor-checked: 1024/768/512 misses at 8 MiB = exact cyclic floor; 65/48/32 at
128 MiB).

### 8 MiB (256 chunks)

`tid0`+on, `all-threads`+off, `all-threads`+on all read **exactly OPT (D/OPT =
1.000) at every cell, both compute levels.** `tid0`+off reads OPT at compute=0
but **1.5–1.9× OPT at compute=400000** (session-1 regression). At 256 chunks the
heuristic costs nothing — protecting 2 of ~64–192 budget chunks is free.

### 128 MiB (16 chunks) — where the heuristic's cost lives

| cell | `tid0`+on (default) | `tid0`+off | **`all-threads`+off** | `all-threads`+on |
|---|---|---|---|---|
| r0.25 / c0 | 1.154 | 1.000 | **1.077** | 1.231 |
| r0.25 / c400k | 1.154 | 2.062 ✗nd | **1.077** | 1.231 |
| r0.5 / c0 | 1.146 | 1.000 | **1.042** | 1.167 |
| r0.5 / c400k | 1.146 | 1.875 ✗nd | **1.042** | 1.167 |
| r0.75 / c0 | 1.094 | 1.000 | **1.000** | 1.125 |
| r0.75 / c400k | 1.094 | 1.500 ✗nd | **1.000** | 1.125 |

`✗nd` = non-deterministic (the c400k `tid0`+off blow-ups span 47–52 / 79–91 /
133–134 misses across reps).

---

## Pre-registered expectations

1. **`all-threads` + protection off is deterministic at all six A.2 cells —
   HOLDS.** This is the claim: the exact signal alone is sufficient.

2. **`all-threads` + protection off recovers the D/OPT regression at
   128 MiB/r=0.25/c=0 (1.15 → ~1.06 or better) — HOLDS (substantially).**
   1.154 → **1.077**. Not fully to the 1.06 that session-1's no-heuristic config
   reached at compute=0 — but that config is *non-deterministic at
   compute=400000* (2.06× OPT). `all-threads`+off is deterministic at **both**
   compute levels and within 8 % of Belady; at r=0.5 it is 1.04 and at r=0.75
   exactly OPT.

3. **`all-threads` + protection off is at or below `tid0` + protection on on
   bytes at every cell — HOLDS.** 8 MiB: equal (both OPT). 128 MiB: strictly
   lower at all six cells (1.077 vs 1.154, 1.042 vs 1.146, 1.000 vs 1.094).

4. **`tid0` + protection off reproduces session 1's broken numbers — HOLDS.**
   (See Part A cells 5–6 and the 128 MiB c400k column.)

**`all-threads` + on (both) is *worse* than `all-threads` + off** — 1.23 / 1.17 /
1.13 vs 1.08 / 1.04 / 1.00 at 128 MiB. With the exact signal present, the
heuristic over-protects (it pins chunks the exact signal already knows are done).
The heuristic is not merely unnecessary — it is mildly harmful once the signal is
exact.

---

## Decision executed (per the spec's pre-decided outcome)

> "If expectation 1 holds, the heuristic is unnecessary — report that, set the
> default to `all-threads` + protection off, and re-run T-1..T-7."

Expectation 1 holds. Done:

- **`replay_main.c`**: defaults flipped to `--consumption-signal all-threads
  --protect-current off`. Pin `--consumption-signal tid0 --protect-current on` to
  reproduce a pre-Phase-2 `layer_order_declared` or arm-E number exactly.
- **`residctl_llama.c`**: `protect_current` config default flipped to **off** —
  the real-model per-layer eval callback already fires `notify` *after* each
  layer's compute completes across all llama threads (the exact "all-threads"
  signal by construction), so the heuristic is redundant there too. Phase 3
  checks this on the real model.
- **`test_policy.c`**: the protect-*on* assertions now pin
  `policy_set_protect_current(1)` explicitly (self-documenting, default-
  independent); a new **protect-off** assertion block validates the plain-scan
  semantics that are now the default. `test_policy` PASS.
- **T-1..T-7 re-run with the flipped defaults — all PASS** (`mismatches = 0`, no
  lost fault, T-6 `dedup_fetching = 14046 > 0`).

### Reproducibility note

`--consumption-signal` and `--protect-current` change results only for
`layer_order_declared` (any prefetch) and for arm E (prefetch on — the retention
FIFO release timing, under any policy). `layer_order_learned` with prefetch off,
and every historical sweep and gate that used it, are byte-for-byte unaffected.
Historical `layer_order_declared` / arm-E CSVs are frozen; their scripts can pin
the old flags for exact reproduction.

---

## Files

- `results/final/phase2_determinism.csv` — 120 rows (4 combos × 6 cells × 5 reps).
- `results/final/phase2_sweep.csv` — 144 rows (4 combos × 2 cs × 3 r × 2 c × 3 reps).
- `results/final/phase2_opt.csv` — OPT per (cs, ratio), floor-checked.
- `results/final/phase2_log.txt` — full run log.
- `src/run_final_phase2.sh`, `src/phase2_supervisor.sh`, `src/smoke_p2.sh`.

## Final check

- No number estimated: every value is a measured `absent_handled` /
  `pager_bytes_fetched` (median of 3, or all 5 reps for determinism) or a
  `belady_main` output.
- No test weakened — a test was *added* (protect-off), and the protect-on
  assertions made mode-explicit.
- All four pre-registered expectations checked against measured values; #2 is
  reported as "substantially, not fully" recovered with the exact number.
- No new mechanism invented — the driver already tracked `completed[]`; this
  routes the existing signal through it.
- T-1..T-7 evaluated; PASS reported as PASS.
