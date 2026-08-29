# CLAIMS — what the writeup may assert, and on what evidence

One entry per claim the report will make. `Strength`: strong (clean,
replicated, deterministic) / qualified (real but with a named caveat) /
preliminary (single regime, or synthetic-only where that matters).

Each body is current and standalone as written. The `Superseded prior claims`
field of every entry is the audit trail — it records what an earlier session
asserted and why it no longer holds.

**Policy-default note (read once):** `layer_order_declared` runs with
`--protect-current` **off** on **both** paths (LIVELOCK FIX A-14). On the
**synthetic replay driver** (`replay_main`), `--consumption-signal all-threads`
advances the cursor only after every driver thread finishes a chunk. On the
**real model** (`residctl_llama`, llama.cpp) the eval callback now fires the
consumption signal on the **pre-compute** pass (LIVELOCK FIX Defect 2) and
matches the `embd` graph node (Defect 4), so the declared cursor tracks the real
read frontier and `seq[pos]` is distance 0 by construction — the heuristic is
redundant, exactly as on the synthetic path. The cleanup session's "off ⇒ arm D
+67–78 %" was a Defect-2/Defect-4 artifact (the cursor lagged a full layer and
`token_embd` was never signalled at all); with the fixes, `protect_current` on
vs off moves arm D by ≤ 1.8 % at every ratio (`results/livelock/phase3c_arm_d_
protect_on.csv`). `--protect-current` and its unit tests are retained for a
caller whose consumption signal is genuinely inexact.

---

### Claim 1: Kernel LRU degenerates to full-pass thrashing on a cyclic reference string.

Evidence, synthetic: `campaign12_phaseD_paper_table.csv` — arm C, every one of
the 20 cells: `absent_handled == touches`, miss rate exactly 1.000,
`read_bytes` = the whole 2 GiB region every run. Back to item 7's smoke test
(`lru` 40/40 faults). `figure2_miss_rate.csv`.

Evidence, real model: `results/final/phase1_equal_budget.csv` — arm C on
Qwen2.5-3B Q4_K_M reads **134–144 GB per 64 tokens at every budget ratio
including 0.75**, `absent_handled` 2561–2817 vs 2304 layer transitions (miss
rate ≥ 1.0 — a chunk evicted mid-scan re-faults), ~0.7–0.9 tokens/s regardless
of budget. Byte-identical run to run. `figure6_llamacpp.csv`.

Figure: Figure 2 (synthetic, flat at 1.000); Figure 6 (real model, arm C
flat-high); Figure 1 (arm C flat at region size).

Strength: **strong.** Deterministic, universal across every synthetic cell,
**confirmed on a real model at every ratio**, and mechanically inevitable (the
working set exceeds budget and LRU evicts the chunk needed soonest on a cyclic
scan).

Caveats: one real model (3B dense, CPU-only). A reviewer could argue a real
workload has enough irregularity to give LRU some hits; the real-model data
answers that directly — a transformer layer scan has none, and arm C thrashed
identically to the synthetic case.

Superseded prior claims: the earlier caveat "only demonstrated on the
cyclic-scan replay workload; WP2 would have confirmed — not run (BLOCKER 1)" —
WP2 (session 2) and the final session's Phase 1 ran it on Qwen2.5-3B and it
holds.

---

### Claim 2: Application-authoritative residency reduces bytes read per unit of work, and the reduction scales with available budget.

Evidence, synthetic (`layer_order_declared`, `replay_main`, `--consumption-
signal all-threads --protect-current off`): `phase2_sweep.csv` — arm D reads
**exactly OPT (D/OPT = 1.000) at 8 MiB (256 chunks)** and **D/OPT = 1.00–1.08 at
128 MiB (16 chunks)**, at **both** compute levels (0 and 400000), deterministic
at all cells. Against arm C flat at the region size.

