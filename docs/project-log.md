# Project log

The narrative version of how this project unfolded — what each stretch of work
set out to do, what it found, and where it was wrong. The per-experiment detail
is in [`experiments/`](../experiments/); the design decisions that came out of
it are in [`design-history.md`](design-history.md); the numbers that got
retracted are in [`superseded.md`](../results/superseded.md).

`CLAUDE.md` at the repo root is the agent working file — this is the readable
version of the same history. Session prompt files and per-session wrap-up
summaries were archived out of the repo (`/root/residctl-archive/`, not on
GitHub); their substance is here.

---

## Feasibility spike

Established, on real hardware, that the primitives compose: `userfaultfd` with
`UFFDIO_CONTINUE` over a double-mapped `memfd`, `FALLOC_FL_PUNCH_HOLE` for
eviction, cgroup v2 `memory.swap.max = 0` so the kernel can't undo an eviction
by swapping. Measured the costs that shape the design (a 150 MiB `O_DIRECT` read
is 61 ms; `CONTINUE` is 2.9 ms — not a cost centre). Hung 5/5 runs before
diagnosing that `poll()` readiness on the uffd is advisory and only `EAGAIN`
means "queue empty" (invariant I-2). Report and constants live in
`/root/spike/results/`.

## Items 1–10 — building the mechanism

Region setup, the handler state machine, the aligned fetch path, eviction +
budget + reconcile, the trace recorder, the replay driver, the `lru` and
`layer_order` policies, prefetch, the Belady solver, and the six-arm harness —
built in the spec's fixed order.

Two things went wrong and were caught:

- **The Belady sanity check in the spec was itself wrong** (item 9). The spec
  asked the solver to return "approximately `(1−r)·W` bytes per pass" on a
  cyclic string; that under-counts, because demand paging forces a cache slot
  open on every miss. Replaced with the exact provable floor `n + (passes−1)·
  max(n−k, 0)` — amendment A-2.
- **The item 10 harness (V1) had three defects** and its results were voided.
  The offline solver was being fed the *handler's fault trace* — a record of
  which references this policy missed on, which makes the OPT bound circular
  (it returned values *below* the provable floor). Arms A/B touched one byte
  per chunk while the pager arms read every byte, so arm A "read" ~250× less.
  And the primary metric was fault count, not `read_bytes`. Amendments A-1, A-4,
  and the metric change; V2 is the first valid sweep (experiment
  [`03`](../experiments/03-corrected-harness.md)).

## Item 10b–10e — the I/O pipelining thread

A sequence of diagnostics chasing why prefetch hit rate sat at 14–27 %, flat
across depth.

- **10b** found arm A's throughput exceeded the spike's own `O_DIRECT` ceiling —
  the first sighting of the WSL2 VHDX host-cache confound — and three latent
  concurrency bugs in the fetch path.
- **10c** made the handler dispatch-only with a shared fetch-worker pool
  (amendment A-5), after proving the synchronous design made the `FETCHING`
  dedup branch *structurally unreachable*. Added prefetch admission (A-6) —
  which then declined 0/36 cells, a recorded negative result: `layer_order`'s
  victim selector already picks the coldest chunk.
- **10d** made the replay driver multi-threaded (A-8) and added prefetch
  retention (A-9) — a prefetched chunk stays pinned until consumed or aged out
  of a bounded FIFO.
- **10e** replaced 10d's hard barrier with a bounded lookahead window (A-10),
  because the barrier made the async handler's whole reason for existing
  untestable by construction. Found retention's byte effect *reverses* under
  real cross-chunk overlap (A-11) — `stat_pin_broken` is the diagnostic for
  which regime a run is in.

## Campaign 11 — closing session, measure only

Five phases, no decisions. Platform microbenchmark: raw `O_DIRECT` concurrency
plateaus at a ~3400–4300 MiB/s ceiling past two threads; `fd-per-thread` doesn't
lift it. Added a calibrated compute phase to the driver and found prefetch hit
rate *fell* with more compute — opposite the hypothesis — and arm E beat arm D
on bytes at r ≥ 0.5 under a heavy compute phase, the first time in the project.
Chunk-size sweep {32…256 MiB}: arm D bytes and wall-clock both climb with chunk
size.

## Campaign 12 — the drop_caches bug, and the chunk-size floor

