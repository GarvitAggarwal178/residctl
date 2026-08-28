# CLAIMS — what the writeup may assert, and on what evidence

One entry per claim the report will make. `Strength`: strong (clean,
replicated, deterministic) / qualified (real but with a named caveat) /
preliminary (single regime, or synthetic-only where that matters).

Each body is current and standalone as written. The `Superseded prior claims`
field of every entry is the audit trail — it records what an earlier session
asserted and why it no longer holds.

**Policy-default note (read once):** on the **real model** (`residctl_llama`,
llama.cpp), `layer_order_declared` runs with `protect_current` **on** — its
eval-callback consumption signal fires *after* each layer's compute, so the
declared cursor lags the access and the heuristic that pins `seq[pos]` /
`seq[pos-1]` is load-bearing (cleanup Phase 1: with it off, arm D reads
+67–78 % more, deterministically). On the **synthetic replay driver**
(`replay_main`), `--consumption-signal all-threads` advances the cursor only
after every driver thread finishes a chunk, so the heuristic is redundant there
and `--protect-current` defaults **off**. The two paths genuinely differ.

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

Evidence, real model (`layer_order_declared`, `protect_current` on):
`phase1_equal_budget.csv` — arm D `read_bytes` falls with budget:
126.1 → 98.4 → 79.3 → 60.6 → 43.4 GB at r = 0.25 … 0.75, vs arm C flat at
134–144 GB and arm A flat-ish at 83–129 GB. **D beats arm C by 12–68 % and arm A
by 2–48 % at every ratio.** `figure6_llamacpp.csv`.

Figure: Figure 1 (synthetic), Figure 2, Figure 6 (real model, left panel).

Strength: **strong.** Deterministic in both regimes; the synthetic declared
policy equals Belady exactly at realistic chunk counts, and the real model lands
within 14 % of it at every ratio.

Caveats: (1) the real-model arm-A comparison carries a disclosed ~50 MiB
weight-cache asymmetry favouring the pager arms (≤ 10 % of B at r=0.25); the
arm-C comparison (arm C not host-cache-contaminated) is clean. (2) synthetic +
one real model only.

Superseded prior claims: (a) Campaign 12 Phase C / Campaign 13 Phase C reported
D/OPT 1.070–1.775 for the **learned** policy — still correct for
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
- `layer_order_declared` (real model, protect-on): D/OPT **1.09–1.14** at all
  five ratios (`phase1_opt.csv`, `figure6_llamacpp.csv`).

Figure: Figures 1, 2 (OPT dashed reference), Figure 6.

Strength: **strong.** The bound is exact (provable cyclic floor) and
independently cross-checked; it is also computable for the real model's
non-uniform chunk table.

Caveats: "distance from OPT" is regime-dependent — the informed policy *reaches*
OPT on the synthetic path with the exact signal, and is within 14 % on the real
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

Evidence, real model at equal budget (`phase1_equal_budget.csv`,
`phase1_verify.csv`):

- r ≥ 0.5: arm E reads **7–13 % more bytes** than arm D and cuts demand faults
  ~2× (a bytes-for-latency trade). The synthetic "E beats D on bytes under a
  heavy compute phase" regime **does not exist on this model** — real per-layer
  compute is ~51 k ns/MiB, ~30× lighter than the synthetic "400000" setting.
- r ≤ 0.375: arm E is a **net loss**. With `protect_current` on (the real-model
  default) it **livelocks** — the budget can't retain the two large chunks
  (`token_embd` 175 MiB, `output` 243 MiB) alongside the layers, they are
  re-fetched every token, and `protect_current` on also breaks `prefetch_admit`
  so the prefetcher spins (Cleanup Phase 1; `phase1_deadlock_fix.md`). With
  `protect_current` off, arm E completes but reads 37–43 % **more** than arm D
  (protect-on) at r ∈ {0.25, 0.375}.

Figure: Figure 5 (synthetic), Figure 6 (real model).

Strength: **qualified.** The bytes-for-latency trade at r ≥ 0.5 is real and
replicated; the "no benefit / net loss at tight budget" is confirmed on the real
model. The metric (total fetches) replaced hit rate, which Campaign 13 Phase B
showed was a denominator artefact.

Caveats: the synthetic compute phase is a busy loop, not real matmul. **Operating
recommendation: run arm D (prefetch off) at r ≤ 0.375.**

Superseded prior claims: (a) the "14–46 % prefetch hit-rate ceiling" as evidence
of mechanism saturation (items 10b–10e) — superseded by Campaign 13 Phase B;
Figure 5 uses total fetches. (b) The earlier caveat "WP2 Phase 2.3 expectation 3
was not run (BLOCKER 1)" — it ran (session 2) and the real model contradicted
the synthetic heavy-compute finding. (c) WP2 / Phase 3's framing of arm E's
tight-budget failure as a *hard deadlock / orphaned FETCHING slot* — the cleanup
session's gdb + 420 s counter trace show it is a **livelock** (steady forward
progress, ~90× I/O amplification), not a stuck state.

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
- **Real model, `protect_current` on** (the default there — its eval callback's
  notify lags the access): `phase1_equal_budget.csv` — D reads 12–68 % fewer
  bytes than arm C and 2–48 % fewer than arm A at every ratio; D/OPT = 1.09–1.14.
  `figure6_llamacpp.csv`.
