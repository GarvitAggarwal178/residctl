# Limitations — what this does not show

Ordered by how much a reviewer should care. The first two bound every number in
the project.

## 1. One shared WSL2 VM; no bare-metal comparison anywhere

Every measurement in this repository comes from one Windows-hosted WSL2 virtual
machine. There is no cross-platform data point. A readiness plan for a bare-metal
run exists ([`docs/bare-metal-plan.md`](bare-metal-plan.md) — minimum sweep,
environment capture, an S0–S3 pre-flight checklist) but it was never executed;
dual-booting is a human decision outside this project's scope.

The Windows VHDX host-side cache is unreachable by guest `drop_caches`. It shows
as real, fast `read_bytes` exceeding the 3396 MiB/s `O_DIRECT` ceiling — a
signature, not a zero. In the real-model runs arm A was fault-stall-bound
(920–1310 MiB/s, below the ceiling) so contamination did not engage there, but
7–11 of 20 synthetic arm-A/B cells in earlier campaigns exceeded it. The
platform I/O ceiling itself (~3400–4300 MiB/s aggregate, plateauing past two
threads) may be a genuine hardware/virtio limit or a WSL2 artifact — only bare
metal would tell.

## 2. One model

Qwen2.5-3B-Instruct, Q4_K_M, dense, CPU-only, a 2 GiB weight region. Not larger
models, not MoE routing, not GPU or partial offload, not any other model. The
core thesis (kernel LRU thrashes; app-authoritative residency reads far less and
sits near the offline optimum; throughput scales with budget) is confirmed
there. The synthetic "prefetch beats arm D on bytes under a heavy compute phase"
finding is *contradicted* there — real per-layer compute (~51 k ns/MiB) is ~30×
lighter than the synthetic setting where prefetch won.

## 3. The synthetic compute phase is a busy loop, not real matmul

It has a disclosed calibration overshoot (achieved ~1.5 M ns/MiB at the
"400000" setting). Real per-layer compute sits near the zero-compute end; the
synthetic sweeps bracketed reality but did not hit it. A synthetic knob swept
wide enough to produce a result can produce one that describes no real regime —
see the negative result in [`findings.md`](../results/findings.md).

## 4. `layer_order_declared`'s determinism guarantee holds only where the consumption signal is exact

On the synthetic path (`--consumption-signal all-threads`), all six
Campaign-13-Phase-A stress cells are deterministic. On the real model, the
LIVELOCK FIX made the eval-callback signal fire pre-compute, match the `"embd"`
node, and be audited at startup — all three Phase 3 reps are byte-identical at
every ratio with `--protect-current off`. A workload that declares its sequence
imprecisely (out-of-order or skipped chunks) still weakens the guarantee; the
forward-search path in `lo_declared_on_access` exists for that case but is
lightly tested. Under the full three-factor stress trigger
(`--driver-threads>1` ∧ `--lookahead-window>0` ∧ `--compute-ns-per-mib>0`) the
*learned* policy remains non-deterministic by design.

## 5. Prefetch (arm E) has no byte advantage at a tight budget

At r ≤ 0.375 on the real model arm E completes but reads ~13 % more than arm D
for only a modest latency gain — run arm D there. The earlier tight-budget
livelock (~90× I/O amplification) is fixed (A-14); the `pager_abandon_fetch` +
FETCHING-watchdog fix (A-13) remains as the independent fix for a latent
orphaned-slot class.

## 6. Region and chunk scale are mostly fixed

2 GiB region throughout; chunk size varied only in dedicated sweeps
(experiments [`09`](../experiments/09-chunk-size-sweep.md),
[`09d`](../experiments/09d-chunk-size-floor.md)). No sweep varied region size.

## 7. Ratios {0.375, 0.625} are real-model-only

The synthetic declared-vs-learned and consumption-signal grids use
{0.25, 0.5, 0.75}; the real-model sweeps add the two intermediate ratios.

## 8. Coverage gaps disclosed in the individual records

- `MADV_RANDOM`'s n=3 coverage: 2/3 reps at r=0.25, 1/3 at r=0.75 (two
  `timeout 180` hits) — never affected `best_mode` selection.
- `--sync-handler` × `guarded` admission, and `--sync-handler` × `pinned`
  retention, were wired but never swept in combination.
- A budget tight enough to make `E_INFEASIBLE`/OOM fire organically was never
  reached; the 256 MiB/r=0.25 arm-E cell is the closest approach.
- The `--policy-trace` resident-set bitmap is a fixed 64 bits — valid for
  `n_chunks ≤ 64` (covers every traced cell, including 128 MiB's 16 chunks),
  not valid at 8 MiB (256 chunks).

## Open questions still on the table

- **Why does `pread()` never show >1.0 concurrently-outstanding on arm D** even
  with genuine cross-chunk demand? The microbenchmark (experiment
  [`07c`](../experiments/07c-platform-io-microbenchmark.md)) tested raw
  concurrency directly but did not instrument *where* in the I/O stack the
  ceiling sits — likely needs host-side tooling or bare metal.
- **Why did prefetch hit rate FALL with heavier compute** (experiment
  [`08`](../experiments/08-compute-phase.md)), opposite the hypothesis? The
  count-based explanation is ruled out (experiment
  [`12b`](../experiments/12b-hitrate-count-hypothesis.md)); a per-prefetch
  survival-time trace is unbuilt.
- **Is arm B's ratio-invariant `read_bytes` genuine hint-driven reduction or
  partial host-cache contamination?** Not answerable from inside the guest.
- **Does the retention-direction reversal between experiments
  [`06`](../experiments/06-concurrent-demand.md) and
  [`07`](../experiments/07-lookahead-window.md)** share the fault-dispatch-order
  mechanism of experiment [`11`](../experiments/11-policy-determinism.md)?
  Plausible; not traced.
