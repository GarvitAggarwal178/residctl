# WP3 — Figures and final data package · box: 2h · analysis only

**This work package runs even if WP1 and WP2 both failed.** It uses data already
on disk. If the night goes badly, this is what makes the writeup possible.

No new sweeps. No mechanism changes. Read the CSVs, produce the figures, produce
the tables.

Use matplotlib. Write PNG at 200 dpi **and** the underlying CSV for each figure,
so the numbers behind every plot are inspectable and the figure can be
regenerated without re-reading the source data.

Output to `results/overnight/figures/`.

---

## FIGURE 1 — Bytes read per unit of work, by budget ratio

The paper's central figure.

X: budget ratio. Y: `read_bytes` per touch (MiB). One line per arm (A, C, D, E)
plus OPT as a dashed reference. One panel per chunk size {8 MiB, 128 MiB}.

Source: Campaign 12 Phase D, with Campaign 12 Phase A's repaired arm A/B numbers.
Use WP1's declared-order arm D/E if WP1 completed; otherwise use the learned
policy and say so in the caption.

**Exclude the five non-deterministic arm D cells** identified in Campaign 13
Phase A, and mark the exclusion visibly on the figure — a gap in the line, not a
silent interpolation.

**Mark every arm A point whose achieved bandwidth exceeded 3396 MiB/s** with a
distinct symbol. That is the host-cache signature and the reader needs to see
which baseline points are contaminated.

## FIGURE 2 — Miss rate against the optimal bound

X: budget ratio. Y: demand faults as a fraction of total references. Lines for C,
D, E, and OPT.

The point of this figure is that C sits flat at 1.000 while D and E descend
toward OPT. It is the clearest single picture of the thesis.

## FIGURE 3 — Chunk size trade-off

X: chunk size {4, 8, 16, 32, 64, 128, 256} MiB, log scale. Two Y axes: `read_bytes`
(left) and wall-clock (right). Arm D only, one line per budget ratio.

Source: Campaign 11 Phase 3 and Campaign 12 Phase B combined. Note in the caption
that the two came from different sweeps and state whether the 32 MiB overlap
point agreed between them.

This figure shows the divergence — bytes minimise small, wall-clock minimises
large. Make that visible.

## FIGURE 4 — The reclaim-authority result

A two-bar comparison, from the spike's S3/S3e data:

| Condition | `pgscan` | `pgsteal` | shmem reclaimed |
|---|---|---|---|
| `memory.swap.max = 0` | 0 | 0 | 0 MiB |
| swap available | ~120,600 | ~60,150 | ~236 MiB |

Annotate with the `memory.events[high]` counts (37 vs 512-515).

This is the project's strongest single causal result and it currently exists only
as prose in the spike addendum. It deserves a figure.

## FIGURE 5 — Prefetch: total fetches, not hit rate

X: compute setting {0, 400000}. Y: total fetches (demand + prefetch). Grouped
bars for D and E, one group per budget ratio.

Caption must state that hit rate was the metric used through Campaign 12 and was
found in Campaign 13 Phase B to be an artifact of its own denominator; total
fetches is the volume metric that replaced it.

## FIGURE 6 — llama.cpp, if WP2 produced data

Same structure as Figure 1, real workload. If WP2 did not complete, skip this
figure and note the omission in the index.

---

## TABLE 1 — The paper's main results table

One table, markdown and CSV. Rows: arm × budget ratio × chunk size × compute
setting. Columns: `read_bytes`/touch, total fetches, demand faults, wall-clock,
D/OPT.

Mark every cell that is excluded, contaminated, or superseded, with a footnote
key. A reader must be able to tell at a glance which numbers are clean.

## TABLE 2 — Environment and configuration

Every value needed to reproduce: kernel, `shmem_enabled`, THP settings,
filesystem, cgroup version, `memory.swap.max`, region size, chunk sizes, handler
mode, fetch workers, driver threads, lookahead window, prefetch depth, retention
mode, policy, model file hash if WP2 ran.

---

## THE CLAIMS FILE

Write `results/overnight/CLAIMS.md`. For each claim the writeup will make, one
entry:

```
### Claim: <one sentence, as it would appear in the report>
Evidence: <file, table, cell — specific>
Figure: <which figure shows it, if any>
Strength: strong / qualified / preliminary
Caveats: <what a reviewer would attack, and the honest answer>
Superseded prior claims: <if this replaces something in an earlier report>
```

Cover at minimum:

1. Kernel LRU degenerates to full-pass thrashing on a cyclic reference string.
2. Application-authoritative residency reduces bytes read per unit of work, and
   the reduction scales with available budget.
3. Eviction under `memory.swap.max = 0` is authoritative — the kernel enters
   reclaim and finds nothing eligible.
4. The offline optimal bound is computable for this workload and both policies
   are measurably distant from it.
5. Chunk size trades byte efficiency against wall-clock, optimising in opposite
   directions.
6. Prefetch pays only when there is a compute phase to overlap against.
7. Declared access order outperforms inferred access order — **only if WP1
   completed**.
8. The findings hold on a real model — **only if WP2 completed**.

For every claim, the Caveats field must be filled honestly. Where the evidence is
contaminated (arm A host-cache), non-deterministic (the five excluded cells), or
synthetic-only, say so there. This file is what prevents an overclaim reaching
the report.

---

## Report

`results/overnight/wp3_figures.md` — an index of every figure and table produced,
what data each came from, and any figure that could not be produced and why.