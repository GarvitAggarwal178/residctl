# WP2 — llama.cpp integration

## Verdict

- **Correctness gate: PASS.** 32 tokens, fixed prompt, greedy, budget large
  enough to hold the whole model: `--load-mode mmap` and `--load-mode
  residctl` produce **byte-identical token sequences**. The pager serves
  correct weight data through userfaultfd for a real transformer.
- **Did arm D (declared order) beat arm A (kernel mmap) on bytes?**
  **Yes at budget ratios 0.5 and 0.75** — D reads 24% and 51% fewer bytes
  than the kernel-managed baseline. **No at ratio 0.25** — D (126 GB) and A
  (117 GB) are roughly tied (the kernel keeps `memory.max`, 782 MiB,
  resident, which exceeds D's self-imposed 526 MiB budget).
- **Did arm E (declared + prefetch) beat arm D on bytes?** **No, at any
  ratio** — E reads 7–9% *more* bytes than D. It trades bytes for latency:
  ~2× fewer demand faults, higher tokens/s, lower TTFT. **Arm E collapsed
  at ratio 0.25** (both reps hit the 360 s timeout without finishing 64
  tokens).
- **D vs the offline optimum: D/OPT = 1.09–1.14 at every ratio.** On the
  real workload, application-authoritative residency lands within 15% of
  Belady.

**The single largest structural gap in the project is now closed: every
prior finding was synthetic-only; the core thesis (kernel LRU thrashes on a
cyclic layer scan; app-authoritative residency reads far less and stays
near optimal) holds on a real 3B model.**

**This work package also forced the WP0 consumption-signal fix** (commit
`8c15d8b`, BLOCKER 2): the first WP2 sweep showed `layer_order_declared`
deterministically reading 216.6 GB at ratio 0.25 — 1.5× arm C — because
`lo_declared_dist()` ranked the *actively-consumed* chunk as furthest-future
and therefore the top eviction target. All numbers in this report are
post-fix.

---

## Machine exclusivity

Checked before/after the sweep (`uptime`, `ps --sort=-%cpu`). Clean — no
foreign workload; load average 0.5–1.8 (own `wp2_gen` load) throughout.

---

## PHASE 2.0 — Model and build

**Model** (`models/model.gguf`): `Qwen2.5-3B-Instruct-GGUF`, `q4_k_m`,
downloaded via `curl` from Hugging Face (WP0 §0.1; the WP2.md "cannot
download" gate is corrected by the session-2 prompt).
- size: 2,104,932,768 bytes (1.96 GiB — in the 1.5–3 GiB target)
- sha256: `626b4a6678b86442240e33df819e00132d3ba7dddfe1cdc4fbb18e0a9615c62d`
- 435 tensors, 36 transformer layers, GGUF v3, `general.alignment` 32.

Tensor inventory: `experiments/logs/overnight__wp2_tensor_inventory.txt`.

**Build:** `llama.cpp` copied to `third_party/llama.cpp` (gitignored;
`scripts/setup-llama-cpp.sh` + `src/wp2_llama_mmap.patch` reconstruct it),
built CPU-only (`GGML_CUDA=OFF GGML_NATIVE=ON`).

**GATE:** the unmodified `llama-bench` runs the model — `qwen2 3B Q4_K_M`,
3.40 B params, CPU, 8 threads: **87.9 tokens/s prompt, 13.2 tokens/s
generation** (this is the unconstrained reference for "graceful
degradation"). Peak RSS ~2.1 GiB (the whole model, mmap'd).
(`llama-cli` hangs on this build — an unrelated interactive-mode issue; not
used. `wp2_gen`, based on `examples/simple`, drives generation.)

---

## PHASE 2.1 — Chunk table from real tensors

`src/residctl_llama.c` parses the GGUF via the ggml `gguf_*` API and builds
the chunk table.

**Table:** 41 chunks — 1 GGUF header + 1 `token_embd` (175 MiB) + 1
`output` (243 MiB) + 36 per-layer chunks + a small `output_norm`/tail
chunk.
- size distribution: min 0.01 MiB, **median 41.5 MiB**, max 243.4 MiB.
- total region 2,104,934,400 bytes = `align_up(file_size, 4096)` (1.0000×
  the file).

**Finding — the real GGUF is not laid out in layer order.** Tensors are
stored in *name-lexicographic* order (`blk.0`, `blk.1`, `blk.10`,
`blk.11`, …, `blk.19`, `blk.2`, `blk.20`, …), so chunk index ≠ layer
order. **Layer 21 is split across two non-contiguous file runs** (chunks 17
and 25). Consequences:
- The declared access sequence given to the policy is the real per-token
  consumption order — `[token_embd, L0, L1, …, L35, output_norm, output]`
  (40 entries) — **not** the naive `0, 1, …, n-1` WP2.md assumed (that
  assumption held for the synthetic uniform table).
- `residctl_llama_notify_layer(L)` fires the consumption signal for *all*
  of layer L's chunks.

### Sites that had to change for unequal chunk sizes — reported per WP2.md

| Site | Change | Why |
|---|---|---|
| `region.h` / `region.c` `build_chunk_table_explicit()` | **new** — accepts an explicit non-uniform spec array (`residctl_chunk_spec_t`), validates it tiles `[0, region_len)` contiguously, 4096-aligned. | the synthetic `build_chunk_table` only makes uniform chunks. |
| `fetch.c` `fetch_read()` | the last chunk's 4096-aligned end runs past EOF (a real GGUF isn't a 4096 multiple). Split the read: O_DIRECT for the aligned bulk, one **buffered** `pread` for the sub-4096 tail (O_DIRECT rejects unaligned partial reads), zero-fill `[file_size, aligned_end)`. Added `region_t.model_file_size` + `model_fd_buf`. **This is §6.1's slack-absorption case, exercised for the first time.** Guarded by `model_file_size != 0` so the replay path is byte-for-byte unchanged. | §6.1 anticipated it; the synthetic table always had `file_off == region_off`, both aligned. |
| `budget.c` (`ensure_budget`, `evict_chunk`) | **no change needed** — already fully byte-based (`c->len`, `resident_bytes`, `budget_bytes`). | verified: the eviction loop is `while (resident + reserved + need > budget)`. |
| retention FIFO (`pinned_prefetch_queue`) | **no change needed** — its cap is `prefetch_depth` (a count), size-agnostic. `pin_break_select_victim` uses `next_use_distance`, byte-agnostic. | — |
| `belady_main` / `belady.c` | **not touched.** New standalone `src/wp2_opt.c` computes OPT with unequal chunk sizes. | rule 9 / WP1 precedent (write it separately, cross-check). |
| `policy.c` `lo_declared_dist()` | WP0 fix — protect the actively-consumed chunk. | see Verdict / BLOCKER 2. |

### Pre-registered check — OPT bytes for unequal chunks

`wp2_opt.c` runs greedy Belady/MIN over the declared sequence with a
**byte** budget: on a miss, `missed_bytes += chunk_size[c]` (**not
`misses × chunk_size`**), then evict the furthest-future resident until `c`
fits. **Verified explicitly:** `opt_missed_bytes` for the three ratios
(110.6 / 72.6 / 38.3 GB) each exceed `sum(distinct chunk sizes)` = 2.00 GiB
(the compulsory-bytes floor) and `opt_misses` ≥ 40 (compulsory misses). An
equal-size cross-check path asserts `missed_bytes == misses × len` when all
chunks are equal (not the case here). No circular-input / below-floor
failure — the original item-10 OPT defect is not reproduced in this new
form.

---

## PHASE 2.2 — Wire the pager into the loader

**Approach (pre-decided):** a load mode that replaces the model mmap with a
`residctl` region and hands tensor pointers into `map_a`.

- `third_party/llama.cpp/src/llama-mmap.cpp` patched (~35 lines,
  `src/wp2_llama_mmap.patch`): when `RESIDCTL_CONFIG` is set, resolve
  `residctl_llama_mmap` via `dlsym(RTLD_DEFAULT, …)` (no link-time
  dependency) and use it instead of `mmap()`. `unmap_fragment()` and the
  destructor become no-ops in that mode (the pager owns the region).
  Nothing else in llama.cpp changed; every existing load mode is untouched.
- Tensor *i*'s pointer is `map_a + tensor_file_offset` (S4's substitution
  point `ggml_backend_tensor_alloc` accepts it unchanged, since the region
  maps the file 1:1).
- Declared sequence declared at load time (Phase 2.1).
- `pager_notify_access()` fired from the eval callback
  (`cb_eval`, `wp2_gen.cpp`): per graph node, mapped to layer via the
  node-name suffix `-<il>`, fired once per **layer transition** (plus the
  `inp_embd` / `result_norm` / `result_output` nodes for the non-layer
  chunks).

**CORRECTNESS GATE: PASS** (`experiments/logs/overnight__wp2_gate_log.txt`). 32
tokens, `RESIDCTL_CONFIG` on vs off:
```
mmap:     13 2585 1657 4244 525 1052 304 279 11652 30 2014 8253 279 1372 315 4244 304 279 11652 330 785 3974 13876 38835 34208 916 279 15678 5562 323 1221 13598
residctl: 13 2585 1657 4244 525 1052 304 279 11652 30 2014 8253 279 1372 315 4244 304 279 11652 330 785 3974 13876 38835 34208 916 279 15678 5562 323 1221 13598
```
Identical. Re-verified after the WP0 fix — still identical.

---

## PHASE 2.3 — Measurement

`scripts/run-real-model-arms-sweep.sh`. Arms A (mmap), C (`lru`), D
(`layer_order_declared`, prefetch off), E (`layer_order_declared`, prefetch
on, depth 2, retention pinned). Budget ratios {0.25, 0.5, 0.75} of the
2.10 GB weight region. 64 tokens, fixed prompt, greedy, **n=2** (reduced
from 3 to fit the time box — arms C and D are perfectly deterministic;
arm A varies ~10%). `memory.max` = budget + 256 MiB, identical across arms
per ratio. `memory.swap.max = 0`. `drop_caches` before every arm A run.
Per-run timeout 360 s.

`read_bytes`: arm A = `/proc/self/io` `read_bytes` delta over generation;
arms C/D/E = `pager_bytes_fetched`. OPT from `wp2_opt` over the declared
sequence at each budget, 65 passes (measured layer-scan count incl. prompt).

| ratio | arm | read (GB) | /OPT | demand faults | tokens/s | TTFT (ms) | p99 inter-tok (ms) | wall (s) | mem_peak (MiB) |
|---|---|---|---|---|---|---|---|---|---|
| 0.25 | A | 116.9 | 1.06† | — | 0.82 | 1295 | 1321 | 78.4 | (mmap) |
| 0.25 | C | 144.4 | 1.31 | 2817 | 0.97 | 1348 | 1160 | 66.3 | 588 |
| 0.25 | D | 126.1 | **1.14** | 2499 | 1.13 | 1188 | 924 | 56.8 | 581 |
| 0.25 | E | **COLLAPSED** (360 s timeout, both reps) | | | | | | | |
| 0.5 | A | 104.7 | 1.44 | — | 0.90 | 1258 | 1201 | 70.8 | (mmap) |
| 0.5 | C | 134.5 | 1.85 | 2561 | 1.00 | 1463 | 1081 | 63.7 | 1089 |
| 0.5 | D | 79.3 | **1.09** | 1614 | 1.66 | 1229 | 632 | 38.6 | 1089 |
| 0.5 | E | 86.4 | 1.19 | 756 | 1.82 | 939 | 653 | 35.1 | 1089 |
| 0.75 | A | 88.0 | 2.30 | — | 1.03 | 857 | 1381 | 68.8 | (mmap) |
| 0.75 | C | 134.5 | 3.51 | 2561 | 0.99 | 1615 | 1123 | 64.8 | 1591 |
| 0.75 | D | 43.4 | **1.13** | 871 | 2.77 | 1301 | 450 | 23.1 | 1585 |
| 0.75 | E | 46.2 | 1.21 | 430 | 3.01 | 957 | 394 | 21.3 | 1591 |

† arm A/OPT < 1.06 at r=0.25 is not "beating optimal" — the kernel's
effective cache (up to `memory.max`, 782 MiB) exceeds D's 526 MiB budget,
so A faces a looser constraint than OPT-at-D's-budget. A's `read_bytes`
also varied run to run (110–124 GB at r=0.25; 58–118 GB at r=0.75) — the
kernel's reclaim behaviour under `memory.max` pressure is noisy.

`infeasible = 0` and `pin_broken = 0` on every completed run. Arm A shows
no host-cache bandwidth flag (achieved bandwidth ~1.4 GB/s, well under the
3396 MiB/s O_DIRECT ceiling — the workload is fault-stall-bound, not
bandwidth-bound).

### Pre-registered expectations

1. **Arm C misses at/near 100%: HELD, emphatically.** C faults 2561–2817
   references out of ~2600 total (≈0.99–1.08 — *above* 1.0 because a chunk
   evicted mid-pass is re-faulted) at **every** ratio, **including 0.75**.
   Reads the whole model (134–144 GB) every run, ~1 tokens/s regardless of
   budget. The kernel's own LRU on a cyclic layer scan degenerates exactly
   as it did synthetically.

2. **Arm D reads fewer bytes than arm A: HELD at r=0.5 (−24%) and r=0.75
   (−51%); DID NOT HOLD at r=0.25** (D 126 GB vs A 117 GB — roughly tied).
   At r=0.25 the mismatch between D's self-imposed 526 MiB budget and the
   kernel's ~700 MiB effective working set explains it. As budget loosens,
   D pulls decisively ahead.

3. **Arm E beats arm D on bytes: DID NOT HOLD at any ratio.** E reads
   7–9% *more* bytes than D. **But** E cuts demand faults ~2× (1614→756 at
   r=0.5; 871→430 at r=0.75), raises tokens/s (1.66→1.82; 2.77→3.01), and
   roughly halves p99 inter-token latency. Prefetch on a real compute phase
   trades total volume for latency — the same direction as the synthetic
   findings (Campaign 11 Phase 2). **This is the expectation the work
   package existed for; reported prominently: prefetch helped latency, not
   bytes.**

4. **OPT ≤ D at every ratio: HELD.** D/OPT = 1.14 / 1.09 / 1.13.
   Application-authoritative residency on a real model lands within 15% of
   the offline optimum — the synthetic "D is 1.07–1.78× OPT" result
   *improves* on real inference (real per-layer compute is light, so the
   concurrency-timing effects that hurt D synthetically barely bite).

5. **Tokens/s degrades gracefully rather than collapsing: PARTIALLY.**
   All arms degrade hard from the 13.2 t/s baseline. But D and E scale
   *with* budget (D: 1.1 → 1.7 → 2.8 t/s) while A and C stay pinned near
   1 t/s at every budget — the kernel cannot turn extra budget into
   throughput on this access pattern; the app-authoritative policy can.
   Against that, **arm E's outright collapse at r=0.25** is the opposite of
   graceful (see below).

### Arm E's collapse at ratio 0.25

Both reps hit the 360 s timeout without finishing 64 tokens (arm D finished
the same cell in 57 s). Mechanism: at the tightest budget, `--prefetch-depth
2` + `--prefetch-retention pinned` keeps ~2 chunks (up to ~180 MiB, ~34% of
the 526 MiB budget) pinned, and the WP0 fix additionally protects the
2 actively-consumed chunks. Demand fetches then repeatedly find no evictable
victim, spin in `handle_absent()`'s bounded `ensure_budget` retry loop
(200 × 2 ms), and generation crawls. This is a real interaction — prefetch
retention and current-chunk protection are each individually sound but
together over-constrain a very tight budget. Recorded in
`/root/residctl-archive/session-summaries/overnight-blockers.md`; not chased further (WP2.md caps ambition).

---

## PHASE 2.4 — Synthetic vs real

**Chunk count and size distribution.**
| | synthetic (Campaign 12 Phase D) | real GGUF |
|---|---|---|
| chunks | 16 (128 MiB) or 256 (8 MiB), **uniform** | 41, **min 0.01 / median 41.5 / max 243.4 MiB** |
| layout | `file_off == region_off`, both 4096-aligned, §6.1 slack always 0 | slack non-zero at the final chunk; one layer (21) split across 2 non-contiguous chunks; tensors in name-lex not layer order |
| declared sequence | `0, 1, …, n-1` | the real per-token order `[embd, L0..L35, out_norm, output]` |

**Measured per-layer compute time.** Unconstrained baseline: 13.2 tokens/s
= 75.8 ms/token / 36 layers ≈ **2.1 ms per layer**. Each layer chunk is
~41.5 MiB, so real inference compute ≈ **~51,000 ns/MiB**. The synthetic
sweeps used `--compute-ns-per-mib` ∈ {0, 400000} (achieved ~1.5 M ns/MiB
for the "400000" setting, per Campaign 11 Phase 2's disclosed calibration
overshoot). **Neither synthetic setting is close: real compute is ~1/30 of
the synthetic "heavy" setting and well above "0".** The synthetic sweeps
bracketed reality but did not hit it; the truth is much nearer the
zero-compute end.

**Arm D byte reduction over arm A: synthetic vs real.**
| | synthetic (D/A on bytes, clean cells) | real (D/A on bytes) |
|---|---|---|
| r=0.25 | D < A (Campaign 13 Phase C: 8/8 clean) | D ≈ A (tied) |
| r=0.5 | D < A | D = 0.76 × A (−24%) |
| r=0.75 | D < A | D = 0.49 × A (−51%) |
The direction holds on the real workload at r ≥ 0.5; at r=0.25 the
budget/`memory.max` mismatch (arm A gets the slack, arm D self-limits)
makes it a wash.

**E-vs-D ordering.** Synthetic: E beat D on bytes only under heavy compute
(`--compute-ns-per-mib 400000`), at r=0.5 and r=0.75. Real: **E never beats
D on bytes** — because real compute (~51k ns/MiB) is far lighter than the
synthetic "heavy" setting where E won. E *does* win on latency/faults,
which the synthetic byte-only comparison never captured. The synthetic
"E beats D under heavy compute" finding **does not transfer** to this
model — its compute phase is too light.

### What the real workload confirms / contradicts / cannot address

**Confirms:**
- Kernel LRU degenerates to full-pass thrashing on a cyclic reference
  string (arm C, every ratio, including 0.75).
- Application-authoritative residency reads far less than the kernel and
  scales with budget (arm D at r ≥ 0.5).
- The offline optimal is computable and D is measurably close to it
  (D/OPT ≈ 1.1) — in fact *closer* than synthetically.

**Contradicts / does not transfer:**
- "Prefetch (arm E) beats arm D on bytes under a real compute phase" — the
  synthetic heavy-compute regime (`--compute-ns-per-mib 400000`) is ~30×
  heavier than this real model's per-layer compute; E does not beat D here.
- "Tokens/s degrades gracefully" — real inference under a tight budget
  drops 4–13×, and arm E collapses outright at r=0.25.
- The synthetic uniform chunk table hid the real GGUF's name-lex tensor
  order and split layers.

**Cannot address:**
- Larger models (this is 3B / 2 GiB); MoE routing (dense model);
  GPU/partial offload (CPU-only per WP2.md); the host-cache confound (arm A
  here is fault-stall-bound, not bandwidth-bound, so the 3396 MiB/s ceiling
  never engaged).

---

## What was not tested

- Ratios outside {0.25, 0.5, 0.75}; n=3 (used n=2 for the time box);
  compute levels (real inference has one, fixed, compute phase).
- Arm E at r=0.25 to completion (collapsed; not re-run).
- `--sync-handler`, `--fetch-workers` other than 4, `--driver-threads`
  (llama.cpp's own 8 compute threads are the "drivers"; `-t` fixed at 8).
- The driver-side alternative for the consumption signal ("notify =
  finished") — the policy-side fix was chosen (BLOCKER 2).
- Whether a larger `--prefetch-depth` or `--prefetch-retention none` avoids
  arm E's r=0.25 collapse.

## Final check

- Every number is a direct read from `results/data/real-model-arms.csv`
  (median of n=2), `results/data/real-model-arms-opt-bound.csv` (`wp2_opt` over the declared sequence),
  `wp2_gate_log.txt`, or `wp2_tensor_inventory.txt`. Nothing estimated
  except the per-layer compute time in Phase 2.4 (derived from the
  llama-bench baseline, method shown).
- The correctness gate was an exact token-sequence match, re-run after the
  WP0 fix.
- The 5 pre-registered expectations are each reported held / did not hold /
  partial with numbers; the failures (2 at r=0.25, 3, 5) are reported with
  the mechanism.
- `results/data/real-model-arms-before-wp0-fix.csv` retains the pre-WP0-fix data (arm D 217 GB at
  r=0.25) that motivated the fix.
- No test was weakened; residctl T-1..T-7 pass after every code change this
  session.
