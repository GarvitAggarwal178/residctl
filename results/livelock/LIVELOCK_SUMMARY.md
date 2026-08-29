# LIVELOCK FIX — session summary

**Premise.** A source review found three defects, one in the next-use distance
function. The spec fixed them and re-measured everything they touch. During
Phase 0 a fourth defect surfaced (a silent node-name mismatch); the user
authorised a fourth fix and a guarding assertion.

## The four defects and their fixes

| # | Where | Defect | Fix |
|---|---|---|---|
| 1 | `policy.c:lo_declared_dist()` | scanned `for d = 1..seq_len`, so the actively-consumed chunk `seq[pos]` matched only at `d == seq_len` — max distance, top eviction victim | scan origin is **signal-mode-aware**: `d` starts at **0** on the pre-consumption path (`seq[pos]` → distance 0), stays at **1** on the post-consumption path (byte-for-byte the old loop). `policy_set_signal_mode()`; unit tests for both modes |
| 2 | `wp2_gen.cpp:eval_cb()` | acted on the **post**-compute callback pass, so the declared cursor advanced only after layer N's weights were already read — lagged the real access by a full layer | act on the **pre**-compute pass (`if (!ask) return true;`) |
| 3 | `prefetch_pool.c` | a declined prefetch was immediately re-enqueued and re-declined — a busy loop (`stat_prefetch_declined` → 2104 while `stat_prefetches` froze at 77) | on decline, `chunk_t.decline_until_ns = now + 100 ms`; `prefetch_pool_top_up` skips candidates still in backoff |
| 4 | `wp2_gen.cpp:eval_cb()` | matched the graph node `"inp_embd"`; llama.cpp names the embedding output `"embd"`, so `token_embd` (chunk 2, 175 MiB) received **zero** consumption signals | match `"embd"`; `residctl_llama.c:notify_audit_maybe()` aborts at startup if any workload-declared chunk gets 0 signals in the first 2 decode passes |

## What the fixes changed

- **The arm-E "livelock" is fixed** — not avoided. It had been mislabelled three
  times (WP2: "over-constrained budget"; final Phase 3: "hard deadlock / orphaned
  `FETCHING` slot"; cleanup: "a livelock from two individually-correct
  mechanisms, mitigate by config"). The real cause was Defect 1 ranking the
  in-use chunk as coldest, fed by Defect 2's lagging cursor with Defect 4's
  unsignalled `token_embd`. **Phase 3b**: the exact prior-livelock config
  (`--prefetch-retention pinned --protect-current on`, r ∈ {0.25, 0.375}) now
  completes in **~44–54 s**, both reps — it previously ran `rc=124` for ~6 h at
  ~90× I/O amplification.
- **`stat_prefetch_declined` 2104 → 0–20**; `stat_fetching_timeout = 0` on all
  45 Phase 3 runs.
- **`residctl_llama.c` `protect_current` default flips to `off`** (spec
  amendment A-14). The sole reason it was `on` (cleanup: "off ⇒ arm D +67–78 %")
  was a Defect-2/Defect-4 artifact. **Phase 3c**: with the fixes, `protect_current`
  on vs off moves arm D by ≤ 1.8 % at every ratio. Both paths now default `off`.

## What did NOT change

- **The synthetic path** (`replay_main`, `--consumption-signal all-threads`):
  every `layer_order_declared` cell byte-identical to `results/final/phase2_*.csv`
  (Phase 2). The post-mode `d0 = 1` origin is a no-op refactor.
- **Arm D byte efficiency on the real model**: 125.0 / 98.5 / 78.9 / 59.7 /
  41.9 GB — within noise of the pre-fix protect-on baseline. The fixes' value is
  the arm-E livelock fix, the declined-prefetch collapse, and the
  consumption-signal correctness audit — not arm-D bytes.
- **`layer_order_learned`, `lru`, `select_victim`, `INT64_MAX` semantics** —
  untouched (spec constraint). WP1 §1.2 gate: `layer_order_learned` reproduces
  Campaign 12 Phase D exactly (57 / 49 / 7,650,410,496, 3/3 reps).

## Numbers that moved (65-pass OPT, decimal GB)

| quantity | was | now |
|---|---|---|
| arm D real-model bytes, r=0.25…0.75 | 126.1 / 98.4 / 79.3 / 60.6 / 43.4 | 125.0 / 98.5 / 78.9 / 59.7 / 41.9 |
| D/OPT, real model | 1.09–1.14 | **1.08–1.13** |
| D/A, real model | 0.98 / 0.89 / 0.71 / 0.58 / 0.52 | 0.97 / 0.89 / 0.71 / 0.57 / 0.50 |
| arm D throughput | 0.91 → 1.95 t/s (2.1×) | **1.12 → 2.85 t/s (2.5×, monotone)** |
| arm C throughput | ~0.8 t/s | ~1.0 t/s (byte-identical thrashing; machine variance) |
| arm E at r ≤ 0.375 | livelocks (protect on) / +37–43 % bytes (protect off) | completes; +13–14 % bytes (latency win) |

## Verification (Phase 5)

- **T-1..T-7** (eager reconcile — `test_correctness.c` hardcodes
  `reconcile_interval = 1`, A-3): PASS, `mismatches = 0`, after the code changes
  and after the `protect_current` default flip.
- **`test_policy`**: PASS — declared next-use vs naive Belady scan agrees in
  both signal modes; protect-on/off both modes ok.
- **Real-model correctness gate**: mmap vs residctl, 32 tokens, byte-identical —
  both with `--protect-current off` explicit (Phase 3) and with the flipped
  default (Phase 4 re-gate).
- **WP1 §1.2 learned-policy gate**: PASS, byte-identical, 3/3 reps.
- **Citations**: `WRITEUP_PACKAGE.md` / `CLAIMS.md` / `PROJECT_STATE.md` file
  references resolve (spike-report refs live in `/root/spike/`, as before).

## Commits

`01a0ca1`, `cfa4907` (Phase 0) · `bb20f34` (Phase 0b) · `cfacb64` (Phase 1) ·
`ddffaad` (Phase 2) · `96b1c28` (Phase 3) · `c6f4e80` (Phase 4) · Phase 5 (this
summary). All on `origin/main`, `github.com/GarvitAggarwal178/residctl`.

## Artifacts

- `results/livelock/phase0_cursor_diagnostic.md`, `BLOCKERS.md`
- `results/livelock/phase1_fixes.md`
- `results/livelock/phase2_synthetic.md`
- `results/livelock/phase3_real_model.md` (+ `.csv`, `phase3b_arm_e_protect_on.csv`,
  `phase3c_arm_d_protect_on.csv`, policy traces, correctness gate)
- `results/livelock/phase4_propagation.md`
- `docs/design-history.md` — consumption-signal timing decision + the road not taken
- Spec amendment **A-14** in `PROJECT_STATE.md` §5
