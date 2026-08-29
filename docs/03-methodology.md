# Methodology

## Arms

Every experiment compares the same read workload under different owners of the
replacement decision.

| arm | configuration | what it isolates |
|---|---|---|
| **A** | kernel `mmap` + `madvise` (best mode reported) | the "just let the kernel do it" baseline |
| **B** | A + `MADV_WILLNEED` / `MADV_PAGEOUT` hints | an *informed* kernel — does advice close the gap? |
| **C** | pager + `lru`, prefetch off | **authority without policy** — same mechanism as D/E, but the kernel's own replacement rule. Isolates the value of *authority* from the value of a *good policy*. |
| **D** | pager + `layer_order_declared`, prefetch off | the informed policy |
| **E** | D + prefetch | what speculation adds |
| **OPT** | offline Belady over the workload-authored reference trace | the bound |

Arm C is the load-bearing control: if D beats C, the win is the policy; if C
already beats A, the win is authority (a real eviction the kernel can't undo).

## The primary metric is `read_bytes`

Not fault count, not wall-clock. `read_bytes` is the physical quantity the
mechanism exists to reduce; it is comparable across arms doing equal work; and
it is not confounded by CPU contention. Fault count and wall-clock are reported
alongside, never instead.

**Equal work (amendment A-4):** every arm — A/B included — consumes *every page*
of every referenced chunk, the way a real consumer reads every byte of a layer's
weights. A one-byte sentinel touch makes arm A read orders of magnitude less
than the pager arms and biases `madvise` mode selection. The read loop
accumulates into a `volatile` sink, verified against `-O2` elision by `objdump`.

## The offline optimum is computable and exact

The workload emits a ground-truth **reference trace** (`TRACE_TYPE_REFERENCE`) —
one record per access, hit or miss, in true order. This is never the handler's
fault trace, which would make the bound circular (amendment A-1). Belady runs
offline over it: a next-use index in one reverse pass, then a max-heap
simulation, O(n log k). For the real model's unequal chunk sizes the objective
is `Σ missed chunk sizes`, not `misses × chunk_size`.

Every solver run is checked two ways:

- against a naive O(n²) next-occurrence scan over many random cases (the primary
  correctness gate);
- against the provable cyclic-scan floor `n + (passes−1)·max(n−k, 0)` whenever
  the input is a strict cyclic scan (amendment A-2) — any result *below* this is
  a mathematical impossibility, which is what caught the circular-input bug.

## Equal budget

The quantity equalised across arms is the weight-residency ceiling:
`memory.max = B` for the kernel arm (it has no other lever), `budget_bytes = B`
for the pager arms, plus a uniform 128 MiB `memory.max` margin for the
integration's non-weight memory (which the kernel arm absorbs within B). The
residual asymmetry — ~50 MiB, favouring the pager arms — is disclosed. Budget
ratios are **swept** (r ∈ {0.25, 0.375, 0.5, 0.625, 0.75} on the real model),
never tuned to one point; the report is the sensitivity curve.

## Pre-registration and censoring

Every phase states its expectations *before* the run. The report marks each
HELD / DID NOT HOLD / PARTIAL against a measured value, and a failure is
reported with its mechanism, not dropped.

An `E_INFEASIBLE` or OOM kill at budget *B* is a **censored data point** —
"policy infeasible at *B*" — plotted, never discarded. The discriminator between
a finding and a bug: if the pager's accounting said it was under budget and
`memory.stat[shmem]` disagreed, that is a bug (fix and rerun); if the accounting
was correct and the budget was simply infeasible, that is the data point.

## The correctness harness (T-1 … T-7)

Runs with `--eager-reconcile` (reconcile against `memory.stat[shmem]` on every
fetch, not every 16th) after every code change. All pass, `mismatches = 0`.
T-1 (full region read under a 25 % budget, every sample checked, all policies)
and T-3 (8-thread storm) are the ones that catch a broken eviction path — a
punched hole reads as zeros, which is a plausible-looking wrong tensor, not a
crash. T-6/T-7 confirm the dedup branches fire and no fault is ever lost in a
60 s storm with a 120 s watchdog. The real-model integration adds a startup
audit that aborts if any workload-declared chunk gets zero consumption signals
in the first two decode passes.

## Machine exclusivity

No other workload on the machine during a measurement. Checked before and after
every phase that runs code. The feasibility spike was contaminated by exactly
this.

## Environment

One shared WSL2 VM, cgroup v2, `memory.swap.max = 0`, ext4-on-VHDX, THP
`madvise`. Full table: [`results/figures/table-2-environment.md`](../results/figures/table-2-environment.md).
Every measurement in the project comes from this one VM — see
[`05-limitations.md`](05-limitations.md).
