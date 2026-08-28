# WRITEUP PACKAGE — everything the report needs

The project stops measuring after this session. This file is the single
reference for the writeup. Every number below traces to a named cell.

---

## 1. The abstract's numbers (with source cell)

| quantity | value | source |
|---|---|---|
| Byte reduction, app-authoritative vs kernel LRU (arm D vs arm C), real model | **12–68 %** (r=0.25 → 0.75: D 126.1 vs C 144.4; D 43.4 vs C 134.5 GB) | `phase1_equal_budget.csv` / Table 1 (FINAL) |
| Byte reduction, arm D vs kernel mmap (arm A), real model, equal budget | **2–48 %** (D/A = 0.98 / 0.89 / 0.71 / 0.58 / 0.52 at r = 0.25 … 0.75) | `phase1_equal_budget.csv` |
| Distance from offline optimum, arm D, real model | **D/OPT = 1.09–1.14** at all 5 ratios | `phase1_equal_budget.csv` + `phase1_opt.csv` |
| Distance from optimum, arm D, synthetic, final policy config | **D/OPT = 1.00** at 8 MiB (256 chunks), **1.00–1.08** at 128 MiB (16 chunks), both compute levels, deterministic — `--consumption-signal all-threads --protect-current off` (synthetic path only; the real model keeps `protect-current on`) | `phase2_sweep.csv`, `phase2_opt.csv` |
| Throughput scaling, arm D, real model | **0.91 → 1.95 tokens/s** (2.1×) as budget goes r=0.25 → 0.75; kernel arms flat at **0.6–0.9 t/s** at every budget | `phase1_equal_budget.csv` / Figure 7 |
| Kernel LRU miss rate (arm C), real model | **~1.00 at every budget ratio including 0.75** (`absent_handled` 2561–2817 vs 2304 layer transitions); reads 134–144 GB every run | `phase1_equal_budget.csv` |
| Reclaim-authority counters (`memory.swap.max = 0`) | `pgscan = pgsteal = 0`, `memory.events[high] = 37`; with swap: `pgscan ≈ 120,687`, `pgsteal ≈ 60,141`, ~236 MiB shmem reclaimed | spike S3d / S3e, `figure4_reclaim_authority.csv` |
| Correctness | byte-identical token sequences, `--load-mode mmap` vs `residctl`, 32 tokens; T-1..T-7 PASS after every code change | `wp2_gate_log.txt`, `correctness_harness_log.txt`, `t6_t7_log.txt` |
| Model | Qwen2.5-3B-Instruct Q4_K_M, 2,104,932,768 B, sha256 `626b4a66…62d`, CPU-only | `table2_final_environment.csv` |
| Per-layer compute (measured) | ~51,000 ns/MiB (≈ 2.1 ms/layer at the 13.2 t/s baseline) | `wp2_llamacpp.md` §2.4 |

---

## 2. Results narrative skeleton (the order the argument must build in)

One line per result. Start from the problem, end at the payoff.

1. **The kernel's page cache has no model of the future.** On a cyclic layer
   scan whose working set exceeds the budget, LRU evicts exactly the chunk
   needed soonest — arm C misses ~100 % at *every* budget ratio and reads the
   whole model every scan (Claim 1; Figure 2).
2. **This is not a synthetic artefact.** The same degeneration happens on a real
   3B model through llama.cpp + userfaultfd — arm C, 134–144 GB / 64 tokens,
   ~1 t/s, at r = 0.75 (Claim 8; Figure 6).
3. **The application knows its access order in advance.** Declaring the layer
   sequence to the pager (`layer_order_declared`) lets it keep the *right*
   chunks resident: arm D reads 12–68 % fewer bytes than arm C and beats the
   kernel-mmap baseline at every budget (Claims 2, 7; Figure 6).
4. **It is close to the offline optimum.** D/OPT = 1.09–1.14 on the real model,
   and exactly 1.00 on the synthetic workload at realistic chunk counts once the
   consumption signal is exact (Claims 4, 7).
