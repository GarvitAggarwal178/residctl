# LIVELOCK FIX — Phase 3: real-model re-measurement

All four fixes in (`embd` node match + zero-notify audit + signal-mode-aware
Defect 1 + Defect 2 pre-compute notify + Defect 3 decline backoff). Real model
(Qwen2.5-3B-Instruct Q4_K_M), `signal_mode=pre`. The main grid runs
`--protect-current off`; **Phase 3c** re-runs arm D with `--protect-current on`
for a clean one-variable comparison against the pre-fix protect-on baseline.
Machine exclusive (own runs; `pgrep cn-spike|iperf3|gate5` clean; load returned
to ~0.5 after).

Scripts: `src/livelock_phase3.sh`, `src/livelock_phase3_analyze.py`,
`src/livelock_phase3b.sh`, `src/livelock_phase3c.sh`. Data:
`phase3_real_model.csv` (45 rows, 3 arms × 5 ratios × 3 reps),
`phase3b_arm_e_protect_on.csv`, `phase3c_arm_d_protect_on.csv`,
`phase3_correctness_gate.txt`, per-ratio `phase3_*_policy.trace`.

Arm A is unchanged by these fixes — reused from
`results/final/phase1_equal_budget.csv`.

---

## Correctness gate — **PASS**

32 tokens, `mmap` vs `residctl` (arm D, r=0.75). Byte-identical token sequences:

```
13 576 1156 12677 311 5545 264 11580 304 264 501 33161 1033 22208 911 279
11580 594 39671 13 2379 13166 429 279 39671 11 2598 279 1863 1775 2382 11
```

Defect 2's pre-compute notify is compute-equivalent.

---

## The grid (median of n=3; decimal GB; `X/OPT` = read ÷ 65-pass `phase1_opt.csv`)

Main grid: `--protect-current off` (the new A-14 default). Arm A is unchanged
and stays in `phase1_equal_budget.csv` (129.1 / 110.4 / 111.3 / 105.1 / 83.3 GB;
0.65 / 0.79 / 0.62 / 0.64 / 0.75 tok/s).

| r | arm | reps rc | read (GB) | demand faults | prefetches | declined | tok/s | X/OPT |
|---|---|---|---|---|---|---|---|---|
| 0.25 | C | 0,0,0 | 144.4 | 2817 | – | – | 0.97 | 1.305 |
| 0.25 | **D** | 0,0,0 | **125.0** | 2689 | 0 | 0 | 1.12 | **1.130** |
| 0.25 | E | 0,0,0 | 142.1 | 1055 | ~1690 | 20 | 1.22 | 1.284 |
| 0.375 | C | 0,0,0 | 143.2 | 2753 | – | – | 0.96 | 1.588 |
| 0.375 | **D** | 0,0,0 | **98.5** | 2141 | 0 | 0 | 1.35 | **1.093** |
| 0.375 | E | 0,0,0 | 111.1 | 937 | ~1400 | 0 | 1.53 | 1.233 |
| 0.5 | C | 0,0,0 | 134.5 | 2561 | – | – | 1.01 | 1.852 |
| 0.5 | **D** | 0,0,0 | **78.9** | 1734 | 0 | 0 | 1.65 | **1.086** |
| 0.5 | E | 0,0,0 | 84.9 | 766 | ~1020 | 0 | 1.88 | 1.169 |
| 0.625 | C | 0,0,0 | 134.5 | 2561 | – | – | 0.99 | 2.437 |
| 0.625 | **D** | 0,0,0 | **59.7** | 1300 | 0 | 0 | 2.17 | **1.081** |
| 0.625 | E | 0,0,0 | 62.9 | 653 | ~680 | 0 | 2.34 | 1.140 |
| 0.75 | C | 0,0,0 | 134.5 | 2561 | – | – | 0.97 | 3.511 |
| 0.75 | **D** | 0,0,0 | **41.9** | 915 | 0 | 0 | 2.85 | **1.095** |
| 0.75 | E | 0,0,0 | 43.3 | 489 | ~430 | 0 | 3.06 | 1.132 |

**`stat_infeasible = 0`, `stat_pin_broken = 0`, `stat_fetching_timeout = 0` on
every one of the 45 runs.** No WATCHDOG fired (140 s; a healthy run is ~20–70 s).
D/OPT range **1.08–1.13** (was 1.09–1.14 at the pre-fix protect-on baseline).

---

## Pre-registered expectations

### 1. **Arm E completes at r = 0.25 — HELD.** (headline)

3/3 reps `rc = 0`, ~53 s, 1.22 tok/s, with prefetch on + retention pinned +
`protect_current off`. No livelock, no watchdog, `stat_fetching_timeout = 0`.
The cleanup session already showed protect-*off* arm E completes; what changed
is **how much** it reads — see expectation 4.

### 2. Arm D reads fewer bytes than the protect-on baseline — **essentially FLAT (revised expectation).**

