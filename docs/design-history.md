# Design history

Decisions that shaped the implementation and are not obvious from the code —
especially roads not taken and why. Newest first.

---

## 2026-08-29 — consumption-signal timing: per-path signal mode, not one uniform rule

**Context.** `layer_order_declared`'s next-use distance
(`policy.c:lo_declared_dist`) is a lookup into the workload's declared access
sequence from the current consumption position `pos`. The distance the loop
assigns to `seq[pos]` — the chunk the cursor points at — depends on whether the
workload's consumption signal (`pager_notify_access()`) fires **before** or
**after** the read it announces:

- **fires after the read** (post-consumption): `seq[pos]` was just fully
  consumed; its next use is a whole cycle away → the scan starts at `d = 1` and
  `seq[pos]` gets distance `seq_len` (furthest → top eviction victim). Correct
  Belady for this timing.
- **fires before the read** (pre-consumption): `seq[pos]`'s use is imminent →
  the scan starts at `d = 0` and `seq[pos]` gets distance 0 (protected).

The original code scanned `for d = 1..seq_len` unconditionally — post-consumption
semantics applied to every caller. That is correct for the synthetic
`--consumption-signal all-threads` path (the driver advances `pos` only once
every thread has finished a chunk) but an off-by-one for the real model, where
`wp2_gen.cpp:eval_cb()` now fires the signal on the eval callback's **pre**-compute
pass (LIVELOCK FIX Defect 2).

**Decision.** `lo_declared_dist` takes a **signal mode**
(`policy_set_signal_mode`, default post = the pre-fix behaviour byte-for-byte).
Each caller sets it from how its own notify is wired:

| caller | notify timing | mode |
|---|---|---|
| `replay_main` + `--consumption-signal all-threads` (default) | after full step consumption | post |
| `replay_main` + `--consumption-signal tid0` | before the read | pre |
| `residctl_llama` (real model, Defect 2 in) | before compute of the layer | pre |

**Road not taken — fire notify pre-consumption everywhere.** The cleaner
uniform design is to make *every* path fire the signal before the read and drop
the mode switch entirely: one rule, `d` always starts at 0, and the
`--protect-current` heuristic disappears with it. It was **not adopted here**
because of its blast radius on the synthetic results. The synthetic
`all-threads` path is post-consumption by construction (the "exact" signal the
final measurement session was built around), and switching it to pre would move
every arm-D and arm-E number in `results/final/phase2_*.csv` and every figure
and claim derived from them. This session's Phase 2 expectation was explicitly
"the synthetic path is unchanged; any change there is a regression to
investigate," so a uniform pre-consumption rewrite is out of scope. It is the
right direction for a future engine-integration milestone (item 11) where the
synthetic driver is retired.

**Deliberate deviation — the serial `replay_cyclic` path.** `replay_cyclic`
(used for `--driver-threads 1`) fires `pager_notify_access()` immediately
before the read loop — genuinely pre-consumption — but `replay_main` sets the
mode from `consumption_signal_all` only, which defaults to 1 (all-threads =
post). So the serial path runs on **post** semantics under the default, even
though a clean-slate design would put it on pre. This is intentional: the
WP1 §1.3 determinism grid's serial cells (1 and 2) are a fixed baseline, and
holding them byte-identical is worth more than the local correctness of a
non-default driver mode. An explicit `--consumption-signal tid0` with
`--driver-threads 1` does select pre for that path, which is correct.

**Also settled here.** `--protect-current`'s one-step lookback (forcing
`seq[pos-1]` to distance 0) is redundant on the pre-consumption path once
Defect 2 lands: `seq[pos]` is already 0 by construction, and `seq[pos-1]` is the
*previous* layer, whose next use is a full lap away — protecting it is
counter-productive. Phase 3 measures the real model with `--protect-current off`;
whether the `residctl_llama.c` default flips is settled in Phase 4 / spec
amendment A-14 on that data, not pre-emptively.