5. **The consumption signal must be exact.** The naïve "thread 0 started this
   chunk" signal races the other driver threads and makes the policy
   non-deterministic under concurrency + compute; firing it only when *all*
   threads finish the chunk restores determinism at all six stress cells and
   removes the last D/OPT gap at small chunk counts (Phase 2). On the synthetic
   path that exact signal makes the heuristic redundant; on the real model the
   eval-callback signal lags the access, so the heuristic (`protect_current on`)
   is retained there — it is load-bearing (Cleanup Phase 1: off ⇒ arm D +67–78 %
   bytes).
6. **Eviction only works because `memory.swap.max = 0`.** Without it the kernel
   reclaims the pager's shmem pages to swap and the authority leaks; with it the
   kernel enters reclaim, finds nothing eligible, and the application's hole-punch
   is the only thing that frees memory (Claim 3; Figure 4).
7. **Prefetch is a narrow tool.** It trades ~7–13 % more bytes for ~2× fewer
   demand faults and lower tail latency at r ≥ 0.5; at r ≤ 0.375 it has no
   advantageous configuration — with `protect_current on` it *livelocks*
   (~90× I/O amplification), with it off it completes but reads 37–43 % more
   than arm D. Run arm D there (Claim 6; Claim 10).
8. **The payoff is throughput that scales with memory.** Give the kernel more
   budget and tokens/s does not move (arm A/C flat at 0.6–0.9 t/s across a 3×
   budget range); give the app-authoritative pager more budget and tokens/s
   rises 2.1× (Claim 9; Figure 7). *This is the closing figure.*

---

## 3. Numbers that must NOT be cited (the dead list)

For someone writing at 2 a.m. From PROJECT_STATE §6 + this session.

| do NOT cite | why | cite instead |
|---|---|---|
| Item 10 **V1** OPT values, fault-count rankings, "`MADV_RANDOM` fastest" | 3 defects: circular OPT input, arms not doing equal work, wrong metric | HARNESS_REPORT_V2 |
| **Prefetch hit rate** ("14–46 % ceiling") as a mechanism property | artefact of its own denominator (Campaign 13 Phase B) | total fetches (demand + prefetch) |
| Campaign 11 Phase 3/4 **arm A/B `read_bytes = 0`** | guest-side `drop_caches` redirect bug | Campaign 12 Phase A re-run |
| "**Arm D is 1.07–1.78× OPT**" as a property of the informed policy | true only for `layer_order_learned` and compute=400000; declared is 1.00–1.14 | regime-specific: learned vs declared, compute level |
| "**Declared order is worse than the learned hedge under a compute phase**" (session-1 WP1) | reversed by the WP0 fix, then by Phase 2's exact signal | Phase 2 (synthetic, `all-threads` signal): declared is deterministic and ≤ learned everywhere |
| "**The WP0 / protect-current heuristic is unnecessary**" (final-session Phase 2) | true only on the synthetic path (exact `all-threads` signal available); on the **real model** the eval-callback signal lags and the heuristic is load-bearing | Cleanup Phase 1: real-model default reverted to `protect_current on`; off ⇒ arm D +67–78 % bytes |
| "**Arm E beats arm D on bytes under a compute phase**" (synthetic, Campaign 11/13) | does not transfer — real compute ~51 k ns/MiB, ~30× lighter than the synthetic "heavy" setting | real model: E never beats D on bytes |
| "**Arm D ≈ arm A at r=0.25**" (WP2) | WP2 gave arm A a 256 MiB `memory.max` margin | Phase 1 equal budget: D beats A at every ratio |
| WP2's "**arm E collapsed — prefetch retention + current-chunk protection over-constrain the budget**", *and* Phase 3's "it is a **hard deadlock / orphaned `FETCHING` slot**" | both wrong — Cleanup Phase 1's gdb + 420 s counter trace show a **livelock**: steady forward progress (`stat_absent_handled` 239 → 4250 linear), ~90× I/O amplification, `stat_infeasible = 0`, would finish in ~6 h | Cleanup Phase 1 / Claim 10: livelock; run arm D at r ≤ 0.375; BLOCKERS.md FINDING 1 |
| The 5 Campaign-12-Phase-D **non-deterministic arm D cells' exact numbers** | one sample from a distribution (Campaign 13 Phase A) | excluded from any comparison needing a deterministic D |
| Any **bare-metal** number | none exist — every measurement is one shared WSL2 VM | say so |
| `layer_order_learned`'s **fault-dispatch-order chain** as "the application knows its order" | it infers from the past; only `layer_order_declared` is told | declared policy |

