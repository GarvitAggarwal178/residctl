# Project State — residctl

Reference document, not a narrative. Consolidates every headline result,
open question, disclosed limitation, spec amendment, and superseded
number across the spike and every project item through Campaign 11.
Compiled from `CLAUDE.md`, `docs/MECHANISM_SPEC.md`, and the reports named
next to each entry below — not re-derived or re-measured for this
document.

---

## 1. Every measured headline result

**Spike** (`/root/spike/results/SPIKE_REPORT.md` + addenda; constants
reproduced in `MECHANISM_SPEC.md` §2):
- `O_DIRECT pread`, 150 MiB: 61 ms median (2450 MiB/s).
- `UFFDIO_CONTINUE`, 150 MiB: 2.9 ms median (50,900 MiB/s), 4.5% of fetch.
- `UFFDIO_COPY`, 150 MiB: 29.0 ms (5164 MiB/s) — 48% more than CONTINUE, rejected.
- Buffered `pread`, 150 MiB: 128 ms median, 6× spread — rejected.
- `memory.swap.max` left at default: 236 MiB swapped per run (S3e) — I-3's origin.
- Max measured `O_DIRECT` bandwidth (used as the "exceeds spike max" reference threshold in items 10b/10e and this campaign): 3396 MiB/s.

**Item 1-9** (`CLAUDE.md`'s own per-item notes; no dedicated report files):
- Item 7: `lru` 40/40 faults (100% miss, sequential flooding) vs. `layer_order` 32/40, on an 8-chunk/3-budget smoke test.
- Item 9: naive-vs-fast Belady cross-check 300/300 exact matches; cyclic-scan measured misses (15.56-15.8/pass, W=20/K=5) exceed the naive `(1-r)×W` formula — formula replaced by A-2's exact floor.

**Item 10 V1** (`results/HARNESS_REPORT.md`, SUPERSEDED — see §6 below):
- Void; superseded by 3 defects. Not a source of live numbers.

**Item 10 V2** (`results/HARNESS_REPORT_V2.md`):
- OPT at/above the exact cyclic floor at every ratio (65/48/32 vs. floors 64/48/32).
- OPT ≤ D and E-never-beats-OPT hold at every ratio (r=0.25/0.5/0.75).
- `lru` (arm C) thrashes at 100% miss at every ratio.
- `MADV_RANDOM` 60-75× slower than sequential under full-chunk consumption.
- D's bytes/touch falls with more budget (115.8→95.6→68.8 MiB); kernel-native arms stay flat (~134 MiB/touch).
- Pager ~2.3× slower per byte than kernel mmap at r=0.25 (synthetic-benchmark scale, no compute phase — see item 10b Task A and Campaign 11 Phase 2 for the compute-phase follow-up).

**Item 10b** (`results/DIAGNOSTIC_REPORT.md`):
- Arm D device-busy fraction 0.82-0.86, per-fetch bandwidth 85-95% of spike O_DIRECT median.
- Arm A's V2 throughput (4785 MiB/s) exceeded the spike's own O_DIRECT max (3396 MiB/s) — first identification of the WSL2 VHDX host-cache confound.
- `reconcile()` on every fetch: ~14% secondary handler-overhead contribution (→ A-3).
- 3 concurrency bugs found and fixed at `--prefetch-depth` 2/4/8 (see §4).
- Depth sweep: device-busy rises with depth at r=0.25/0.5, flat at r=0.75; wall-clock vs. depth non-monotonic at r=0.75.
- Prefetch hit rate 14-27%, flat across depth — the finding items 10c/10d/10e/Campaign-11 all followed up.
- `dedup_fetching` provably unreachable under the synchronous handler (source-level proof).

**Item 10c** (`results/ASYNC_REPORT.md`):
- T-6: `dedup_fetching=14494` on a 60s storm — first-ever nonzero observation, proof the async handler works.
- Sweep 1 (arm D, sync vs. async): device-busy stayed in the 0.82-0.86 band at every `--fetch-workers` count — arm D structurally has only one demand fault outstanding at a time.
- Sweep 2 (6-arm): E does not beat D at any ratio (contradicts a pre-registered expectation).
- Sweep 3: `guarded` vs. `always` admission statistically indistinguishable at all 12 cells, `stat_prefetch_declined=0` everywhere — `layer_order`'s own victim selection already subsumes the admission guard.

**Item 10d** (`results/CONCURRENCY_REPORT.md`):
- Task A gate: `--driver-threads` 1 vs. 8 identical on trace/bytes/OPT — PASS.
- Sweep B: `dedup_fetching` 0→scales with driver threads under async (0/1/2/4 threads); device-busy did NOT rise with driver threads even under async (barrier-caused, diagnosed not just observed).
- Task C: hit rate rose over `none` at 12/12 directly-compared cells under `pinned` retention; exceeded the old 14-27% ceiling at 2/6 (ratio,depth) cells (r=0.75: 0.32, 0.46).
- Found and fixed a 3rd latent concurrency bug (`do_one_prefetch`'s 3 unlocked `pin` ops).

**Item 10e** (`results/LOOKAHEAD_REPORT.md`):
- Task A gate: `--lookahead-window` 0/1/2 identical on trace/bytes/OPT; W=0 reproduces item 10d exactly — PASS.
- Sweep B: concurrently-outstanding fetches = exactly 1.00 in ALL 36 (ratio,window,threads,handler) cells on arm D, including W=2/8-threads/async — the headline negative result of the item.
- `fd-per-thread` did not lift the ceiling over `shared-fd` — ruled out the single-`model_fd` hypothesis directly (later confirmed again by Campaign 11 Phase 1).
- Sweep C: `r_c` (the `stat_pin_broken`-zero/nonzero boundary) is depth-dependent — between 0.25-0.375 at depth=2, between 0.375-0.5 at depth=4. Retention's byte-cost effect REVERSED from item 10d: `pinned` cost more bytes than `none` at every cell but one, under real overlap.

**Campaign 11** (this campaign; `results/phase{0,1,2,3,4}_*.md`):
- Phase 0.1: arm E (prefetch on) DOES show real fetch overlap (median 1-3, max 7) — item 10e's "exactly 1.00" finding was arm-D-specific, not general.
- Phase 0.2: item 10e's dramatic 5.177s→3.379s→3.227s gate-run wall-clock improvement does NOT reproduce in Sweep B's own `layer_order` data — traced to a policy confound (the gate script used `policy=default`, not `layer_order`).
- Phase 0.3: confirmed by independent re-derivation — only a per-chunk lock (`c->lock`/`target->lock`) is held at the `pread()` call site in both `do_one_demand` and `do_one_prefetch`; no global lock. Prior review CONFIRMED correct.
- Phase 1: platform microbenchmark (no pager) — neither "scales" nor "serialises" holds cleanly for `O_DIRECT`; aggregate throughput rises materially 1→2 threads then plateaus at a ~3400-4300 MiB/s ceiling through 8 threads (per-thread rate roughly halves at each doubling). `fd-per-thread` vs `shared-fd`: no meaningful difference (confirms item 10e's own inconclusive hint).
- Phase 2: STOP-AND-REPORT gate passed (compute=0 reproduces item 10e exactly). Prefetch hit rate FELL with more compute time in 14/18 series — the opposite of the pre-registered hypothesis. Arm E beat arm D's `read_bytes` at r=0.5 and r=0.75 under heavy compute (400000) — the first time in the project's history E has beaten D at this scale under `layer_order`.
- Phase 3: chunk-size sweep {32,64,128,256} MiB — OPT ≥ floor gate passed at all 12 (chunk size, ratio) combinations. Arm D's `read_bytes` and wall-clock BOTH monotonically increase with chunk size at every ratio — minimum among tested sizes is always 32 MiB (smallest tested), not 128 MiB. Handler-overhead fraction did NOT shrink at larger chunk sizes as predicted.
- Phase 4: consolidated 6-arm/5-ratio/2-compute sweep. OPT ≤ D held at all 10 cells. E beat D's `read_bytes` at exactly r=0.5 and r=0.75, only under heavy compute — an independent replication of Phase 2's finding on a separate grid.

---

## 2. Every open question

| Question | What would answer it | Answerable on WSL2? |
|---|---|---|
| Why does `pread()` never show >1.0 concurrently-outstanding on arm D even with genuine cross-chunk demand? | A platform I/O trace tool that can see below `pread()` (e.g. `blktrace`/`iostat` inside the WSL2 VM, or testing on bare metal) — Phase 1 tested raw concurrency directly but did not instrument the exact serialization point. | Partially — Phase 1's microbenchmark IS answerable on WSL2 and was run; isolating WHERE inside the I/O stack the ceiling sits (guest kernel, virtio-scsi layer, or the Windows host) likely requires bare metal or host-side tooling outside this project's guest-only access. |
| Why did hit rate FALL (not rise) with heavier compute in Phase 2, opposite the pre-registered hypothesis? | A per-prefetch survival-time trace correlated against the lookahead window's step-completion timing — not built this campaign; the plausible mechanism (heavier compute lengthens the FIFO-cap/pin-break exposure window) is offered but not directly instrumented. | Yes, in principle — no WSL2-specific barrier; just unbuilt instrumentation. |
| Is arm B's real, ratio-invariant `read_bytes` (10,057,940,992, Phase 4) genuine hint-driven reduction or partial host-cache contamination? | A host-side (Windows) cache-clearing mechanism, or running the same sweep on bare metal / a fresh VM with a cold cache. | Not from inside the guest — explicitly out of scope ("do not attempt to defeat the Windows VHDX host cache"). |
| Does the platform's I/O ceiling (Phase 1: ~3400-4300 MiB/s aggregate, plateauing past 2 threads) reflect a genuine hardware/virtio limit, or a WSL2-specific virtualization artifact? | The same microbenchmark run on bare metal Linux against the same physical disk. | No — this project has guest-only WSL2 access; bare metal is out of scope per every report's own repeated caveat. |
| Is there a chunk size below 32 MiB that continues to reduce bytes/wall-clock (Phase 3 found a monotonic trend, not an interior minimum)? | Extending Phase 3's chunk-size grid downward (e.g. 4, 8, 16 MiB). | Yes — no barrier, simply unbuilt/unrun this campaign, time-boxed out. |
| Does the depth-dependent `r_c` boundary (item 10e) generalize to depths other than {2,4}, or budget ratios finer than the tested grids? | A finer-resolution sweep across depth × ratio. | Yes, unbuilt/unrun. |
| Would llama.cpp integration (item 11) change any of these findings under a real, non-cyclic-scan, non-synthetic access pattern? | Building item 11 — explicitly out of scope for every item through this campaign. | Unknown — the single biggest unanswered structural question in the whole project; every finding to date is validated only against the synthetic cyclic-scan workload. |

---

## 3. Every disclosed limitation, consolidated and deduplicated

*(Sourced from every report's own "What I did NOT test" / limitations section; duplicates across reports collapsed to one line.)*

- **The Windows VHDX host-side cache is unreachable by guest `drop_caches`.** First identified item 10b (arm A's V2 throughput exceeding spike's own max); re-confirmed directly in Campaign 11 Phase 1 (buffered throughput far exceeding spike max) and Phase 3/4 (arm A's `read_bytes` reading exactly 0 in 11/12 and 5/5 cells respectively). Affects every arm-A/B measurement at this project's scale; never attempted to defeat, per every report's own explicit instruction.
- **Bare metal is out of scope; every number in this project comes from one shared WSL2 VM.** No cross-platform comparison exists anywhere in this project's history.
- **The synthetic cyclic-scan workload (replay driver) is the only workload ever tested.** llama.cpp integration (item 11) remains entirely unbuilt; no finding in this project has been validated against a real, non-cyclic access pattern.
- **Region/chunk scale**: every sweep except Campaign 11 Phase 3 used the fixed V2 scale (2 GiB region / 128 MiB chunks); Phase 3 varied chunk size {32-256 MiB} but region size was never varied.
- **`MADV_RANDOM`'s own n=3 coverage gap** (item 10c ASYNC_REPORT): 2 of 3 reps at r=0.25, 1 of 3 at r=0.75, due to two `timeout 180` hits — never affected `best_mode` selection.
- **`--sync-handler` × `--prefetch-admission guarded`** (item 10c) and **`--sync-handler` × `--prefetch-retention pinned`** (item 10d) were wired but never swept in combination — every retention/admission sweep used the async handler.
- **`--driver-threads`/`--lookahead-window` combined with arm A or B** — never tested; those flags apply only to the pager-driven arms (C/D/E)'s own driver, not `baseline_main`.
- **A budget ratio tight enough to make `E_INFEASIBLE`/OOM fire organically** — unexercised across V1, V2, item 10b, and item 10c; Campaign 11 Phase 3's 256MiB/r=0.25 cell is the closest approach found (`infeasible=43` for arm E under `pinned` retention, not a true censoring event).
- **The exact root cause of Phase 1's platform I/O ceiling and Phase 2's calibration-overhead discrepancy (CPU/cache/memory-bandwidth contention hypothesis)** — both offered as plausible, neither independently isolated further, per each report's own time-box.
- **Arm A's `madvise`-mode sweep was skipped in Campaign 11 Phase 3** (single fixed `sequential` mode used) — disclosed as a deliberate, time-boxed simplification; Phase 4 restored the full 3-mode sweep.
- **Campaign 11 Phase 4's device-busy/concurrently-outstanding data is n=1 supplementary**, not the main sweep's n=3 — a disclosed backfill for an omitted `--fetch-trace` flag, not the main sweep's own statistical power.

---

## 4. The six latent concurrency bugs

Already consolidated into one list in `CLAUDE.md` under "ITEM 10e" (per
that item's own framing note) — not repeated here. Reference:
`CLAUDE.md`, section beginning "**Concurrency bugs found, consolidated
across items 10b-10e**". Summary index only: 3 found in item 10b
(`RECONCILE FAILED` false-positive range; commit-before-`RESIDENT`
deadlock; `commit_reserved_and_pin()` atomicity), 2 in item 10c (unlocked
`latency_hist_record`; unlocked stat counters), 1 in item 10d (3 unlocked
`pin` operations in `do_one_prefetch`). Campaign 11 found zero new bugs
of this class (Phase 2's calibration discrepancy is a measurement-
accuracy issue, not a concurrency bug — no shared state was raced).

---

## 5. Every spec amendment

| # | §, item | What it changed | Why |
|---|---|---|---|
| A-1 | §5/§9, item 10 correction | Reference trace is workload-authored (`TRACE_TYPE_REFERENCE`), never the handler's fault trace; header distinguishes the two; solver aborts on a fault trace. | V1's OPT values fell below the provable cyclic floor — the handler's own trace is circular input to Belady. |
| A-2 | §10, item 10 correction | Replaced the approximate `(1-r)×W` sanity check with the exact, provable cyclic floor `n + (passes-1)×max(n-k,0)`. | Item 9 already found the approximate formula rests on a flawed derivation (demand paging can't permanently pin k items). |
| A-3 | §7/I-7, item 10 correction | `reconcile()` runs on every eviction and every 16th fetch otherwise (was: every fetch); `--eager-reconcile` restores eager checking. | Measured real, un-amortized cost (fresh `memory.stat` open/read/close per fetch), dominant at small chunk sizes. |
| A-4 | §11, item 10 correction | Replay driver (and arms A/B) must consume every page of a chunk per reference, not one byte. | 1-byte touches made arm A read ~250× less data than the pager arms, invalidating both the wall-clock comparison and the `MADV_RANDOM` finding. |
| A-5 | §5, item 10c | Handler thread dispatch-only (never blocks on I/O); fetch moves to a shared worker pool. `--sync-handler` restores the old path. | Item 10b Task C proved the old synchronous design made `dedup_fetching` structurally unreachable, not merely rare. |
| A-6 | §6.3/§8, item 10c Task B | Prefetch admission: `prefetch_admit()` declines an eviction unless the victim is strictly colder than the prefetch's own target (`next_use_distance` added to the policy interface). `--prefetch-admission {always,guarded}`. | Item 10b measured 14-27% prefetch hit rate, flat across depth — hypothesized cause was unconditional eviction forcing out something needed sooner. |
| A-7 | §4 step 10, item 10c | Replaces "one thread" text with the dispatcher+worker-pool architecture description. | Companion amendment to A-5; documents why the handler is no longer single-threaded-and-blocking. |
| A-8 | §11, item 10d Task A | Replay driver is multi-threaded (`--driver-threads N`), hard `pthread_barrier_t` at chunk boundaries. | Item 10c Task C's Sweep 1 found device-busy flat regardless of `--fetch-workers` — driver was single-threaded, never generating cross-chunk concurrent demand. |
| A-9 | §6.3, item 10d Task C | Prefetch target retention: pinned from arrival until consumed (`pager_notify_access()`) or evicted from a bounded `prefetch_depth` FIFO; demand-fetch pin-break override (`stat_pin_broken`). `--prefetch-retention {none,pinned}`. Supersedes A-6 as the primary hit-rate mechanism (A-6 kept as a recorded negative result). | Item 10c Sweep 3 found A-6's admission guard declined 0/36 cells — `layer_order`'s own victim selection already subsumes it; the real waste was a prefetched chunk being evicted before its own turn. |
| A-10 | §11, item 10e Task A | Hard barrier replaced by a bounded lookahead window (`--lookahead-window W`, counting-mechanism-based, provably bounds W+1 chunks in flight). Supersedes A-8's barrier. | Item 10d's own report found the hard barrier made async's throughput benefit (device-busy rising with driver threads) untestable by construction, not merely unconfirmed. |
| A-11 | §6.3, item 10e Task C | Documents `stat_pin_broken` as the regime-boundary indicator for retention's byte-cost effect; records `r_c` as depth-dependent, not a single constant. | Item 10d scored retention's byte effect as "did not hold" without recognizing the budget-tightness-correlated regime split `pin_broken` reveals. |

*(No A-12 exists as of this document — Campaign 11 found no new mechanism-level defect requiring a spec amendment; its findings are reported as measurements and reversals, not fixes.)*

---

## 6. Superseded-results list

**This section is the one this document exists primarily to provide.**

| Superseded numbers | Report | Superseded by | Reason |
|---|---|---|---|
| All V1 OPT values, fault-count-based rankings, and the `MADV_RANDOM` "fastest mode" finding | `results/HARNESS_REPORT.md` (marked SUPERSEDED in place, not deleted) | `results/HARNESS_REPORT_V2.md` | 3 defects: circular OPT input (A-1), arms not doing the same work (A-4), wrong primary metric (fault count vs. `read_bytes`, Defect 3). |
| Item 10c Sweep 1's framing of "device-busy capped at 0.82-0.86 regardless of `--fetch-workers`" as evidence about the async architecture's ceiling | `results/ASYNC_REPORT.md` | Item 10d's diagnosis (`results/CONCURRENCY_REPORT.md`) + item 10e's fix (`results/LOOKAHEAD_REPORT.md`) | The numbers themselves are not wrong, but the INTERPRETATION was: arm D's single-threaded, barrier-free driver structurally could never generate more than one outstanding fetch, so the flat device-busy measured a driver limitation, not an architecture ceiling. Item 10e's Task A re-ran the same style of measurement with real overlap possible and found the SAME flat ceiling persists — see below. |
| Item 10e Sweep B expectation 2 (concurrently-outstanding rises with window under async) | `results/LOOKAHEAD_REPORT.md` | Campaign 11 Phase 0.1 + Phase 2 | Not superseded in the sense of being wrong — item 10e correctly measured concurrently-outstanding=1.00 on arm D. Campaign 11 clarified the SCOPE: arm E (prefetch on) already shows real overlap independent of any window or compute setting (Phase 0.1), so the "1.00" finding is arm-D/prefetch-off-specific, not evidence the mechanism can never overlap. Read item 10e's finding together with Campaign 11 Phase 0/1/2, not in isolation. |
| Item 10d's retention byte-effect regime claim ("`pinned` reduces bytes above `r_c`, costs more below it") | `results/CONCURRENCY_REPORT.md` | Item 10e's Sweep C (`results/LOOKAHEAD_REPORT.md`) | Under real cross-chunk overlap (`--lookahead-window 1`, which item 10d's own barrier-driven driver could not produce), `pinned` cost MORE bytes than `none` at every cell but one — the REGIME-BOUNDARY mechanism (`stat_pin_broken` as indicator) still holds and is not superseded, but the DIRECTION of the effect above `r_c` reversed. Both reports' raw numbers stand as measurements of the driver architecture they were taken under; only the general claim "retention reduces bytes above `r_c`" is superseded. |
| Item 10e's incidental gate-run observation of a 38% wall-clock improvement from `--lookahead-window` (5.177s→3.227s) | `results/LOOKAHEAD_REPORT.md` (one line in an incidental-observation paragraph) | Campaign 11 Phase 0.2 | Traced to a policy confound: the gate script used `policy=default` (the lowest-index FIFO fallback), never used in any other reported sweep, not `policy=layer_order`. Sweep B's own `layer_order` data shows no such effect at any ratio or thread count. The item 10e Task A verification GATE ITSELF (trace/bytes/OPT identity, W=0 regression) is unaffected and remains valid — only the incidental wall-clock observation is superseded. |
| Every prior report's characterization of prefetch as reducing byte cost only in narrow/no cases (items 10b through 10e, uniformly negative or null on this axis) | `results/ASYNC_REPORT.md`, `results/CONCURRENCY_REPORT.md`, `results/LOOKAHEAD_REPORT.md` | Campaign 11 Phase 2 + Phase 4 | Not contradicted at `compute=0` (E still costs more bytes than D everywhere, consistent with every prior report). But under a real compute phase (`--compute-ns-per-mib 400000`, never tested before this campaign — no prior report includes a compute phase at all), E beats D's `read_bytes` at r=0.5 and r=0.75, replicated independently in Phase 2 and Phase 4. This does not retroactively invalidate the zero-compute findings — it adds a regime (heavy compute) none of items 10b-10e tested. |