Evidence, real model (`layer_order_declared`, `--protect-current off`, all four
LIVELOCK FIX defects): `results/livelock/phase3_real_model.csv` — arm D
`read_bytes` falls with budget: 125.0 → 98.5 → 78.9 → 59.7 → 41.9 GB at
r = 0.25 … 0.75, vs arm C flat at 134–144 GB and arm A flat-ish at 83–129 GB.
**D beats arm C by 13–69 % and arm A by 3–50 % at every ratio**
(D/A = 0.97 / 0.89 / 0.71 / 0.57 / 0.50). `figure6_llamacpp.csv`. Deterministic
— all three reps byte-identical at every ratio.

Figure: Figure 1 (synthetic), Figure 2, Figure 6 (real model, left panel).

Strength: **strong.** Deterministic in both regimes; the synthetic declared
policy equals Belady exactly at realistic chunk counts, and the real model lands
within 13 % of it at every ratio.

Caveats: (1) the real-model arm-A comparison carries a disclosed ~50 MiB
weight-cache asymmetry favouring the pager arms (≤ 10 % of B at r=0.25); the
arm-C comparison (arm C not host-cache-contaminated) is clean. (2) synthetic +
one real model only.

Superseded prior claims: (a0) the real-model arm-D column
126.1 → 98.4 → 79.3 → 60.6 → 43.4 GB and "D/OPT 1.09–1.14" from
`phase1_equal_budget.csv` (`protect_current` on, pre-LIVELOCK-FIX) — re-measured
with all four defects fixed and `--protect-current off` (the A-14 default):
125.0 → 98.5 → 78.9 → 59.7 → 41.9 GB, D/OPT 1.08–1.13. Within noise of the old
column (the fixes barely move arm D; their value is the arm-E livelock fix and
the consumption-signal correctness audit). (a) Campaign 12 Phase C / Campaign 13
Phase C reported D/OPT 1.070–1.775 for the **learned** policy — still correct for
`layer_order_learned` and for the pre-fix declared policy; the declared policy
with the exact all-threads signal reads 1.00–1.08. (b) Session-1 WP1's
"declared arm D is non-deterministic and reads up to 1.9× OPT under
`--compute-ns-per-mib 400000`" — superseded first by the session-2 WP0 heuristic
and then by the final session's Phase 2 all-threads signal, which is
deterministic at both compute levels with no 128 MiB regression.

---

### Claim 3: Eviction under `memory.swap.max = 0` is authoritative — the kernel enters reclaim and finds nothing eligible.

Evidence: spike S3d vs S3e (`/root/spike/results/s3d_output.log`,
`s3e_summary.txt`, `s3e_verify.txt`), `figure4_reclaim_authority.csv`.
Under `swap.max=0`: `memory.events[high]=37` (the kernel *did* enter reclaim),
`pgscan=0`, `pgsteal=0`, `memory.stat[shmem]` unchanged at 600 MiB. With swap
available: `pgscan≈120,687`, `pgsteal≈60,141`, `high=512–515`, ~236 MiB of shmem
reclaimed to swap (3 runs).

Figure: Figure 4.

Strength: **strong.** The project's cleanest causal result — a controlled A/B on
one cgroup setting, 3 replicates, unambiguous direction.