**Phase A found Campaign 11's arm-A `read_bytes = 0` was a guest-side bug**, not
host-cache contamination — both sweep scripts' `drop_caches()` redirected `echo
3`'s output to a log file instead of the kernel interface (`2>&1 >> "$LOG"`
ordering). Every D-vs-A byte comparison in Campaign 11 Phase 3/4 was against a
baseline that did no measured I/O. Re-ran with a repaired script and a
startup-abort guard in `baseline_main`. Phase B extended the chunk-size sweep
below 32 MiB and found wall-clock and bytes *diverge* there (wall-clock favours
larger chunks). Phase D produced the consolidated 6-arm × 5-ratio × 2-compute
paper table.

## Campaign 13 — determinism, and the metric artefact

**Phase A** isolated Campaign 12's "arm D degenerates to arm C" cells to a
precise mechanism: `layer_order`'s successor chain is built from
*fault-dispatch order*, which is timing-dependent only when driver threads,
lookahead, and compute are all present at once. Added `--policy-trace` and
caught the exact divergence: identical resident set, different cursor, different
victim. Five Phase-D arm-D cells are genuinely non-deterministic; treat their
numbers as a distribution.

**Phase B** recomputed the prefetch hit-rate metric as *total fetches* and
showed the "14–46 % ceiling" three items had chased was an artefact of the
rate's own denominator — issuing more prefetches mechanically dilutes it. The
mechanisms' real byte savings are not in question; the "stuck at a ceiling"
narrative was.

## Overnight sessions — declared order and the real model

**WP1** implemented amendment A-12: the policy interface accepts a
workload-declared access sequence. `layer_order` split into
`layer_order_declared` (new default; distance is a lookup into the declared
sequence) and `layer_order_learned` (the inferring variant, retained as the
comparison arm). Declared order reads exactly the Belady optimum wherever the
learned policy was deterministic — but is still non-deterministic under the full
three-factor trigger, the dependence having moved to the consumption signal
racing concurrent fault dispatch.

**WP0** (session 2) acquired the model — Qwen2.5-3B-Instruct Q4_K_M — and, with
no spec on disk, implemented a minimal consumption-signal fix from the evidence:
`lo_declared_dist()` returns 0 for `seq[pos]` / `seq[pos-1]` (the
"protect-current" heuristic).

**WP2** integrated llama.cpp: a GGUF parser and per-layer chunk table
(`residctl_llama.c`), greedy generation with a per-layer eval callback
(`wp2_gen.cpp`), and a ~35-line `dlsym` hook in `llama-mmap.cpp`. Correctness
gate passed — byte-identical tokens, `mmap` load vs `residctl` load. Kernel LRU
thrashes on the real model exactly as synthetically. Arm D reads far less than
the kernel; D/OPT ≈ 1.1. The synthetic "prefetch beats D under compute" finding
does *not* transfer — real per-layer compute is ~30× lighter. Found the GGUF
stores tensors in name-lexicographic, not layer, order, with layer 21 split
across two non-contiguous chunks. **Arm E collapsed at r=0.25** (360 s timeout).

## Final measurement session

**Phase 1** re-ran the real-model sweep at a genuinely equal budget (WP2 had
given arm A a 256 MiB `memory.max` margin) — arm D beats arm A on bytes at every
ratio; the "D ≈ A at r=0.25" result was the confound. Throughput scales 2.1× for
arm D across a 3× budget range; the kernel arms are flat.

**Phase 2** added `--consumption-signal all-threads` (fire the notify when
*every* driver thread finishes a chunk, not when thread 0 starts it). Made
`layer_order_declared` deterministic at all six stress cells and removed the
last D/OPT gap. Concluded the `protect_current` heuristic was redundant and
flipped its default to `off` on both paths.

**Phase 3** read a SIGUSR1 residency dump of arm E's r=0.25 hang — `resident_bytes`
well under budget, fetch workers in `futex_do_wait` — and called it a hard
deadlock (an orphaned `FETCHING` slot).

## Cleanup session

**gdb backtraces plus a 420 s counter trace proved the arm-E hang is a
livelock, not a deadlock** — fetch workers active the whole time, every counter
climbing linearly, ~90× I/O amplification, would finish in ~6 h. Framed it as an
interaction of two individually-correct mechanisms (pinned retention ×
current-chunk protection) and mitigated by config. Implemented the
`pager_abandon_fetch` + FETCHING-watchdog fix (A-13) — a real fix for a
*separate* latent orphaned-slot class. **Also found the final session's
`protect_current off` default regressed the real model by +67–78 %** and
reverted `residctl_llama.c` to `on`. Wrote the novelty argument
([`04-related-work.md`](04-related-work.md)).

## LIVELOCK FIX session

A source review of `policy.c` / `wp2_gen.cpp` found the actual cause of the
arm-E failure — three application bugs, not a mechanism interaction:

1. `lo_declared_dist()` scanned `for d = 1..seq_len`, so the chunk being
   actively consumed ranked as the *coldest* (an off-by-one in the distance
   origin).
2. The eval callback acted on the *post*-compute pass, so the declared cursor
   lagged the real access by a full layer.
3. The callback matched `"inp_embd"`; llama.cpp names the embedding node
   `"embd"`, so `token_embd` received *zero* consumption signals.

Phase 0 caught bug 3 as a GATE failure and stopped. The user authorised a fourth
fix (match `"embd"`) plus a startup audit that aborts on any zero-signal
declared chunk. Phases 1–3 applied the signal-mode-aware distance origin, the
pre-compute callback, and a 100 ms decline backoff, then re-measured:

- **The livelock is fixed, not avoided** — arm E completes at every ratio with
  `protect_current` on *or* off; the exact prior-livelock config finishes in
  ~50 s (was `rc=124` / ~6 h).
- **The synthetic path is byte-identical** — the post-mode distance origin is a
  no-op refactor of the old loop.
- **Arm D byte efficiency barely moves** — the fixes' value is the livelock fix,
  the `stat_prefetch_declined` collapse (2104 → ~20), and the signal-correctness
  audit.
- The cleanup session's "+67–78 % for protect-off" was a bug-2/bug-3 artifact.
  `protect_current` now defaults **off** on both paths (amendment A-14).

## Repository restructure

Renamed every file by content, split `PROJECT_STATE.md` into
[`findings.md`](../results/findings.md) / [`superseded.md`](../results/superseded.md)
/ [`05-limitations.md`](05-limitations.md), rewrote `MECHANISM_SPEC.md` as
[`02-design.md`](02-design.md) with its 14 amendments moved to
[`design-history.md`](design-history.md), and archived process noise (per-run
configs, console logs, duplicate source snapshots, session prompts) out of git.
Notes: `experiments/logs/restructure-notes.md`.