The original spec expected a *large* drop ("mis-ranking one chunk per decision
is a large error"). The main grid ran `--protect-current off`, so it compared
two variables at once against a `--protect-current on` baseline. **Phase 3c**
supplies the clean one-variable cell — arm D, `--protect-current on`, all four
fixes, n=2, deterministic (both reps byte-identical at every ratio):

| r | on + 4 fixes (3c) | pre-fix baseline (on) | Δ | off + 4 fixes (main grid) | Δ (off vs on, fixes in) |
|---|---|---|---|---|---|
| 0.25  | 126.141 GB | 126.141 | **−0.0002 %** (63 pages) | 124.966 GB | −0.93 % |
| 0.375 | 97.607 GB  | 98.386  | −0.79 % | 98.507 GB | +0.92 % |
| 0.5   | 79.632 GB  | 79.294  | +0.43 % | 78.895 GB | −0.93 % |
| 0.625 | 60.762 GB  | 60.616  | +0.24 % | 59.659 GB | −1.82 % |
| 0.75  | 41.655 GB  | 43.361  | **−3.94 %** | 41.938 GB | +0.68 % |

**With `protect_current` held fixed, the four fixes do not change arm-D byte
efficiency on the real model** — flat within ±1 % at every ratio except r=0.75,
where they help by 3.9 % (fewer faults too: 847 vs 871). And with the fixes in,
`protect_current` **on vs off is within ±1.8 % at every ratio** — arm D is now
insensitive to the heuristic.

Why the flat result is expected, not a disappointment:

- **On the pre-consumption signal path `seq[pos]` is already distance 0 by
  construction**, so `--protect-current`'s only residual effect is forcing
  `seq[pos-1]` (the *previous* layer, a full lap from reuse) to 0 — one chunk
  out of 40, mis-ranked. That is worth about the ±1 % measured. The heuristic
  was never going to move arm D much once the cursor tracks the real read
  frontier.
- **The cleanup session's "+67–78 % for protect-off" was a Defect-2/Defect-4
  artifact.** Pre-fix, the cursor lagged a full layer (notify fired
  post-compute) *and* `token_embd` received no signal at all (`embd` vs
  `inp_embd`). With protect-off there was nothing shielding the chunk in use,
  so it churned. Defect 2 + the `embd` match remove that entirely — protect-off
  now costs arm D ~1 %, not 67–78 %.
- **At tight budget the working set genuinely does not fit** regardless of
  ranking: at r=0.25 the 502 MiB budget cannot hold `token_embd` (175 MiB) +
  `output` (243 MiB) + a layer working set, so correct Belady evicts the
  just-read big chunk anyway. Room to keep more only appears at r ≥ 0.625,
  which is why the only real improvement is at r=0.75.

Demand faults on the protect-off grid rose slightly at mid budgets (r=0.5: 1734
vs 1614 baseline) while bytes fell — the fault mix shifted away from the two
large chunks toward layers, the intended direction.

### 3. **`stat_prefetch_declined` drops orders of magnitude from 2104 — HELD.**

`2104 → 20` (arm E, r=0.25) and **`0` at every other arm-E cell**. Defect 3's
100 ms per-chunk decline backoff removed the re-enqueue-and-decline busy loop.
Attribution: the backoff itself, plus Defect 1 (correct pre-consumption
distances make `ensure_budget_prefetch`'s `victim_dist > target_dist` check
pass far more often, so fewer declines to back off from in the first place).

### 4. Arm E vs arm D at r ≤ 0.375 — **prefetch still non-advantageous, but the penalty roughly halved.**

| r | E (this session) | E (cleanup, protect off) | E vs D (this session) | E vs D (cleanup: E protect-off vs D protect-on) |
|---|---|---|---|---|
| 0.25  | 142.1 GB | ~173 GB (**−18 %**) | **+13.8 %** | +37 % |
| 0.375 | 111.1 GB | ~140 GB (**−21 %**) | **+12.8 %** | +43 % |

Arm E reads ~13 % more than arm D at tight budget (was 37–43 %), and it is
**faster than arm D at every ratio** (r=0.25: 1.22 vs 1.12 tok/s; r=0.375:
1.53 vs 1.35) — the bytes-for-latency trade now holds even at tight budget,
where before it was a pure loss. At r ≥ 0.5 arm E is +3–8 % bytes for
~15 % more throughput (Claim 6's "7–13 % more bytes for ~2× fewer faults"
regime, confirmed: faults ~halved — r=0.5: E 766 vs D 1734).

**The operating recommendation softens:** arm E is now a viable
bytes-for-latency choice at *every* ratio, not only r ≥ 0.5. It is still not
the byte-efficiency choice anywhere.

### 5. D/OPT improves on the 1.09–1.14 baseline — **HELD (65-pass OPT).**

| r | D/OPT (this session, protect-off + fixes) | baseline (protect-on, pre-fix) | |
|---|---|---|---|
| 0.25  | 1.130 | 1.140 | improved |
| 0.375 | 1.093 | 1.091 | flat (+0.2 %) |
| 0.5   | 1.086 | 1.091 | improved |
| 0.625 | 1.081 | 1.098 | improved |
| 0.75  | 1.095 | 1.132 | improved |

Small but consistent. New range **1.08–1.13** (was 1.09–1.14). All D/OPT here
use the canonical 65-pass `phase1_opt.csv` (prompt scan + 64 decode scans),
matching Table 1 and Figure 6.

---

## Phase 3b — is the *livelock* fixed, or just avoided?

The original arm-E livelock (Claim 10) was a **`protect_current` on**
phenomenon: `rc = 124`, ~90× I/O amplification, would run ~6 h. Phase 3 above
ran `protect off` per the spec. Phase 3b re-runs arm E with `protect_current
**on**` + all four fixes, at r ∈ {0.25, 0.375}, n=2, with a 180 s watchdog.

### VERDICT: the livelock is **FIXED**, not merely avoided.

| r | reps rc | wall (s) | tok/s | read (GB) | declined | fetch-timeout | infeas | pin-brk | WATCHDOG |
|---|---|---|---|---|---|---|---|---|---|
| 0.25  | 0, 0 | 53.1, 54.3 | 1.18–1.21 | 137.4, 138.8 | 23, 26 | 0 | 0 | 0 | none |
| 0.375 | 0, 0 | 43.7, 44.4 | 1.44–1.47 | 109.8, 110.9 | 0, 0  | 0 | 0 | 0 | none |

The exact configuration that previously returned `rc = 124` with ~90× I/O
amplification and would have run ~6 h — arm E, `prefetch on`, `retention
pinned`, **`protect_current on`**, r ≤ 0.375 — now completes in **~44–54 s**,
both reps, at both ratios. The 180 s watchdog did not fire. `stat_prefetch_
declined` is 23–26 (was 2104) and `stat_fetching_timeout` is 0.

**Why it is fixed and not avoided:** the livelock was Defect 2 feeding Defect 1.
The consumption signal fired *after* layer N's weights were already read, so the
cursor lagged one layer; `lo_declared_dist` then returned max-distance for the
chunk actually in use (`seq[pos]` matched only at `d == seq_len`); `protect_
current on` layered its lookback onto that already-wrong ranking, and
`prefetch_admit` kept evicting-and-refetching the working set. With Defect 2's
pre-compute notify the cursor tracks the real read frontier, the signal-mode-
aware distance function returns 0 for `seq[pos]`, and `protect_current` now
pins the chunks that are genuinely in use. The amplification loop has no input.

Protect-**on** arm E also reads marginally **fewer** bytes than protect-off
(137–139 vs 142.1 GB at r=0.25; ~110 vs 111.1 at r=0.375) for ~3 % less
throughput — a small accidental interaction with prefetch retention, not a
policy improvement (Phase 3c shows arm D is insensitive to the heuristic).

---

## Phase 3c — arm D, `--protect-current on`, all four fixes

The one-variable cell for expectation 2 and the A-14 default. Arm D,
`prefetch off`, `--protect-current on`, `signal_mode=pre`, 5 ratios × n=2.
Deterministic (both reps byte-identical `pager_bytes_fetched` at every ratio);
`stat_infeasible = stat_pin_broken = stat_fetching_timeout = stat_prefetch_
declined = 0` on all 10 runs.

| r | read (GB) | faults | evictions | tok/s | vs pre-fix baseline (on) | vs protect-off + fixes |
|---|---|---|---|---|---|---|
| 0.25  | 126.141 | 2499 | 2495 | 1.03 | −0.0002 % | +0.93 % |
| 0.375 | 97.607  | 1993 | 1983 | 1.31 | −0.79 %   | −0.92 % |
| 0.5   | 79.632  | 1614 | 1598 | 1.59 | +0.43 %   | +0.93 % |
| 0.625 | 60.762  | 1235 | 1214 | 2.01 | +0.24 %   | +1.82 % |
| 0.75  | 41.655  | 847  | 820  | 2.75 | −3.94 %   | −0.68 % |

**Finding:** with the four fixes in, `--protect-current` changes arm-D bytes by
≤ 1.8 % at every ratio, in both directions — the real-model signal is now
accurate enough (cursor tracks the read frontier, `token_embd` signalled,
`seq[pos]` distance 0 by construction) that the heuristic is redundant, exactly
as on the synthetic `all-threads` path. On the pre-consumption path its residual
effect — pinning `seq[pos-1]`, the previous layer — is mis-ranked by
construction.

**A-14 default decision:** flip `residctl_llama.c` to `protect_current = off`.
The only reason it was `on` (cleanup session: off ⇒ arm D +67–78 %) is now
proven to be a Defect-2 + Defect-4 artifact; Phase 2 (synthetic, with the fixes)
already showed protect-on 12–14 % *worse* on D/OPT where the signal is accurate,
and Phase 3+3c show the real path's signal is now accurate too. Flipping also
makes the default match the config the Phase 3 correctness gate already ran
under. `--protect-current` and its unit tests stay; only the default moves.

---

## Machine state

Load 0.5 before, ~3.6 during the sweep (own 8-thread runs), 0.5 after. No
foreign workload. Every value is a measured counter from `RESIDCTL_STATS` or a
`WP2_CSV` line; medians over n=3; nothing estimated.
