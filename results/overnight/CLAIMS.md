# CLAIMS — what the writeup may assert, and on what evidence

One entry per claim the report will make. `Strength`: strong (clean,
replicated, deterministic) / qualified (real but with a named caveat) /
preliminary (single regime, or synthetic-only where that matters).

---

## FINAL SESSION amendments (2026-08-28) — read these first

The claim bodies below are from the overnight sessions. The final measurement
session changes them as follows; the new numbers supersede the old on the
points named.

- **Claim 2 / Claim 4 / Claim 7 — the compute=400000 non-determinism and the
  128 MiB D/OPT regression are FIXED.** Phase 2 replaced the WP0 protect-current
  heuristic with the **exact all-threads consumption signal**
  (`--consumption-signal all-threads`, now default): `layer_order_declared` is
  **deterministic at all six Campaign-13-Phase-A cells** (including cell 3, which
  the heuristic never fully fixed) and reads **≤ the heuristic at every sweep
  cell**. At 128 MiB the D/OPT regression goes **1.15 → 1.08** at r=0.25/c=0,
  **1.15 → 1.04** at r=0.5, **1.09 → 1.00** at r=0.75 — both compute levels.
  At 8 MiB (256 chunks) D/OPT = **1.000** unchanged. `phase2_*.csv`,
  `phase2_consumption_signal.md`.