---

## 4. Methodology section (bullets)

- **Arm design.** A = kernel `mmap` + `madvise` (the "just let the kernel do it"
  baseline). B = A + `madvise` hints (informed kernel). C = pager with `lru`
  (isolates *authority* from *policy* — same mechanism, kernel-equivalent
  policy). D = pager with the informed policy, prefetch off. E = D + prefetch.
  OPT = offline Belady/MIN over the workload-authored reference trace.
- **The OPT bound is computable and exact.** The workload emits a ground-truth
  reference trace (`TRACE_TYPE_REFERENCE`, never the handler's fault trace —
  that would be circular). Belady runs offline over it; cross-checked against a
  naïve O(n²) solver (300/300) and against the provable cyclic-scan floor
  `n + (passes−1)·max(n−k, 0)` on every run (T-5). For unequal chunk sizes
  (`wp2_opt.c`) the objective is `Σ missed chunk sizes`, not `misses ×
  chunk_size`, and is floor-checked against `Σ distinct chunk sizes`.
- **`read_bytes` is the primary metric**, not fault count or wall-clock: it is
  the physical quantity the mechanism exists to reduce, it is comparable across
  arms doing equal work (every arm consumes every byte of every referenced
  chunk — A-4), and it is not confounded by CPU contention. Fault count and
  wall-clock are reported alongside, never instead.
- **The correctness harness (T-1..T-7)** runs `--eager-reconcile` and checks:
  no page served stale (T-1/2), no hang (T-3), equal work across arms (T-4),
  the Belady self-test (T-5), the dedup branches fire under load (T-6), and no
  fault is ever lost in a 60 s storm with a 120 s watchdog (T-7). Re-run after
  every code change this session — all PASS, `mismatches = 0`.
- **Pre-registration discipline.** Every phase states its expectations before
  the run; the report marks each HELD / DID NOT HOLD / PARTIAL against a
  measured value, and failures are reported with their mechanism, not dropped.
