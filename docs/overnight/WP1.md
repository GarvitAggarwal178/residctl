# WP1 — Declared access order · box: 3h

## The defect

Campaign 13 Phase A found that `layer_order` builds its successor chain from
**fault-dispatch order** — it learns the access sequence by observing which
chunks are missed, in the order the handler dispatches them.

That contradicts the project's central claim. `MECHANISM_SPEC.md` §1 and the
proposal's problem statement both argue that the kernel must infer the future
from the past while **the application knows its access order in advance**. A
policy that derives its chain from observed faults is doing inference from the
past — structurally the same thing the kernel does, one layer up.

For a cyclic workload the learned chain converges to the true order after roughly
one pass, which is why the results are real. But three consequences follow:

1. The timing dependence Campaign 13 Phase A isolated is downstream of this. A
   declared static sequence cannot be perturbed by fault-dispatch order, so the
   five non-deterministic cells should become deterministic.
2. D/OPT (measured 1.070-1.775) includes a learning cost — the first pass is
   spent building a chain the application could have supplied at startup.
3. The claim "the application knows its access order" is currently not
   implemented.

## What to build

**Do not delete the learned policy.** Keep it, rename it, and measure both. The
comparison is a result: it quantifies what declared application knowledge is
worth over inferring the same pattern.

### 1.1 — Declaration interface · 45 min

Add to the policy interface:

```c
// Declare the workload's access sequence in advance. chunk_ids is the
// reference string: the order in which chunks will be touched, one pass.
// The policy may assume the sequence repeats cyclically.
void policy_declare_sequence(region_t *r, const uint32_t *chunk_ids, uint32_t n);
```

- The replay driver already generates the reference sequence. It calls this at
  startup, before the first touch.
- `layer_order_declared` computes `next_use_distance(chunk)` as a lookup: given
  the current position in the declared sequence, the distance to the next
  occurrence of `chunk`. Precompute a next-use table once at declaration time —
  it is a single reverse pass over the sequence, the same computation the Belady
  solver already does offline. Reuse that code if it is factored out; if not, do
  not refactor the solver, write it separately and cross-check the two agree.
- `predict_next(cursor)` returns the chunk at the next position in the declared
  sequence, not a chain walk.
- **Position tracking must not depend on fetch completion order.** Advance the
  position from `pager_notify_access()` — the workload's own consumption signal,
  which the driver already calls once per (pass, chunk) — never from `on_fault`
  or `on_resident`.

### 1.2 — Policy naming · 15 min

- Existing `layer_order` → rename to **`layer_order_learned`**. Behaviour
  unchanged, byte-for-byte.
- New policy → **`layer_order_declared`**.
- `--policy` accepts both. **Default is `layer_order_declared`.**
- `lru` unchanged.

**Verification gate — STOP-AND-REPORT:** `--policy layer_order_learned` must
reproduce Campaign 12 Phase D's arm D numbers exactly at a deterministic cell
(128 MiB, r=0.5, compute=0, threads=8, window=1). If it does not, the rename
changed behaviour and nothing downstream is valid.

### 1.3 — Determinism check · 30 min

Run `layer_order_declared` at Campaign 13 Phase A's exact A.2 grid — the six
cells crossing threads / window / compute — **n=5 each**, r=0.5, 128 MiB.

Report `absent_handled` for every rep of every cell.

**Expected:** all reps identical within a cell, including cell 5 (threads=8,
window=1, compute=400000) where `layer_order_learned` was non-deterministic.

If `layer_order_declared` is still non-deterministic, that is a finding — report
the full distribution and the `--policy-trace` divergence point, exactly as
Campaign 13 Phase A did. Do not attempt a further fix; record it and continue to
1.4.

### 1.4 — Learned vs declared sweep · 60 min

Arms C, D (both policies), E (both policies), OPT.

Grid: chunk size ∈ {8 MiB, 128 MiB} × ratio ∈ {0.25, 0.5, 0.75} ×
`--compute-ns-per-mib` ∈ {0, 400000}, n=3.

Fixed: async handler, `--fetch-workers 4`, `--driver-threads 8`,
`--lookahead-window 1`, `--prefetch-depth 2`, `--prefetch-retention pinned`.

Arms A and B are not needed here — this is a policy comparison and the baseline
is unchanged. Skip them and save the time.

Report per cell: `read_bytes`, total fetches (demand + prefetch), demand faults,
wall-clock, D/OPT, and the declared-minus-learned delta on each.

### Pre-registered expectations

Record held / did not hold with numbers. Do not tune to hit them.

1. `layer_order_declared` is deterministic at every cell in 1.3.
2. `layer_order_declared` reads fewer bytes than `layer_order_learned` at every
   cell — the learning cost is eliminated.
3. The margin is larger at 128 MiB than at 8 MiB, because with 16 chunks a
   one-pass learning cost is a larger fraction of the run than with 256.
4. D/OPT improves under declared order at every cell.
5. The five cells Campaign 13 Phase A flagged as non-deterministic become
   deterministic and cease to approach or exceed arm C.

If expectation 2 fails — declared order reads *more* bytes anywhere — report the
cell and the `--policy-trace` divergence. Do not rationalise it.

### 1.5 — Correctness · 30 min

Re-run T-1..T-7 with `--eager-reconcile` under both policies. All must pass. A
T-7 failure is a hard stop per the runbook.

## Spec amendment

**A-12 (§8):** the policy interface accepts a declared access sequence from the
workload. `layer_order_declared` computes next-use distance from that sequence;
`layer_order_learned` (formerly `layer_order`) infers it from fault-dispatch
order and is retained as a comparison arm. Record that the learned variant was
the project's implementation through Campaign 13, that Campaign 13 Phase A found
its chain construction timing-dependent, and that this amendment aligns the
implementation with §1's claim that the application knows its access order in
advance.

## Report

`results/overnight/wp1_declared_order.md`. Standard structure: verdict, machine
exclusivity, each sub-phase, expectations 1-5 held/not-held with numbers,
anomalies, what was not tested, final check.

State plainly in the verdict whether the declared policy is deterministic and
whether it beats the learned policy on bytes.