- **Claim 7 / Claim 8 — "arm D ≈ arm A at r=0.25" is superseded.** Phase 1
  re-ran the real-model sweep at a genuinely **equal budget** (`memory.max = B`
  for arm A; WP2 had given arm A +256 MiB). Arm D now beats arm A on bytes at
  **every** ratio: D/A = 0.98 / 0.89 / 0.71 / 0.58 / 0.52 at
  r = 0.25 … 0.75. At r=0.25 the margin (2.3 %) is within a disclosed ~50 MiB
  weight-cache asymmetry that favours D — honest read: **parity at r=0.25, clear
  D win at r ≥ 0.375** (WP2's "D loses at r=0.25" was the confound).
  D/OPT = **1.09–1.14 across all five ratios** on the real model.
  `phase1_equal_budget.md`.
- **Claim 7 — arm E's r=0.25 collapse is now characterised (Phase 3): a latent
  deadlock**, not budget pressure — a demand fault deduped against an orphaned
  `FETCHING` slot while retention-pinned + protect-current chunks block eviction
  (`resident_bytes` 148–403 MiB of a 526 MiB budget at the hang). **Phase 2's
  protect-off default alone resolves it**; `--prefetch-retention none` resolves
  it with the lowest bytes. **Stated operating limit: prefetch provides no
  benefit at r ≤ 0.375 on this model** (arm E's best completing config reads
  166.5 GB — *more* than arm D's 126.1 and arm C's 144.4). Recommend arm D
  (prefetch off) at r ≤ 0.375. `phase3_arm_e_collapse.md`, BLOCKERS.md FINDING 1.
- **Claim 6 — confirmed on the real model at equal budget.** Arm E reads 7–13 %
  more bytes than arm D at every completing ratio and cuts demand faults ~2×;
  the synthetic "E beats D on bytes under heavy compute" regime does not exist on
  this model (real compute ~51 k ns/MiB). Prefetch is a bytes-for-latency trade
  at r ≥ 0.5 and a net loss at r ≤ 0.375.
- **Claim 9 is new** (below) — the throughput-scaling result gets its own claim
  and its own figure (Figure 7).
- Figures 1–2 regenerated with Phase 2's `all-threads`+off arm-D data; Figure 6
  regenerated with Phase 1's equal-budget arm A and five ratios, arm E at r=0.25
  marked as the Phase 3 fallback config; Figure 7 added; Tables 1–2 regenerated
  (Table 2 now carries the real model hash). `phase4_figures.md`.

---

### Claim 1: Kernel LRU degenerates to full-pass thrashing on a cyclic reference string.

Evidence: `campaign12_phaseD_paper_table.csv` — arm C, every one of the 20
cells: `absent_handled == touches` (1280 at 8 MiB, 80 at 128 MiB), miss
rate exactly 1.000, `read_bytes == 10,737,418,240` (the whole 2 GiB region
every run). Reproduced in every sweep back to item 7's smoke test
(`lru` 40/40 faults). `results/overnight/figures/figure2_miss_rate.csv`.

Figure: Figure 2 (the flat line at 1.000); Figure 1 (arm C flat at the
region size).

Strength: **strong.** Deterministic, universal across every cell and every
sweep, and mechanically inevitable (the working set exceeds budget and LRU
evicts the chunk that will be needed soonest on a cyclic scan).

Caveats: only demonstrated on the cyclic-scan replay workload — but a
repeating layer scan is exactly the real llama.cpp access pattern (WP2
would have confirmed on a real model; not run — BLOCKER 1). A reviewer
could argue a real workload has enough irregularity to give LRU some hits;
the answer is that the irregularity would have to be large relative to the
budget deficit, and inference over fixed layers has none.

Superseded prior claims: none.

---

### Claim 2: Application-authoritative residency reduces bytes read per unit of work, and the reduction scales with available budget.

Evidence: `figure1_bytes_per_work.csv` / Table 1. compute=0, arm D
(`layer_order_declared`, WP1): read_bytes/touch falls monotonically as
budget rises — 8 MiB: 6.41 → 4.80 → 3.36 MiB at r=0.25/0.5/0.75; 128 MiB:
104.2 → 76.6 → 51.3 MiB — against arm C flat at 8.00 / 134.2 MiB and arm A
flat at ~8.06 / ~134 MiB. Declared arm D equals OPT exactly at all six
compute=0 cells (`wp1_declared_order.md` §1.4).

Figure: Figure 1 (central figure), Figure 2.

Strength: **strong at compute=0** (deterministic, equals the Belady
bound). **Qualified under a concurrent compute phase**: at
`--compute-ns-per-mib 400000` the declared policy becomes
non-deterministic and reads *more* than the learned policy at 5 of 6 cells
(up to 1.9× OPT) — see Claim 7's caveat.

Caveats: (1) the byte reduction is measured against a baseline (arm A) some
of whose points are host-cache contaminated (Table 1 flag `h`) — but arm A
and arm C agree closely on bytes and arm C is not contaminated, so the
"D reads far less than the kernel arms" comparison stands on arm C alone.
(2) synthetic workload only.

Superseded prior claims: Campaign 12 Phase C / Campaign 13 Phase C reported
D/OPT of 1.070–1.775 for the **learned** policy. WP1 supersedes this at
compute=0: the **declared** policy reads exactly OPT (D/OPT = 1.000). The
1.07–1.78 figures remain correct for `layer_order_learned` and for every
compute=400000 cell.

---

### Claim 3: Eviction under `memory.swap.max = 0` is authoritative — the kernel enters reclaim and finds nothing eligible.

Evidence: spike S3d vs S3e (`/root/spike/results/s3d_output.log`,
`s3e_summary.txt`, `s3e_verify.txt`), `figure4_reclaim_authority.csv`.
Under `swap.max=0`: `memory.events[high]=37` (the kernel *did* enter
reclaim), `pgscan=0`, `pgsteal=0`, `memory.stat[shmem]` unchanged at
600 MiB. With swap available: `pgscan≈120,687`, `pgsteal≈60,141`,
`high=512–515`, ~236 MiB of shmem reclaimed to swap (3 runs).

Figure: Figure 4.

Strength: **strong.** The project's cleanest causal result — a controlled
A/B on one cgroup setting, 3 replicates, unambiguous direction.

Caveats: measured in the feasibility spike, not re-run this session; on the
same WSL2 VM. The mechanism (no swap ⇒ anonymous/shmem pages are
unevictable ⇒ the application's `FALLOC_FL_PUNCH_HOLE` is the only thing
that frees them) is kernel-general, not WSL-specific.

Superseded prior claims: none — this claim currently exists only as prose
in the spike addendum; Figure 4 is its first visualisation.

---

### Claim 4: The offline optimal bound is computable for this workload and both policies are measurably distant from it.

Evidence: `belady_main` (naive O(n²) cross-check 300/300, exact cyclic-floor
check on every run — `wp1_correctness_harness_log.txt` T-5). OPT per cell
in `figure1_bytes_per_work.csv` / `wp1_sweep_opt.csv`. Distance:
`layer_order_learned` D/OPT 1.06–1.53 at the clean cells;
`layer_order_declared` D/OPT = 1.000 at compute=0 but 1.5–2.0 at
compute=400000.

Figure: Figures 1 and 2 (OPT dashed reference).

Strength: **strong.** The bound is exact (provable cyclic floor) and
independently cross-checked.

Caveats: the "declared = OPT" result means one policy is *not* measurably
distant from OPT in the deterministic regime — the claim should be phrased
"both policies are distant from OPT under a concurrent compute phase; at
compute=0 the declared policy reaches it."

Superseded prior claims: "both policies are measurably distant from OPT"
(Campaign 13 Phase C, D/OPT 1.070–1.775) is now regime-dependent.

---

### Claim 5: Chunk size trades byte efficiency against wall-clock, optimising in opposite directions.

Evidence: `figure3_chunk_size_tradeoff.csv` — Campaign 12 Phase B {4,8,16,32}
+ Campaign 11 Phase 3 {32,64,128,256} MiB, arm D, compute=0. read_bytes has
a shallow minimum near 8–16 MiB then climbs to 256 MiB; wall-clock has a
valley near 16–32 MiB and is worst at 4 MiB.

Figure: Figure 3.

Strength: **qualified.** The two source sweeps used different scripts and
did **not** agree exactly at their 32 MiB overlap point (Phase B read
3–7% more bytes and ran ~10% slower than Phase 3 at 32 MiB — machine-load
and script differences, disclosed on the figure). The *direction* of each
trend is consistent within each sweep; the absolute cross-sweep join is
approximate.

Caveats: `layer_order_learned` only; compute=0 only; region size fixed at
2 GiB throughout. Campaign 11 Phase 3's own "bytes and wall-clock move
together" finding was for its coarser 32–256 MiB range and is superseded
below 32 MiB.

Superseded prior claims: Campaign 11 Phase 3 expectation 5 ("wall-clock
and bytes move together, no divergence") — superseded by Campaign 12
Phase B and shown in Figure 3.

---

### Claim 6: Prefetch pays only when there is a compute phase to overlap against.

Evidence: `figure5_prefetch_total_fetches.csv` (Campaign 12 Phase D,
128 MiB, learned). compute=0: arm E's total fetches (demand + prefetch)
**exceed** arm D's at every ratio (71→98, 63→85, 57→74, 51→66, 41→53).
compute=400000: E ≈ D or below at r ≥ 0.5 (80→79 at r=0.5, 50→48 at
r=0.75) — the only cells where prefetch does not cost extra volume.

Figure: Figure 5.

Strength: **qualified.** The effect is real and replicated (Campaign 11
Phase 2 and Phase 4 independently), but the win is marginal (1–2 fetches)
and only appears at looser budgets; at r ≤ 0.375 prefetch still costs
volume even with compute. The metric itself (total fetches) replaced hit
rate, which Campaign 13 Phase B showed was an artifact of its own
denominator.

Caveats: synthetic compute phase (`--compute-ns-per-mib`, a busy loop),
not real matmul — WP2 Phase 2.3 expectation 3 was written specifically to
test this against real inference compute and was not run (BLOCKER 1).
WP1's declared-order arm E shows the same qualitative pattern
(`wp1_sweep.csv`).

Superseded prior claims: the "14–46% prefetch hit-rate ceiling" as
evidence of mechanism saturation (items 10b–10e) — superseded by Campaign
13 Phase B; Figure 5 uses the replacement metric.

---

### Claim 7: Declared access order outperforms inferred access order, and — with the WP0 consumption-signal fix — is Belady-optimal and deterministic at realistic chunk counts.

Evidence: `wp1_declared_order.md` (session-2 §1.4 amendment),
`wp1_sweep.csv` (re-swept post-fix), `wp1_determinism.csv`,
`wp1_policytrace_log.txt`; and WP2 `wp2_sweep.csv` (real model).

- **WP1, after the WP0 fix (commit `8c15d8b`):** `layer_order_declared`
  reads **exactly OPT (D/OPT = 1.000) at all six 8 MiB (256-chunk) cells,
  both compute levels**, and is deterministic there. It beats
  `layer_order_learned` on bytes at 23 of 24 arm-D cells. At 128 MiB
  (16 chunks) D/OPT = 1.09–1.15 — one regression, 128MiB/r=0.25/c=0
  (1.06 → 1.15), the cost of protecting 2 of only 4 budget chunks.
- **The WP0 fix eliminates the Campaign 13 Phase A non-determinism**: WP1
  §1.3 cells 5–6 go from non-deterministic (79–90 / 1316–1388) to
  deterministic (55 / 768 — the latter the exact 8 MiB/r=0.5 Belady
  floor). Residual mild non-determinism at cell 3 (51–54).
- **WP2 (real model):** D reads 24–51% fewer bytes than the kernel at
  r ≥ 0.5; D/OPT = 1.09–1.14 — *closer to optimal on real inference than
  synthetically*, because real per-layer compute (~51 k ns/MiB) is far
  lighter than the synthetic heavy setting.

Figure: Figures 1, 2 (WP1, post-fix), Figure 6 (WP2 real model).

Strength: **strong for byte efficiency and near-optimality; qualified on
determinism** (cell 3 still mildly non-deterministic; the pathological
cells are fixed).

Caveats: (1) the fix costs ~2 resident chunks — visible only at small
chunk counts (128 MiB / tight budget). (2) Arm E (declared + prefetch)
**collapsed at r=0.25 on the real model** (360 s timeout) — the fix's
current-chunk protection plus prefetch pinning over-constrains the tightest
budget. (3) The driver-side alternative for the signal ("notify =
finished", broader blast radius) was not tried — BLOCKER 2.

Superseded prior claims: **session 1's own Claim 7** ("declared order is
Belady-optimal at compute=0 but *worse* than the learned hedge under a
concurrent compute phase") — the WP0 fix reverses the compute=400000
regression entirely. Campaign 13 Phase A's diagnosis of the learned
chain's fault-dispatch-order dependence stands; the declared policy is now
the recommended informed policy.

Only valid because WP1 completed.

---

### Claim 8: The core findings hold on a real model.

Evidence: **WP2 (session 2).** `wp2_gate_log.txt` (correctness gate PASS —
byte-identical tokens), `wp2_sweep.csv`, `wp2_opt.csv`, `wp2_llamacpp.md`.
Qwen2.5-3B-Instruct Q4_K_M through the userfaultfd pager.

- Kernel LRU (arm C) degenerates to full-pass thrashing on the real layer
  scan — ~100% miss, 134–144 GB per 64 tokens, ~1 tokens/s, at **every**
  budget ratio including 0.75. Identical to the synthetic degeneration.
- Application-authoritative residency (arm D) reads 24–51% fewer bytes than
  the kernel at r ≥ 0.5 and is within 15% of the offline optimum
  (D/OPT = 1.09–1.14).
- The offline optimum is computable for the real workload with unequal
  chunk sizes (`wp2_opt.c`, `missed_bytes = Σ missed chunk sizes`,
  floor-checked).

Figure: Figure 6.

Strength: **strong for the core thesis (Claims 1, 2, 4); qualified /
contradicted for the follow-ups.**

Caveats & what did NOT transfer: (1) **Claim 6 (prefetch pays under a
compute phase) does not transfer** — real per-layer compute (~51 k ns/MiB)
is ~30× lighter than the synthetic heavy setting where arm E won; on the
real model E never beats D on bytes (it does win latency). (2) Arm D ≈
arm A at r=0.25 (the kernel keeps `memory.max` resident, exceeding D's
self-imposed budget). (3) Arm E collapsed at r=0.25. (4) One 3B dense
model, CPU-only, 2 GiB — not larger models, MoE, or GPU offload. (5) The
GGUF's tensors are stored in name-lex not layer order, and layer 21 is
split — the synthetic uniform table hid this.

Superseded prior claims: **session 1's Claim 8 ("cannot be claimed —
WP2 not run")**. The synthetic-only limitation in PROJECT_STATE §3 is now
partially lifted: the thesis is confirmed on one real model; the
prefetch-vs-compute finding is contradicted.

---

### Claim 9: The kernel cannot convert additional memory into throughput on a cyclic layer scan; an application-authoritative pager can.

Evidence: **Phase 1** (`phase1_equal_budget.csv`, `phase1_equal_budget.md`),
Figure 7. Real model, equal budget, tokens/s (median of n=3) vs budget ratio
r ∈ {0.25, 0.375, 0.5, 0.625, 0.75}:

- **Arm A (kernel mmap):** 0.65 / 0.79 / 0.62 / 0.64 / 0.75 t/s — **no trend**.
  More `memory.max` does not help; the kernel's LRU refetches the same working
  set every scan regardless of how much budget it is given.
- **Arm C (kernel LRU via the pager):** 0.82 / 0.85 / 0.81 / 0.72 / 0.73 t/s —
  flat, slight decline. Reads 134–144 GB every run independent of budget.
- **Arm D (`layer_order_declared`):** 0.91 / 1.20 / 1.57 / 1.51 / 1.95 t/s —
  **scales with budget, 2.1× from r=0.25 to r=0.75.**
- Arm E similar to D where it completes (2.28 t/s at r=0.75).

All arms are far below the 13.2 t/s unconstrained baseline — the point is the
*shape*: A and C are horizontal, D and E slope up. The kernel treats extra
memory as more cache for a working set it will thrash anyway; the
app-authoritative policy treats it as more of the *right* chunks kept resident.

Figure: **Figure 7** (throughput scaling — the project's single strongest
picture). Also visible in Figure 6's right panel.

Strength: **strong.** Deterministic direction, five ratios, clean separation
between the kernel arms (flat) and the informed arms (rising). The absolute
values are noisy for arm A (reclaim-bound, run-to-run variance ~2×) but the flat
*trend* is unambiguous across all five ratios.

Caveats: WSL2-only, one 3B dense model, CPU-only. Arm A's absolute tokens/s is
noisy (the kernel's reclaim behaviour under `memory.max` pressure is not
stable); the flatness is the robust part. Arm D's r=0.5→0.625 dip is within
run-to-run noise; the endpoints (r=0.25 vs r=0.75) are 2.1× apart and clean.

Superseded prior claims: none — this result existed only as a bullet in
`OVERNIGHT_SUMMARY.md` ("A/C flat ~1 t/s; D/E scale 1.1→2.8") before Phase 1
measured it at five ratios with equal budget and gave it a figure.
