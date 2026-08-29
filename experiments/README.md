# Experiments

Each file is a **pre-registered record** — it states its expectations before the
run and marks each one HELD / DID NOT HOLD / PARTIAL against a measured value.
They are moved and renamed but never edited; that is what makes them credible.

Numbering follows the work chronologically. Where a campaign had sub-phases that
don't each warrant a mainline number, they carry a topic-grouped sub-letter
(all the chunk-size work is `09*`, the metric-audit thread is `12*`). Experiment
`01` — the feasibility spike — lives in `/root/spike/results/`, not this repo.

**"numbers"** below: *current* = the record's numbers are still the ones cited;
*superseded* = a later experiment replaced them (see
[`../results/superseded.md`](../results/superseded.md)); *method* = the record is
a correction to how measurement is done, not a result.

| # | record | question | answer | numbers |
|---|---|---|---|---|
| 02 | `02-first-harness-superseded.md` | first six-arm sweep — does app-authoritative residency beat the kernel? | yes in direction, but the harness had three defects (circular OPT input, unequal work across arms, wrong metric) | **superseded** by 03 |
| 03 | `03-corrected-harness.md` | re-run with the reference trace, equal-work reads, `read_bytes` as the metric | LRU thrashes at 100 % miss; arm D reads far less; OPT is now a valid bound | current (synthetic) |
| 04 | `04-io-pipelining-diagnostic.md` | why is prefetch hit rate only 14–27 %? | device-busy caps at 0.82–0.86; arm A exceeds the `O_DIRECT` ceiling (host-cache confound); 3 latent concurrency bugs | method + current |
| 05 | `05-async-handler.md` | make the handler dispatch-only with a fetch-worker pool | the synchronous design made the `FETCHING` dedup branch structurally unreachable; admission guard (A-6) declines 0/36 — a recorded negative | method |
| 06 | `06-concurrent-demand.md` | multi-thread the driver; add prefetch retention | retention (A-9) is the real hit-rate mechanism; `stat_pin_broken` is the regime diagnostic | current |
| 07 | `07-lookahead-window.md` | replace the hard barrier with a bounded lookahead window | the barrier made async throughput untestable; retention's byte effect *reverses* under real overlap (A-11) | current |
| 07b | `07b-existing-data-analysis.md` | re-analyse existing data before Campaign 11's runs | arm E does show fetch overlap (10e's "1.00" was arm-D-specific); a gate-script policy confound | current |
| 07c | `07c-platform-io-microbenchmark.md` | raw `O_DIRECT` concurrency, no pager | plateaus at ~3400–4300 MiB/s past 2 threads; `fd-per-thread` doesn't lift it | current |
| 08 | `08-compute-phase.md` | add a calibrated compute phase to the driver | hit rate *falls* with more compute (opposite the hypothesis); arm E beats D on bytes at r ≥ 0.5 under heavy compute | **superseded** for the real model (14/21) |
| 09 | `09-chunk-size-sweep.md` | sweep chunk size {32…256 MiB} | arm D bytes and wall-clock both climb with chunk size | **partially superseded** by 09d below 32 MiB |
| 09b | `09b-consolidated-6arm-sweep.md` | Campaign 11's consolidated 6-arm sweep | replicates 08's E-beats-D-under-compute on a separate grid | superseded (see 08) |
| 09c | `09c-baseline-io-repair.md` | Campaign 11's arm-A `read_bytes = 0` — host cache, or a bug? | a guest-side `drop_caches` redirect bug in both sweep scripts; re-ran with a repaired script + startup guard | method — supersedes C11 arm A/B |
| 09d | `09d-chunk-size-floor.md` | extend the chunk-size sweep below 32 MiB | wall-clock and bytes *diverge* there; no clean single floor | current |
| 10 | `10-consolidated-sweep.md` | the 6-arm × 5-ratio × 2-compute paper table | OPT ≤ D everywhere non-degenerate; one arm-D cell degenerates to arm C | current (synthetic); 5 arm-D cells **superseded** by 11 |
| 11 | `11-policy-determinism.md` | why do those arm-D cells degenerate? | `layer_order`'s successor chain is fault-dispatch-order-derived — timing-dependent under driver-threads ∧ lookahead ∧ compute together | current |
| 12 | `12-metric-audit.md` | is the 14–46 % hit-rate "ceiling" real? | no — it's an artefact of the rate's denominator; total fetches show no ceiling | method — supersedes the ceiling narrative |
| 12b | `12b-hitrate-count-hypothesis.md` | does heavier compute reduce prefetch *count*? | no — count rises; the hit-rate-down/bytes-up contradiction stays open | current (negative) |
| 12c | `12c-claims-rederivation.md` | re-derive the central claims on the total-fetches metric | D beats A on bytes in 8/8 clean cells; chunk size changes which of E/D wins | current |
| 13 | `13-declared-access-order.md` | implement a workload-declared access sequence (A-12) | declared order hits Belady exactly where the learned policy was deterministic; still non-det under the 3-factor trigger | current (synthetic) |
| 14 | `14-real-model-integration.md` | integrate llama.cpp; does the thesis hold on a real model? | yes for LRU-thrash / arm-D-wins / D-OPT ≈ 1.1; the prefetch-vs-compute finding does *not* transfer | current; some numbers **superseded** by 15/21 |
| 14b | `14b-figure-generation-notes.md` | figure build (WP3) | which figure comes from which CSV | notes |
| 15 | `15-equal-budget-baseline.md` | re-run the real model at a genuinely equal budget | arm D beats arm A at every ratio; throughput scales 2.1× for D, flat for A/C | current; arm-D column **superseded** by 21 |
| 16 | `16-consumption-signal.md` | the exact `--consumption-signal all-threads` signal | deterministic at all 6 stress cells; removes the last D/OPT gap; `protect_current` default → off (synthetic) | current |
| 17 | `17-prefetch-collapse.md` | arm E's r=0.25 hang | read from a SIGUSR1 dump as a hard deadlock / orphaned `FETCHING` slot | **superseded** by 17b |
| 17b | `17b-livelock-diagnosis.md` | is it actually a deadlock? | no — a livelock: workers active, counters linear, ~90× I/O amplification, ~6 h to finish. Framed as a two-mechanism interaction | diagnosis **superseded** by 18–21; A-13 fix current |
| 17c | `17c-figure-refresh-notes.md` | figure refresh (final session Phase 4) | which figure from which CSV | notes |
| 18 | `18-signal-audit.md` | does the real-model consumption signal reach every declared chunk? | **no** — `token_embd` got zero signals (`eval_cb` matched `"inp_embd"`; the node is `"embd"`). GATE failed, stopped | current |
| 19 | `19-livelock-fix.md` | fix the four defects (signal-mode-aware distance origin, pre-compute callback, `embd` match, decline backoff) | applied; unit tests for both signal modes pass; regression gates pass | current |
| 20 | `20-livelock-synthetic-recheck.md` | did the fixes change the synthetic path? | **no** — every `all-threads` cell byte-identical to the final session; the post-mode origin is a no-op refactor | current |
| 21 | `21-livelock-real-model.md` | re-measure the real model with all four fixes | the livelock is **fixed** (arm E completes at every ratio, either `protect_current` setting); arm D within noise; D/OPT 1.08–1.13; throughput 2.5× | **current** — supersedes 15's arm-D column |
