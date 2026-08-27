# WP2 — llama.cpp integration · box: 6h

The single biggest gap in the project. Every finding to date is validated only
against a synthetic cyclic-scan replay driver. This work package runs a real
model through the pager.

**Ambition is capped deliberately.** The goal is a working end-to-end
demonstration with measurements, not a production integration. If forced to
choose between more features and a working measurement, take the measurement.

---

## PHASE 2.0 — Model and build · box: 60 min · GATE

### Model

Expect a GGUF at `/root/residctl/models/model.gguf`, placed there by the human
before this session.

**If it is absent: write that to `results/overnight/BLOCKERS.md` and skip the
entire work package.** Do not attempt to download one — the sandbox's allowed
domains do not include model hosts, and time spent discovering that is wasted.
Move to WP3.

If present, record: size, `sha256`, and the tensor inventory (name, offset,
size, count) parsed from the GGUF header. Write the inventory to
`results/overnight/wp2_tensor_inventory.txt`. This is needed for the chunk table
and is worth having regardless of what follows.

### Build

`llama.cpp` is already cloned at `/root/spike/src/llama.cpp` (commit
`69bf6437914596fbbc4caf09a7ac16f2acdd1a94`). **Copy it to
`/root/residctl/third_party/llama.cpp`** — do not modify anything under
`/root/spike/`.

Build CPU-only. Do not build CUDA: 6 GiB of VRAM with ~2 GiB already resident
adds a variable this work package does not need, and partial offload would make
the weight access pattern harder to reason about.

Confirm the unmodified binary runs the model and produces tokens. Record
tokens/s and peak RSS. **This is the reference point for everything after.**

**GATE:** if the unmodified build does not run the model, stop the work package,
record the failure, and move to WP3. Do not debug llama.cpp for more than 30
minutes.

---

## PHASE 2.1 — Chunk table from real tensors · box: 60 min

The synthetic driver used uniform chunks. A real GGUF does not have uniform
tensors.

Build the chunk table from the tensor inventory:

- **One chunk per transformer layer**, grouping that layer's tensors by their
  `blk.N.` name prefix. Non-layer tensors (embeddings, output norm, output head)
  each become their own chunk.
- Chunk boundaries must be **4096-aligned**, per `MECHANISM_SPEC.md` §6.1.
  Absorb slack into the preceding chunk, as the spec requires. This is the case
  §6.1 was written for and has never been exercised — the synthetic table always
  produced `file_off == region_off`, both naturally aligned.
- Chunks will now have **unequal sizes**. Verify every place that assumed
  uniformity: budget accounting, `ensure_budget`, the FIFO cap in retention, the
  Belady solver's byte computation. **Report every site you had to change.**

Report the resulting table: chunk count, size distribution (min/median/max), and
total region size against the file size.

### Pre-registered check

The Belady solver's optimal-bytes computation must handle unequal chunk sizes.
Its miss *count* bound is unchanged, but bytes are now `sum of missed chunk
sizes`, not `misses × chunk_size`. **Verify this explicitly and report it** — an
unchecked assumption here would reproduce the original item 10 OPT defect in a
new form.

---

## PHASE 2.2 — Wire the pager into the loader · box: 90 min

From the spike's S4 findings (`/root/spike/results/s4_findings.md`):

- `ggml_backend_tensor_alloc` (`ggml/src/ggml-backend.cpp:2052`) accepts an
  arbitrary caller pointer and only asserts it falls within the buffer's declared
  range. This is the substitution point.
- Call sites are concentrated in `src/llama-model-loader.cpp` (lines 1396,
  1549/1559).
- The mmap itself is at `src/llama-mmap.cpp:457`, constructed from
  `src/llama-model-loader.cpp:1356`.

**Approach, pre-decided — do not evaluate alternatives:**

Add a load mode that, instead of mmapping the model file, creates a `residctl`
region over it and hands tensor pointers into `map_a`. Tensor *i*'s pointer is
`map_a + (tensor_file_offset - region_base_offset)`. The pager services faults
on that region exactly as it does for the replay driver.

- Gate it behind a new `--load-mode residctl` (or an equivalent flag), leaving
  every existing mode untouched.
- The declared access sequence (WP1) is the layer order: chunk 0, 1, 2, ..., n-1,
  repeating per token. Declare it at load time.
- `pager_notify_access()` is called from the eval callback
  (`ggml_backend_sched_eval_callback`, typedef at
  `ggml/include/ggml-backend.h:314`). It fires **per graph node, not per layer** —
  map node to layer via the tensor's `blk.N.` name prefix and fire
  `pager_notify_access` once per layer transition, not once per node.

**Correctness gate — STOP-AND-REPORT:** generate 32 tokens with a fixed seed and
a fixed prompt under `--load-mode mmap`, then again under `--load-mode residctl`
with a budget large enough to hold the whole model. **The output token sequences
must be identical.** If they differ, the pager is serving wrong data and no
measurement matters. Report the divergence and stop this work package.

---

## PHASE 2.3 — Measurement · box: 90 min

Same arm structure as the synthetic sweeps, adapted.

**Arms:**
- **A** — unmodified `--load-mode mmap`, under the same cgroup budget.
- **C** — `residctl` with `lru`.
- **D** — `residctl` with `layer_order_declared`, prefetch off.
- **E** — `residctl` with `layer_order_declared`, prefetch on, depth 2, retention pinned.
- **OPT** — offline solver over the declared sequence, with unequal chunk sizes.

**Budget ratios:** {0.25, 0.5, 0.75} of the model's weight-region size.

**Workload:** fixed prompt, generate **64 tokens**, fixed seed. n=3.

`memory.max` identical across arms at each ratio. `memory.swap.max = 0` (I-3).
`drop_caches` before every arm A run, with the Campaign 12 Phase A guard active.

**Report per cell:** `read_bytes` from `/proc/PID/io`, total fetches, demand
faults, tokens/s, time-to-first-token, p99 inter-token latency, wall-clock,
`memory.peak`, `pin_broken`, `infeasible`, and for arms A the achieved bandwidth
with a flag if it exceeds 3396 MiB/s.

### Pre-registered expectations

1. Arm C (lru) misses at or near 100%, as it does synthetically — the real access
   pattern is also a repeating layer scan.
2. Arm D reads fewer bytes than arm A at every ratio.
3. Arm E beats arm D on bytes — real inference has a genuine compute phase, which
   the synthetic driver had to simulate with `--compute-ns-per-mib`. This is the
   first test of prefetch against real compute rather than a busy loop.
4. OPT ≤ D at every ratio.
5. Tokens/s degrades gracefully as the budget tightens rather than collapsing.

Expectation 3 is the one this work package exists for. Report it prominently
either way.

---

## PHASE 2.4 — Compare synthetic to real · box: 30 min · analysis only

The synthetic driver was a model of this workload. Now both exist. Report:

- Chunk count and size distribution: synthetic uniform vs real GGUF.
- Measured per-layer compute time in real inference, against the
  `--compute-ns-per-mib` settings {0, 400000} the synthetic sweeps used. **Which
  synthetic setting was closest to reality?**
- Arm D's byte reduction over arm A: synthetic vs real, at matching ratios.
- Whether the E-vs-D ordering found synthetically holds under real inference.

State plainly which synthetic findings the real workload confirms, which it
contradicts, and which it cannot address.

---

## Report

`results/overnight/wp2_llamacpp.md`. Standard structure. The verdict must state:
did the correctness gate pass, did arm D beat arm A on bytes, and did arm E beat
arm D.

Also record, in `results/overnight/BLOCKERS.md` if applicable, any phase that hit
its time-box or a gate.