- Campaign 13 Phase A's diagnosis of the **learned** chain's
  fault-dispatch-order dependence stands; the declared policy is the recommended
  informed policy.

Figure: Figures 1–2 (synthetic), Figure 6 (real model).

Strength: **strong for byte efficiency, near-optimality, and — with the exact
signal — determinism.**

Caveats: (1) determinism is proven only where the consumption signal is exact.
On the real model the signal lags, and `protect_current` on is the heuristic
that compensates; it is load-bearing there (Cleanup Phase 1: off ⇒ arm D
+67–78 % bytes). (2) `layer_order_declared` + prefetch (arm E) is a net loss at
r ≤ 0.375 and livelocks with `protect_current` on — see Claim 6.

Superseded prior claims: (a) **session 1's Claim 7** ("declared order is
Belady-optimal at compute=0 but *worse* than the learned hedge under a
concurrent compute phase; non-deterministic at cells 5–6") — reversed first by
the session-2 WP0 heuristic, then fully by the final session's Phase 2
all-threads signal. (b) The session-2 statement that the WP0 heuristic "costs
~2 resident chunks / a 128 MiB D/OPT regression 1.06 → 1.15" — on the synthetic
path the all-threads signal removes that regression (1.15 → 1.08). (c) The
session-2 framing that the heuristic is "unnecessary / redundant" — true on the
synthetic path (exact signal available) but **not** on the real model, where the
final-session Phase 2 default flip to `off` was reverted after Cleanup Phase 1
measured the regression.

Only valid because WP1 completed.

---

### Claim 8: The core findings hold on a real model.

Evidence: **WP2 (session 2)** + **the final session's Phase 1** (equal budget).
`wp2_gate_log.txt` (correctness gate PASS — byte-identical tokens),
`phase1_equal_budget.csv`, `phase1_opt.csv`, `wp2_llamacpp.md`. Qwen2.5-3B-
Instruct Q4_K_M through the userfaultfd pager, CPU-only.

- **Kernel LRU (arm C)** thrashes on the real layer scan — 134–144 GB / 64
  tokens, ~0.7–0.9 tokens/s, at **every** ratio including 0.75. Identical to the
  synthetic degeneration.
- **App-authoritative residency (arm D)** beats arm A on bytes at **every**
  ratio at a genuinely equal budget (D/A = 0.98 / 0.89 / 0.71 / 0.58 / 0.52),
  and is within 14 % of the offline optimum (D/OPT = 1.09–1.14).
- **Throughput scales** with budget for arm D (0.91 → 1.95 t/s) and does not for
  arm A / arm C — see Claim 9.
- The offline optimum is computable for the real workload's unequal chunk sizes.

Figure: Figures 6, 7.

Strength: **strong for the core thesis (Claims 1, 2, 4, 9); qualified /
contradicted for the prefetch follow-up (Claim 6).**

Caveats & what did NOT transfer: (1) **Claim 6** — real per-layer compute is
~30× lighter than the synthetic heavy setting; arm E never beats arm D on bytes
on this model and is a net loss at r ≤ 0.375. (2) One 3B dense model, CPU-only,
2 GiB — not larger models, MoE, or GPU offload. (3) The GGUF's tensors are stored
name-lex not layer order, and layer 21 is split across two chunks — the synthetic
uniform table hid this. (4) WSL2-only; the real-model arm-A points are
fault-stall-bound (920–1310 MiB/s, below the 3396 MiB/s O_DIRECT ceiling), so
host-cache contamination did not engage here, but it is unresolved in general.