- **Equal budget.** The quantity equalised across arms is the weight-residency
  ceiling: `memory.max = B` for the kernel arm (it has no other lever),
  `budget_bytes = B` for the pager arms (+ a uniform 128 MiB `memory.max`
  margin for llama's non-weight memory, which the kernel arm absorbs within B).
  Residual asymmetry ~50 MiB, favouring the pager arms, disclosed.
- **Environment.** One shared WSL2 VM, cgroup v2, `memory.swap.max = 0`,
  ext4-on-VHDX, THP `madvise`. Full table: `table2_final_environment.csv`.

---

## 5. Limitations section (bullets, ordered by how much a reviewer cares)

1. **One shared WSL2 VM; no bare-metal comparison anywhere in the project.**
   A readiness plan exists (`BARE_METAL_PLAN.md`) but was never run. The
   Windows VHDX host cache is unreachable by guest `drop_caches` — real-model
   arm A here is fault-stall-bound (920–1310 MiB/s, below the 3396 MiB/s
   O_DIRECT ceiling) so it did not bite, but 7–11/20 synthetic arm-A/B cells in
   earlier campaigns exceeded the ceiling.
2. **One model.** Qwen2.5-3B dense, Q4_K_M, CPU-only, 2 GiB. Not larger models,
   not MoE routing, not GPU / partial offload. The core thesis (Claims 1, 2, 4,
   9) is confirmed there; the prefetch-vs-compute finding is contradicted there.
3. **The synthetic compute phase is not real matmul** — a busy loop with a
   disclosed calibration overshoot (achieved ~1.5 M ns/MiB at the "400000"
   setting). Real per-layer compute (~51 k ns/MiB) sits near the zero-compute
   end; the synthetic sweeps bracketed reality but did not hit it.
4. **`layer_order_declared`'s determinism guarantee holds only where the
   consumption signal is exact.** On the synthetic path (`--consumption-signal
   all-threads`) Phase 2 made all six Campaign-13-Phase-A stress cells
   deterministic. On the **real model** the eval-callback signal fires
   post-compute (lags the access), so the `protect_current on` heuristic is
   retained there and is load-bearing (Cleanup Phase 1: off ⇒ arm D +67–78 %
   bytes, deterministically). A workload that declares its sequence imprecisely
   weakens the guarantee further.
5. **Prefetch (arm E) has no advantageous configuration at r ≤ 0.375** on this
   model. With `protect_current on` it **livelocks** (~90× I/O amplification,
   steady progress, would finish in ~6 h — Cleanup Phase 1, not a hard
   deadlock); with it off it completes but reads 37–43 % more than arm D. The
   `pager_abandon_fetch` + FETCHING-watchdog fix (Cleanup Phase 1) closes the
   latent orphaned-slot paths but is not a fix for the livelock. Mitigation:
   run arm D (prefetch off), `protect_current on`, at r ≤ 0.375.
6. **Region and chunk scale are mostly fixed** — 2 GiB region throughout;
   chunk size varied only in dedicated sweeps.
7. **Ratios {0.375, 0.625} are real-model-only** — the synthetic declared-vs-
   learned and Phase 2 grids use {0.25, 0.5, 0.75}.
8. **The reference-trace / OPT machinery assumes the workload consumes chunks in
   exactly its declared order** — true for a transformer layer scan; a workload
   that reorders or skips would need the forward-search path in
   `lo_declared_on_access` exercised (it exists, lightly tested).

---

## 6. The five negative results worth reporting (with what each taught)

1. **The prefetch admission rule declined nothing** (item 10c Sweep 3:
   `stat_prefetch_declined = 0` at all 36 cells). *Taught:* the informed
   policy's own victim selection already subsumes "don't evict something needed
   sooner than the prefetch target" — a separate admission guard is redundant.
   A-6 kept as a recorded negative; A-9 (retention) became the real mechanism.
2. **Prefetch hit rate has a denominator artefact** (Campaign 13 Phase B).
   *Taught:* issuing more prefetches mechanically dilutes the rate; the
   "14–46 % ceiling" three items chased was never a mechanism property. Total
   fetches (the volume actually moved) shows no ceiling. Metric choice is a
   correctness concern, not a presentation one.
3. **The synthetic "prefetch beats arm D on bytes under a compute phase"
   finding does not transfer** (Phase 1 / WP2). *Taught:* real per-layer
   compute is ~30× lighter than the synthetic "heavy" setting where E won; a
   synthetic knob swept wide enough to produce a result can produce one that
   describes no real regime. Always calibrate the knob against the target.
4. **The hard barrier made async throughput untestable by construction**
   (item 10d → A-10). *Taught:* a driver that serialises at chunk boundaries
   can never generate the cross-chunk concurrency the async handler exists to
   exploit — the "device-busy flat regardless of `--fetch-workers`" result was
   measuring the driver, not the architecture. Replaced by the bounded
   lookahead window.
5. **Arm E collapses at a tight budget** (Phase 3 → Cleanup Phase 1). *Taught:*
   two individually sound mechanisms (pinned prefetch retention, current-chunk
   protection) combine into a **livelock** — the budget can't retain the two
   large chunks alongside the layers, they are re-fetched every token, and
   `protect_current` on also makes `prefetch_admit` decline nearly every
   prefetch so the prefetcher spins (~90× I/O amplification, `stat_infeasible =
   0`, steady forward progress). *Also taught:* Phase 3's SIGUSR1 dump read
   `resident_bytes` well under budget as "the pager is idle → deadlock" — it was
   actually churning; thread-state (gdb) and a long counter trace were needed to
   tell a livelock from a hang. Recorded as a 7th concurrency-class issue; the
   `FETCHING` watchdog + `pager_abandon_fetch` fix (Cleanup Phase 1) is a safety
   net for the *orphaned-slot* class, not for this livelock.

---

*Companion: `PROJECT_STATE.md` §§1–6 amended this session.*
