# Findings

The argument in eight steps, from the problem to the payoff. Each step names the
claim, the number, the figure, and the experiment record behind it.
[`claims.md`](claims.md) has the claim-by-claim evidence and caveats;
[`superseded.md`](superseded.md) has every number that moved and why.

All real-model numbers are Qwen2.5-3B-Instruct Q4_K_M, CPU-only, 64 generated
tokens, an equal weight-residency budget per arm, n=3. "GB" is decimal.

---

## 1. The kernel's page cache has no model of the future

On a cyclic layer scan whose working set exceeds the budget, LRU evicts exactly
the chunk needed soonest. Arm C (the kernel's own rule, run through the pager)
misses **~100 % at every budget ratio** and reads the whole model every scan.

- Synthetic: miss rate exactly 1.000 in every cell; `read_bytes` = the entire
  2 GiB region every run.
- Real model: 134–144 GB per 64 tokens at every ratio **including 0.75**,
  `absent_handled` 2561–2817 against 2304 layer transitions (a chunk evicted
  mid-scan re-faults).

Claim 1 · Figure `02-miss-rate-vs-optimal` · experiments
`03-corrected-harness`, `10-consolidated-sweep`, `21-livelock-real-model`.

## 2. This is not a synthetic artefact

The same degeneration happens on a real 3B model through llama.cpp +
userfaultfd. Arm C thrashes identically — a transformer layer scan has no
locality for LRU to exploit.

Claim 8 · Figure `06-real-model-bytes` · experiments
`14-real-model-integration`, `15-equal-budget-baseline`,
`21-livelock-real-model`.

## 3. The application knows its access order in advance

Declaring the layer sequence to the pager (`layer_order_declared`) lets it keep
the *right* chunks resident. Arm D reads **13–69 % fewer bytes than arm C** and
beats the kernel-mmap baseline at every budget (D/A = 0.97 / 0.89 / 0.71 / 0.57
/ 0.50 at r = 0.25 … 0.75). Deterministic — all three reps byte-identical at
every ratio.

Claims 2, 7 · Figure `06-real-model-bytes` · experiments
`13-declared-access-order`, `21-livelock-real-model`.

## 4. It is close to the offline optimum

D/OPT = **1.08–1.13** on the real model (65-pass Belady bound over the
workload-authored reference trace), and **exactly 1.00** on the synthetic
workload at realistic chunk counts once the consumption signal is exact. The
bound is provable (the cyclic-scan floor) and independently cross-checked.

Claims 4, 7 · experiments `15-equal-budget-baseline`, `16-consumption-signal`,
`21-livelock-real-model`.

## 5. The consumption signal must be exact — on both paths

The policy's next-use distance is only as good as its knowledge of *where in the
sequence the workload is now*.

- **Synthetic:** the naïve "thread 0 started this chunk" signal races the other
  driver threads and makes the policy non-deterministic under concurrency +
  compute. Firing it only when *all* threads finish the chunk restores
  determinism at all six stress cells and removes the last D/OPT gap at small
  chunk counts.
- **Real model:** the eval callback originally fired *after* each layer's
  compute and matched the wrong graph-node name, so the declared cursor lagged
  a full layer and `token_embd` was never signalled at all. Firing pre-compute,
  matching `"embd"`, and a startup audit that aborts on any zero-signal
  declared chunk fixed it.

With an accurate signal on both paths, the `protect_current` heuristic is
redundant and defaults **off** everywhere. The cleanup session's "off ⇒ arm D
+67–78 %" was an artifact of the two real-model bugs the fix removes.

Claims 7, 10 · experiments `16-consumption-signal`, `18-signal-audit`,
`19-livelock-fix`, `20-livelock-synthetic-recheck`, `21-livelock-real-model`.

## 6. Eviction only works because `memory.swap.max = 0`

Without it the kernel reclaims the pager's shmem pages to swap and the authority
leaks — `pgscan ≈ 120,687`, ~236 MiB reclaimed. With it the kernel enters
reclaim (`memory.events[high] = 37`), finds nothing eligible (`pgscan = pgsteal
= 0`), and the application's hole-punch is the only thing that frees memory.

Claim 3 · Figure `04-reclaim-authority` · the feasibility spike (S3d/S3e).

## 7. Prefetch is a narrow tool

It trades **~4–8 % more bytes for ~2× fewer demand faults** and lower tail
latency at r ≥ 0.5. At r ≤ 0.375 it completes but reads ~13 % more than arm D
for only a modest latency gain — run arm D there. The synthetic "prefetch beats
arm D on bytes under a compute phase" regime does not exist on this model.

The earlier tight-budget **livelock** (~90× I/O amplification with
`protect_current on`, mislabelled a hard deadlock, then a two-mechanism
interaction) was two application bugs feeding each other — an off-by-one distance
origin and a post-compute consumption signal — and is **fixed**: arm E now
completes at every ratio in either `protect_current` setting.

Claims 6, 10 · Figure `06-real-model-bytes` · experiments
`17-prefetch-collapse`, `17b-livelock-diagnosis`, `21-livelock-real-model`.

## 8. The payoff — throughput that scales with memory

Give the kernel more budget and tokens/s does not move: arm A ~0.7 and arm C
~1.0 t/s, **flat across a 3× budget range**. Give the app-authoritative pager
more budget and tokens/s rises **1.12 → 2.85 t/s (2.5×, monotone)**. All arms
sit far below the 13.2 t/s unconstrained baseline — the result is the *shape*:
the kernel arms are horizontal, the informed arms rise.

(Arm A is a separate run from arms C/D/E, on a session that was faster overall,
so the A-vs-C absolute gap is not a like-for-like comparison — the flatness of
each is the robust part.)

Claim 9 · Figure `07-throughput-scaling` · experiment `21-livelock-real-model`.

---

## The correctness floor under all of it

Byte-identical token sequences, `mmap` load vs `residctl` load, 32 tokens. The
userfaultfd pager serves correct weight data for a real transformer.
T-1…T-7 pass with `--eager-reconcile` after every code change,
`mismatches = 0`. A startup audit aborts if any declared chunk receives zero
consumption signals in the first two decode passes.