Superseded prior claims: (a) **session 1's Claim 8 ("cannot be claimed — WP2 not
run")**. (b) WP2's own "arm D ≈ arm A at r=0.25" — that was the +256 MiB
`memory.max` margin WP2 gave arm A; the final session's Phase 1 re-ran at equal
budget and arm D wins at every ratio (parity at r=0.25 within the disclosed
asymmetry). (c) WP2 / Phase 3's "arm E collapsed at r=0.25 (hard deadlock)" —
it is a livelock (Cleanup Phase 1); the operating limit ("no prefetch at
r ≤ 0.375") is unchanged.

---

### Claim 9: The kernel cannot convert additional memory into throughput on a cyclic layer scan; an application-authoritative pager can.

Evidence: **Phase 1** (`phase1_equal_budget.csv`, `phase1_equal_budget.md`),
Figure 7. Real model, equal budget, `layer_order_declared` with `protect_current`
on (the real-model default), tokens/s (median of n=3) vs budget ratio
r ∈ {0.25, 0.375, 0.5, 0.625, 0.75}:

- **Arm A (kernel mmap):** 0.65 / 0.79 / 0.62 / 0.64 / 0.75 t/s — **no trend.**
  More `memory.max` does not help; LRU refetches the same working set every scan.
- **Arm C (kernel LRU via the pager):** 0.82 / 0.85 / 0.81 / 0.72 / 0.73 t/s —
  flat, slight decline. Reads 134–144 GB every run independent of budget.
- **Arm D (`layer_order_declared`):** 0.91 / 1.20 / 1.57 / 1.51 / 1.95 t/s —
  **scales with budget, 2.1× from r=0.25 to r=0.75.**
- Arm E tracks D where it completes (2.28 t/s at r=0.75).

All arms are far below the 13.2 t/s unconstrained baseline — the point is the
*shape*: A and C horizontal, D and E rising.

Figure: **Figure 7** (throughput scaling — the closing figure). Also Figure 6's
right panel.

Strength: **strong.** Deterministic direction, five ratios, clean separation
between the kernel arms (flat) and the informed arms (rising).

Caveats: WSL2-only, one 3B dense model, CPU-only. Arm A's absolute tokens/s is
reclaim-noisy (run-to-run ~2×); the flatness is the robust part. Arm D's
r=0.5→0.625 dip is within noise; the r=0.25 vs r=0.75 endpoints are 2.1× apart
and clean.

Superseded prior claims: none — this result existed only as a bullet in
`OVERNIGHT_SUMMARY.md` before Phase 1 measured it at five ratios with equal
budget and gave it a figure.

---

### Claim 10: Two individually-correct mechanisms — pinned prefetch retention and current-chunk protection — combine into a pathological failure at a tight budget, found by instrumentation and diagnosed from thread state.

Evidence: **Cleanup Phase 1** (`phase1_deadlock_fix.md`, `repro_decisive.log`,
`phase1_verify.csv`). Real model, r ≤ 0.375, `layer_order_declared` +
`--prefetch-retention pinned --prefetch-depth 2 --protect-current on`:

- A 420 s instrumented run: `stat_absent_handled` climbs linearly 239 → 4250,
  `stat_evictions` 312 → 4323, `stat_bytes_fetched` **48 GB → 910 GB** (a job
  that reads ~10 GB — ~90× amplification), `stat_infeasible = 0`,
  `stat_pin_broken = 0`. gdb backtraces at three points: fetch workers actively
  in `pread`, one always in `blk_io_schedule`, counters advancing. **This is a
  livelock, not a hard deadlock** — it would complete in ~6 hours.
- Mechanism: at a budget that cannot retain `token_embd` (175 MiB) + `output`
  (243 MiB) + the layers, the two large chunks are evicted between fetch and
  consumption (the real-model notify lags, so `protect_current` shields the
  *previous* token's chunks) and re-fetched every token; `protect_current` on
  also makes `prefetch_admit` decline nearly every prefetch
  (`stat_prefetch_declined` → 2104, `stat_prefetches` frozen at 77), so the
  prefetcher spins.

**What was done about it:**

- **A real robustness fix (retained):** every `CHUNK_FETCHING → ABSENT` drop
  path now issues `UFFDIO_WAKE` (`pager_abandon_fetch()`, 4 sites), and
  `pager_run`'s never-blocking loop watchdogs the `FETCHING` state
  (`--fetching-timeout-ms`, default 30 s, `stat_fetching_timeout`). This closes
  latent orphaned-slot paths that would hang a deduped faulter *if*
  `ensure_budget` ever went infeasible — and `stat_prefetch_declined` = 2104
  shows those paths are heavily exercised. T-1..T-7 PASS with it.
  `stat_fetching_timeout = 0` in every run (there is no orphaned slot; it is a
  livelock).
- **The livelock itself is recorded, not fixed** (the spec forbids changing a
  policy or adding a mechanism). Mitigation is by config:
  `protect_current = on` (the real-model default — also strictly better for
  arm D), and **run arm D, not arm E, at r ≤ 0.375**.

Figure: none (an operating limit, stated in Table 1 and Claim 6).

Strength: **strong as a negative result** — reproduced, instrumented, diagnosed
to the mechanism, mitigation stated.

Caveats: the FETCHING watchdog is a safety net for a class of bug (six of this
family have been found), not a fix for this instance. A proper fix would make
the dedup / `ensure_budget` path escalate rather than spin — left for a future
session (BLOCKERS.md FINDING 1).

Superseded prior claims: WP2 / Phase 3's "arm E collapse = a demand fault
deduped against an orphaned `FETCHING` slot; a hard deadlock" — the failure is a
livelock; the earlier SIGUSR1 dump read `resident_bytes` well under budget as
"pager idle" when the working set was in fact being churned.
