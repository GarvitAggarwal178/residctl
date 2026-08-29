# LIVELOCK FIX — Phase 1: the three defects (+ signal mode)

Applied on top of Phase 0b (4th fix + audit). Machine exclusive for the gate
runs (load ~0.1, own runs only).

---

## Defect 1 — `lo_declared_dist()` origin is now signal-mode-aware

The user's original Defect 1 (unconditional `d` starts at 0) was **wrong for
the post-consumption path** and is not what was implemented. The correct origin
depends on when `pager_notify_access()` fires relative to the read it announces:

| mode | when notify fires | `d` starts at | `dist(seq[pos])` |
|---|---|---|---|
| **post** (`policy_set_signal_mode(0)`, default) | after `seq[pos]` fully read | 1 | `seq_len` (furthest) |
| **pre** (`policy_set_signal_mode(1)`) | before `seq[pos]` read | 0 | 0 (imminent) |

`policy.c:lo_declared_dist()` now scans `for k in 0..seq_len-1: d = d0 + k`
with `d0 = signal_mode ? 0 : 1`. **`d0 == 1` is byte-for-byte the old
`for d = 1..seq_len` loop** — post mode is a no-op refactor. Chunks absent from
the declared sequence still fall through to `INT64_MAX` in both modes.
`--protect-current` is unchanged and orthogonal (forces `seq[pos]` / `seq[pos-1]`
to 0 regardless of mode).

**Wiring:**
- `replay_main.c`: `policy_set_signal_mode(consumption_signal_all ? 0 : 1)` —
  all-threads (the default) → post; tid0 → pre. The serial `replay_cyclic`
  path ignores `consumption_signal_all` and thus stays **post** under the
  default, a deliberate baseline-continuity choice (`docs/design-history.md`).
- `residctl_llama.c`: `policy_set_signal_mode(1)` — the real model, because
  Defect 2 moves the notify to the pre-compute callback pass.

`signal_mode=pre|post` is now emitted in `ARM_CSV` and `RESIDCTL_STATS`.

## Defect 2 — `wp2_gen.cpp:eval_cb()` acts on the pre-compute pass

`if (ask) return true;` → `if (!ask) return true;`. The eval callback fires
`ask=true` before a node computes and `ask=false` after; the body (which fires
`residctl_llama_notify_layer` / `notify_role`) now runs on the **pre** pass, so
the consumption signal precedes the weight read. Combined with the 4th fix,
`token_embd` too is signalled pre-read. Token-identity is checked in Phase 3's
correctness gate.

## Defect 3 — declined-prefetch backoff

`chunk_t.decline_until_ns` (new). `prefetch_pool.c:do_one_prefetch()` sets it to
`now + 100 ms` on the `ensure_budget_prefetch() != 0` path (covers both
`stat_prefetch_declined` and `stat_prefetch_infeasible` — the spec equates
"decline" with "returns −1"). `prefetch_pool_top_up()` skips any candidate whose
`decline_until_ns` is still in the future, next to the existing
`pf_tracked` / `CHUNK_ABSENT` checks. No growing backoff, no counter, no new
structure. Expectation 3 in Phase 3 is stated in terms of
`stat_prefetch_declined` alone; the backoff also suppresses re-tries after a
true infeasible, so attribute the drop to both mechanisms.

---

## Unit tests (`test_policy.c`) — PASS

```
declared next-use vs naive Belady scan (post, protect-on): agree (all positions, all chunks)
declared next-use vs naive Belady scan (pre, protect-off):  agree (all positions, all chunks)
layer_order_declared post protect-off:      ok
layer_order_declared pre protect-off:       ok
layer_order_declared protect-on both modes: ok
PASS
```

- **post + protect-off** (unchanged): `dist(seq[pos]) == seq_len`,
  `dist(seq[pos-1]) == seq_len-1`, `dist(seq[pos+1]) == 1`.
- **pre + protect-off** (new): `dist(seq[pos]) == 0`, `dist(seq[pos+1]) == 1`,
  `dist(seq[pos-1]) == seq_len-1`, `dist(chunk absent from seq) == INT64_MAX`,
  `select_victim` picks the furthest.
- **protect-on, both modes** (new): `seq[pos]` and `seq[pos-1]` both 0;
  absent chunk still `INT64_MAX`.
- **Belady cross-check, pre mode** (new): independent naive scan with `k >= 0`
  agrees at every position, every chunk.
- The **existing post-mode cross-check was insensitive to the `d0` change** —
  `protect-on` short-circuits `seq[pos]` / `seq[pos-1]` to 0, and those are the
  only inputs where starting at 0 vs 1 differs. It passes byte-identical; the
  new pre-mode cross-check is what actually exercises the corrected origin.

## Regression gates — PASS

| gate | result |
|---|---|
| WP1 §1.2 (`run_wp1_gate.sh`, `layer_order_learned`) | `absent_handled=57 evictions=49 pager_bytes_fetched=7650410496`, all 3 reps — **exact**, GATE PASS |
| T-1..T-5 (`run_correctness_harness.sh`) | RESULT: PASS |
| T-6, T-7 (`run_t6_t7.sh`) | `mismatches=0`, all storm threads joined, `dedup_fetching=16161 > 0` — RESULT: PASS |
| `test_prefetch`, `test_eviction` | PASS (Defect 3 path) |
| Phase 0 diagnostic (Defect 2 pre-compute timing) | 320 notifies / 8 tokens, all 40 declared chunks in declared order, cursor 7 clean wraps / 0 jumps, audit OK — GATE PASS |

(T-1..T-7 run with `reconcile_interval = 1` compiled into the test binaries =
`--eager-reconcile`.)

## Files touched

`src/policy.{c,h}` (Defect 1 + signal mode), `src/wp2_gen.cpp` (Defect 2),
`src/region.h` + `src/prefetch_pool.c` (Defect 3), `src/replay_main.c` +
`src/residctl_llama.c` (signal-mode wiring + `signal_mode` in the stats lines),
`src/test_policy.c` (both-mode tests), `docs/design-history.md` (new).