Caveats: measured in the feasibility spike, not re-run since; on the same WSL2
VM. The mechanism (no swap ⇒ anonymous/shmem pages are unevictable ⇒ the
application's `FALLOC_FL_PUNCH_HOLE` is the only thing that frees them) is
kernel-general, not WSL-specific.

Superseded prior claims: none — this claim previously existed only as prose in
the spike addendum; Figure 4 is its first visualisation.

---

### Claim 4: The offline optimal bound is computable for this workload; how far a policy sits from it depends on the policy and the regime.

Evidence: `belady_main` (naive O(n²) cross-check 300/300; exact cyclic-floor
check on every run — T-5). `wp2_opt.c` for unequal chunk sizes
(`missed_bytes = Σ missed chunk sizes`, floor-checked). Distances:

- `layer_order_learned` (synthetic): D/OPT **1.06–1.53** at the clean cells.
- `layer_order_declared` (synthetic, all-threads + protect-off): D/OPT
  **1.000** at 8 MiB, **1.00–1.08** at 128 MiB, both compute levels
  (`phase2_sweep.csv`).
- `layer_order_declared` (real model, `--protect-current off`, all four LIVELOCK
  FIX defects): D/OPT **1.08–1.13** at all five ratios (65-pass `phase1_opt.csv`,
  `results/livelock/phase3_real_model.csv`, `figure6_llamacpp.csv`).

Figure: Figures 1, 2 (OPT dashed reference), Figure 6.

Strength: **strong.** The bound is exact (provable cyclic floor) and
independently cross-checked; it is also computable for the real model's
non-uniform chunk table.

Caveats: "distance from OPT" is regime-dependent — the informed policy *reaches*
OPT on the synthetic path with the exact signal, and is within 13 % on the real
model. Phrase claims per policy and per regime, not "the informed policy is
1.07–1.78× OPT" flatly.

Superseded prior claims: "both policies are measurably distant from OPT"
(Campaign 13 Phase C, D/OPT 1.070–1.775) — now regime-dependent; the declared
policy with the exact signal is at OPT.

---

### Claim 5: Chunk size trades byte efficiency against wall-clock, optimising in opposite directions.

Evidence: `figure3_chunk_size_tradeoff.csv` — Campaign 12 Phase B {4,8,16,32}
+ Campaign 11 Phase 3 {32,64,128,256} MiB, arm D, compute=0. `read_bytes` has a
shallow minimum near 8–16 MiB then climbs to 256 MiB; wall-clock has a valley
near 16–32 MiB and is worst at 4 MiB.

Figure: Figure 3.

Strength: **qualified.** The two source sweeps used different scripts and did
**not** agree exactly at their 32 MiB overlap (Phase B read 3–7 % more bytes,
ran ~10 % slower — machine-load and script differences, disclosed on the
figure). Each trend's *direction* is consistent within its own sweep; the
absolute cross-sweep join is approximate.

Caveats: `layer_order_learned` only; compute=0 only; region size fixed at 2 GiB.

Superseded prior claims: Campaign 11 Phase 3 expectation 5 ("wall-clock and
bytes move together, no divergence") — superseded by Campaign 12 Phase B for the
sub-32 MiB range; Phase 3's own 32–256 MiB numbers stand.

---

### Claim 6: Prefetch trades bytes for latency, and only at a loose enough budget; it is a net loss at a tight budget.

Evidence, synthetic (Campaign 12 Phase D, 128 MiB, learned;
`figure5_prefetch_total_fetches.csv`): compute=0 — arm E's total fetches
(demand + prefetch) **exceed** arm D's at every ratio. compute=400000 — E ≈ D
or below only at r ≥ 0.5.

Evidence, real model at equal budget (`results/livelock/phase3_real_model.csv`,
all four LIVELOCK FIX defects, `--protect-current off`):

- r ≥ 0.5: arm E reads **4–8 % more bytes** than arm D (84.9 vs 78.9, 62.9 vs
  59.7, 43.3 vs 41.9 GB) and cuts demand faults ~2× (766 vs 1734 at r=0.5) — a
  bytes-for-latency trade. The synthetic "E beats D on bytes under a heavy
  compute phase" regime **does not exist on this model** — real per-layer
  compute is ~51 k ns/MiB, ~30× lighter than the synthetic "400000" setting.
- r ≤ 0.375: arm E **completes** (3/3 reps, `stat_fetching_timeout = 0`) but is
  **not advantageous on bytes** — it reads +14 % / +13 % more than arm D
  (142.1 vs 125.0, 111.1 vs 98.5 GB). It *is* faster (r=0.25: 1.22 vs 1.12
  tok/s), so the trade is a latency win, not a pure loss as before.
- **The arm-E livelock is fixed** (was: `protect_current` on + r ≤ 0.375 →
  `rc = 124`, ~90× I/O amplification). LIVELOCK FIX Defect 2 makes the
  consumption signal fire pre-compute, so the declared cursor tracks the real
  read frontier; `protect_current` (whether on or off) then shields the chunks
  actually in use and `prefetch_admit` stops spinning. Phase 3b: the exact
  prior-livelock config now completes in ~44–54 s at r ∈ {0.25, 0.375}.

Figure: Figure 5 (synthetic), Figure 6 (real model).

Strength: **qualified.** The bytes-for-latency trade at r ≥ 0.5 is real and
replicated; "no byte benefit at tight budget" is confirmed on the real model,
now without a livelock. The metric (total fetches) replaced hit rate, which
Campaign 13 Phase B showed was a denominator artefact.

Caveats: the synthetic compute phase is a busy loop, not real matmul. **Operating
recommendation: run arm D (prefetch off) at r ≤ 0.375** — arm E completes there
but reads ~13 % more for a modest latency gain.

Superseded prior claims: (a) the "14–46 % prefetch hit-rate ceiling" as evidence
of mechanism saturation (items 10b–10e) — superseded by Campaign 13 Phase B;
Figure 5 uses total fetches. (b) The earlier caveat "WP2 Phase 2.3 expectation 3
was not run (BLOCKER 1)" — it ran (session 2) and the real model contradicted
the synthetic heavy-compute finding. (c) WP2 / Phase 3's framing of arm E's
tight-budget failure as a *hard deadlock / orphaned FETCHING slot* — the cleanup
session's gdb + 420 s counter trace showed it is a **livelock**. (d) The cleanup
session's "arm E livelocks at r ≤ 0.375 with `protect_current` on; mitigate by
config" and "reads 37–43 % more than arm D" — the LIVELOCK FIX (Defect 2's
pre-compute consumption signal) **fixes the livelock**: arm E completes at every
ratio with `protect_current` on or off (Phase 3b), and reads +13–14 % more than
arm D at r ≤ 0.375 (not 37–43 %; that gap was against a protect-on arm-D
baseline that itself carried the defects).

---

### Claim 7: Declared access order beats inferred access order and, given an accurate consumption signal, reaches the Belady optimum.

Evidence:

- **Synthetic, `--consumption-signal all-threads --protect-current off`**
  (`phase2_determinism.csv`, `phase2_sweep.csv`): `layer_order_declared` is
  **deterministic at all six Campaign-13-Phase-A cells** — including cell 3,
  which the session-2 WP0 heuristic never fully stabilised — and reads
  **≤ the heuristic at every sweep cell**: D/OPT = 1.000 at 8 MiB, 1.00–1.08 at
  128 MiB, both compute levels. It beats `layer_order_learned` on bytes
  everywhere the two are comparable.
- **Real model, `--protect-current off`** (the A-14 default on both paths — the
  eval callback now fires the consumption signal pre-compute, LIVELOCK FIX
  Defect 2): `results/livelock/phase3_real_model.csv` — D reads 13–69 % fewer
  bytes than arm C and 3–50 % fewer than arm A at every ratio; D/OPT = 1.08–1.13,
  deterministic (3/3 reps byte-identical). `figure6_llamacpp.csv`.
- Campaign 13 Phase A's diagnosis of the **learned** chain's
  fault-dispatch-order dependence stands; the declared policy is the recommended
  informed policy.

Figure: Figures 1–2 (synthetic), Figure 6 (real model).

Strength: **strong for byte efficiency, near-optimality, and — with the exact
signal — determinism.**

Caveats: (1) determinism is proven only where the consumption signal is exact.
On the real model, LIVELOCK FIX Defect 2 (fire the signal pre-compute) + Defect 4
(match the `embd` node) make it exact enough — all three Phase 3 reps are
byte-identical at every ratio, `--protect-current off`. The cleanup session's
"off ⇒ arm D +67–78 %" was a pre-fix artifact (cursor lagged a full layer,
`token_embd` never signalled). (2) `layer_order_declared` + prefetch (arm E)
reads ~13 % more than arm D at r ≤ 0.375 — a latency win, not a byte win — and
no longer livelocks (see Claim 6).

Superseded prior claims: (a) **session 1's Claim 7** ("declared order is
Belady-optimal at compute=0 but *worse* than the learned hedge under a
concurrent compute phase; non-deterministic at cells 5–6") — reversed first by
the session-2 WP0 heuristic, then fully by the final session's Phase 2
all-threads signal. (b) The session-2 statement that the WP0 heuristic "costs
~2 resident chunks / a 128 MiB D/OPT regression 1.06 → 1.15" — on the synthetic
path the all-threads signal removes that regression (1.15 → 1.08). (c) The
session-2 framing that the heuristic is "unnecessary / redundant" — **restored**
by the LIVELOCK FIX (A-14): true on both paths now. The cleanup session's
real-model revert to `protect_current on` was undone once Defects 2 and 4 made
the real-model consumption signal accurate; on vs off then moves arm D by
≤ 1.8 % (`phase3c_arm_d_protect_on.csv`). (d) This Claim body's earlier "real
model, `protect_current` on … D/OPT 1.09–1.14" — re-measured `--protect-current
off` with the four fixes: 13–69 % fewer bytes than arm C, D/OPT 1.08–1.13.

Only valid because WP1 completed.

---

### Claim 8: The core findings hold on a real model.

Evidence: **WP2 (session 2)** + **the final session's Phase 1** + **the LIVELOCK
FIX Phase 3 re-measurement** (equal budget, all four defects fixed,
`--protect-current off`). `wp2_gate_log.txt` / `phase3_correctness_gate.txt`
(correctness gate PASS — byte-identical tokens), `results/livelock/phase3_real_
model.csv`, `phase1_opt.csv`. Qwen2.5-3B-Instruct Q4_K_M through the userfaultfd
pager, CPU-only.

- **Kernel LRU (arm C)** thrashes on the real layer scan — 134–144 GB / 64
  tokens, ~1.0 tokens/s, at **every** ratio including 0.75. Identical to the
  synthetic degeneration.
- **App-authoritative residency (arm D)** beats arm A on bytes at **every**
  ratio at a genuinely equal budget (D/A = 0.97 / 0.89 / 0.71 / 0.57 / 0.50),
  and is within 13 % of the offline optimum (D/OPT = 1.08–1.13, deterministic).
- **Throughput scales** with budget for arm D (1.12 → 2.85 t/s, 2.5×) and does
  not for arm A / arm C — see Claim 9.
- The offline optimum is computable for the real workload's unequal chunk sizes.

Figure: Figures 6, 7.

Strength: **strong for the core thesis (Claims 1, 2, 4, 9); qualified /
contradicted for the prefetch follow-up (Claim 6).**

Caveats & what did NOT transfer: (1) **Claim 6** — real per-layer compute is
~30× lighter than the synthetic heavy setting; arm E never beats arm D on bytes
on this model and reads ~13 % more at r ≤ 0.375 (a latency win there, not a byte
win; it no longer livelocks). (2) One 3B dense model, CPU-only,
2 GiB — not larger models, MoE, or GPU offload. (3) The GGUF's tensors are stored
name-lex not layer order, and layer 21 is split across two chunks — the synthetic
uniform table hid this. (4) WSL2-only; the real-model arm-A points are
fault-stall-bound (920–1310 MiB/s, below the 3396 MiB/s O_DIRECT ceiling), so
host-cache contamination did not engage here, but it is unresolved in general.

Superseded prior claims: (a) **session 1's Claim 8 ("cannot be claimed — WP2 not
run")**. (b) WP2's own "arm D ≈ arm A at r=0.25" — that was the +256 MiB
`memory.max` margin WP2 gave arm A; the final session's Phase 1 re-ran at equal
budget and arm D wins at every ratio (parity at r=0.25 within the disclosed
asymmetry). (c) WP2 / Phase 3's "arm E collapsed at r=0.25 (hard deadlock)" then
Cleanup Phase 1's "it is a livelock, mitigate by config" — the LIVELOCK FIX
(Defect 2, pre-compute consumption signal) **fixes it**: arm E completes at every
ratio (Phase 3b). The operating limit ("no prefetch at r ≤ 0.375") is unchanged
— arm E completes there but reads ~13 % more than arm D. (d) The final session's
"arm C ~0.7–0.9 t/s" — the LIVELOCK FIX Phase 3 re-run measured arm C at
~1.0 t/s (byte-identical thrashing, machine-speed variance between sessions);
the flat-across-budget shape is unchanged. (e) "D/A = 0.98 / 0.89 / 0.71 / 0.58
/ 0.52, D/OPT 1.09–1.14" (Phase 1) → 0.97 / 0.89 / 0.71 / 0.57 / 0.50, D/OPT
1.08–1.13 (Phase 3, `--protect-current off` + the four fixes).

---

### Claim 9: The kernel cannot convert additional memory into throughput on a cyclic layer scan; an application-authoritative pager can.

Evidence: **LIVELOCK FIX Phase 3** (`results/livelock/phase3_real_model.csv`) for
arms C/D/E; **Phase 1** (`phase1_equal_budget.csv`) for arm A (no pager,
unchanged). Figure 7. Real model, equal budget, `layer_order_declared`,
`--protect-current off`, tokens/s (median of n=3) vs budget ratio
r ∈ {0.25, 0.375, 0.5, 0.625, 0.75}:

- **Arm A (kernel mmap):** 0.65 / 0.79 / 0.62 / 0.64 / 0.75 t/s — **no trend.**
  More `memory.max` does not help; LRU refetches the same working set every scan.
- **Arm C (kernel LRU via the pager):** 0.97 / 0.96 / 1.01 / 0.99 / 0.97 t/s —
  flat. Reads 134–144 GB every run independent of budget.
- **Arm D (`layer_order_declared`):** 1.12 / 1.35 / 1.65 / 2.17 / 2.85 t/s —
  **scales monotonically with budget, 2.5× from r=0.25 to r=0.75.**
- **Arm E:** 1.22 / 1.53 / 1.88 / 2.34 / 3.06 t/s — tracks D, slightly above it
  (fewer demand faults), and now completes at r=0.25 too.

All arms are far below the 13.2 t/s unconstrained baseline — the point is the
*shape*: A and C horizontal, D and E rising.

Figure: **Figure 7** (throughput scaling — the closing figure). Also Figure 6's
right panel.

Strength: **strong.** Deterministic direction, five ratios, clean separation
between the kernel arms (flat) and the informed arms (rising).

Caveats: WSL2-only, one 3B dense model, CPU-only. Arm A's absolute tokens/s is
reclaim-noisy (run-to-run ~2×); the flatness is the robust part. **Arm A is a
different run (Phase 1) than arms C/D/E (Phase 3), on a session that was ~20 %
faster overall** — that is why arm C reads ~1.0 t/s here vs ~0.8 in Phase 1
(byte-identical thrashing). The A-vs-C *absolute* gap is therefore not a
like-for-like comparison; what is robust is that **each** kernel arm is flat
across a 3× budget range while D and E rise. Arm D now rises monotonically
across all five ratios (the Phase 1 r=0.5→0.625 dip did not reproduce); the
r=0.25 vs r=0.75 endpoints are 2.5× apart and clean.

Superseded prior claims: the Phase 1 arm-D throughput row
0.91 / 1.20 / 1.57 / 1.51 / 1.95 t/s (2.1×, `protect_current` on) — the LIVELOCK
FIX Phase 3 re-run (`--protect-current off` + four defects) measured
1.12 / 1.35 / 1.65 / 2.17 / 2.85 t/s (2.5×, monotone). Arm C rose from
~0.8 to ~1.0 t/s (byte-identical thrashing; between-session machine variance).
Shapes unchanged.

---

### Claim 10: An arm-E failure at a tight budget looked like a mechanism interaction; it was two application bugs feeding each other, and fixing them fixed it.

Evidence: **Cleanup Phase 1** (diagnosis: `phase1_deadlock_fix.md`,
`repro_decisive.log`) + **LIVELOCK FIX Phases 0–3** (fix:
`results/livelock/phase0_cursor_diagnostic.md`, `phase1_fixes.md`,
`phase3_real_model.md`, `phase3b_arm_e_protect_on.csv`).

**The failure.** Real model, r ≤ 0.375, `layer_order_declared` +
`--prefetch-retention pinned --prefetch-depth 2 --protect-current on`: a 420 s
instrumented run showed `stat_absent_handled` climbing linearly 239 → 4250,
`stat_bytes_fetched` **48 GB → 910 GB** (~90× amplification), `stat_infeasible =
0`, fetch workers active throughout — a **livelock**, would complete in ~6 h.
The cleanup session diagnosed it as an interaction of two correct mechanisms
(pinned retention × current-chunk protection) and mitigated by config.

**The actual cause — two application-side bugs.** A later source review found:

1. **Off-by-one in the distance function.** `lo_declared_dist()` scanned
   `for d = 1..seq_len`, so the chunk being actively consumed (`seq[pos]`)
   matched only at `d == seq_len` — maximum distance, top eviction victim.
2. **The consumption signal fired one layer late.** `wp2_gen.cpp:eval_cb()`
   acted on the *post*-compute callback pass, so the declared cursor advanced
   only after layer N's weights had already been read — it lagged the real
   access by a full layer.
3. (compounding) **`token_embd` was never signalled at all** — `eval_cb`
   matched the node name `"inp_embd"` but llama.cpp names it `"embd"`.

Together: the cursor pointed at the wrong layer, the distance function ranked
the in-use chunk as the coldest, `protect_current` layered its lookback onto
that already-wrong ranking, and `prefetch_admit` evicted-and-refetched the
working set every token (`stat_prefetch_declined` → 2104, `stat_prefetches`
frozen at 77). The "mechanism interaction" was an artifact of the wrong cursor.

**The fix (LIVELOCK FIX, all four):** `lo_declared_dist` origin is
signal-mode-aware (`d` starts at 0 on the pre-consumption path); `eval_cb` acts
on the pre-compute pass and matches `"embd"`; a startup audit aborts if any
declared chunk gets zero signals in the first two passes; declined prefetches
back off 100 ms. Result: arm E completes at every ratio with `protect_current`
on **or** off (Phase 3b: the exact prior-livelock config finishes in ~44–54 s),
`stat_prefetch_declined` 2104 → 0–20, `stat_fetching_timeout = 0`.

**A-13 is independent and retained.** Every `CHUNK_FETCHING → ABSENT` drop path
issues `UFFDIO_WAKE` (`pager_abandon_fetch()`, 4 sites) and `pager_run`
watchdogs the `FETCHING` state (`--fetching-timeout-ms`, `stat_fetching_
timeout`). That fixes a *real, separate* latent bug — a deduped faulter left
blocked forever if a drop path forgets the wake — which the tight-budget churn
heavily exercised (`stat_prefetch_declined` = 2104). It is not what fixed the
livelock (the livelock was a policy/cursor pathology, not a lost wakeup);
`stat_fetching_timeout = 0` in every run now means both things — no orphaned
slot and no livelock.

Figure: none (Table 1 note; Claim 6).

Strength: **strong.** The failure was reproduced and instrumented (cleanup
session), then root-caused to specific lines and fixed, with the fix verified
against the exact prior-livelock configuration (Phase 3b).

Caveats: the fixes are application-side (the pager mechanism was never wrong);
one 3B model. The four fixes were applied together — no per-defect ablation, so
the claim is the compound result.

Superseded prior claims: (a) **this Claim's own prior framing** — "two
individually-correct mechanisms combine into a pathological failure … recorded,
not fixed … mitigate by config (`protect_current = on`)". The failure is an
off-by-one distance origin plus a post-compute consumption signal; both are
fixed; `protect_current` is now `off` by default (A-14) and the livelock does
not occur in either setting. (b) WP2 / final-session Phase 3's "hard deadlock /
orphaned `FETCHING` slot" — it was a livelock (cleanup session); now fixed.
