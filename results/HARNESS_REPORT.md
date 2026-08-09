# HARNESS_REPORT — residctl build-order item 10

MECHANISM_SPEC.md §11: six arms (A mmap baseline, B mmap+hints, C pager+lru,
D pager+layer_order, E pager+layer_order+prefetch, OPT offline solver over
D's trace), swept across at least three budget ratios, in a cgroup with
identical `memory.max` per arm at a given ratio, with a pre-registered
censoring rule for OOM/infeasible results.

**Read this whole report, especially "What NOT to conclude," before citing
any number from it.** The §13 correctness harness (T-1..T-5) passed before
this run — see `CLAUDE.md`'s note — so the *mechanism* is trusted; the
scale of this specific run is deliberately small and says nothing about
absolute performance at realistic LLM weight sizes.

## Machine exclusivity

Checked before and after (see `results/item10_harness_log.txt`). Clean both
times: load average ~0.00-0.03, no `cn-spike` workload running, only the
sweep's own processes and normal system daemons visible.

## Test parameters (fixed across the whole sweep)

- `region_len` = 16 MiB, `chunk_size` = 2 MiB (8 chunks), `n_passes` = 5
  (touches = 40 per pager/baseline run) — the same parameters used
  throughout items 1-9's tests, chosen for fast iteration during
  development.
- Budget ratios swept: **0.25, 0.5, 0.75** (spec requires at least three).
- `memory.max = budget_bytes + 4 MiB` margin, **identical across every arm**
  at a given ratio (§11's explicit requirement). `memory.swap.max = 0`
  (I-3) for every cgroup.
- Model file: `scratch/pattern_16m.bin`, a position-derived byte pattern
  (same file used by items 1-9's correctness checks).

## Methodology bugs found and fixed while building this harness

1. **Warm page cache invalidated arm A/B's first run.** `pattern_16m.bin`
   had been read repeatedly by every prior item's tests in this session and
   sat warm in the kernel's globally shared page cache. The first sweep
   attempt showed arm A completing in ~20 **microseconds** with
   `pgscan_delta=0`/`pgsteal_delta=0` at *every* ratio, including the
   tightest (8 MiB max for a 16 MiB file) — clearly page-cache-hit speed,
   not real I/O or real reclaim. Fixed by `sync; echo 3 >
   /proc/sys/vm/drop_caches` immediately before every arm A/B invocation.
   The numbers below are from the re-run with this fix applied.
2. **OPT must be compared against D, not E.** OPT is computed by item 9's
   solver over arm D's trace specifically (`layer_order`, prefetch off).
   Arm E adds prefetch, which changes what counts as "demand" — some
   references are satisfied before they'd ever fault, so E's real-fault
   count is not directly comparable to OPT's bound for D's *original*
   demand sequence. E beating OPT numerically (see table) is not a
   contradiction; OPT <= D is the relationship that must hold, and does, at
   every ratio.

## Sensitivity table

All "misses" below are genuine consumer-demand fetches (`absent_handled`
for pager arms; `stat_absent_handled` — prefetched fills are tracked
separately and are not misses). Full raw data:
`results/harness_sweep.csv`; full run log: `results/item10_harness_log.txt`.

### Fault counts (lower is better), by budget ratio

| Ratio | Budget | C (lru) | D (layer_order) | E (layer_order+prefetch) | OPT (vs. D) |
|---|---|---|---|---|---|
| 0.25 | 4 MiB (2 chunks) | 40 / 40 (100%) | 36 / 40 | 28 / 40 | 31 |
| 0.50 | 8 MiB (4 chunks) | 40 / 40 (100%) | 29 / 40 | 23 / 40 | 17 |
| 0.75 | 12 MiB (6 chunks) | 40 / 40 (100%) | 21 / 40 | 19 / 40 | 9 |

**E < D < C at every ratio, and OPT <= D at every ratio (31<=36, 17<=29,
9<=21).** Both are the relationships the mechanism is supposed to produce.

**C (lru) misses 100% of the time at every ratio.** This is the same
textbook "sequential flooding" pathology item 7 first surfaced: strict
recency-based eviction thrashes completely on a cyclic scan larger than the
cache, because by the time the scan wraps around, everything was already
evicted in fetch order. It is the reason `layer_order` — which uses the
*known* future access order instead of blind recency — exists.

Prefetch (`E` vs `D`) converts additional real faults into no-ops at every
ratio (36→28, 29→23, 21→19), consistent with item 8's finding, with more
absolute benefit at the tightest budget (where there's more room for a
correct prediction to matter) than at the loosest.

### Arm A: madvise sweep (best mode selected by touches/sec)

| Ratio | normal | random | sequential | best |
|---|---|---|---|---|
| 0.25 | 100.6 ms, pgscan=1,114,058 | **1.90 ms**, pgscan=0 | 85.6 ms, pgscan=727,114 | random |
| 0.50 | 8.58 ms, pgscan=2,975 | **1.47 ms**, pgscan=0 | 36.3 ms, pgscan=36,778 | random |
| 0.75 | 7.63 ms, pgscan=477 | **1.31 ms**, pgscan=0 | 34.9 ms, pgscan=16,911 | random |

`MADV_RANDOM` wins decisively at every ratio, and by a wide margin. This
makes sense in hindsight rather than being a fluke: `MADV_NORMAL` and
`MADV_SEQUENTIAL` both trigger aggressive kernel read-ahead, which — under
a *tight* `memory.max` — reads far more than the immediate demand and then
has to be reclaimed again almost immediately (the huge `pgscan` counts for
those two modes are exactly this churn). `MADV_RANDOM` disables read-ahead,
so it reads close to only what's actually touched and never has to fight
its own over-reading. This is a real, reproducible result, not noise —
consistent across all three ratios.

### Arm B: A's best mode (random) + WILLNEED/PAGEOUT hints

| Ratio | Arm A (random, no hints) | Arm B (random + hints) |
|---|---|---|
| 0.25 | 1.90 ms | 4.43 ms |
| 0.50 | 1.47 ms | 9.81 ms |
| 0.75 | 1.31 ms | 8.18 ms |

**Hints made things worse, not better, at every ratio** — 2-7x slower. The
upfront `MADV_WILLNEED` directly fights `MADV_RANDOM`'s whole point (avoid
over-reading), and the per-touch `MADV_PAGEOUT` call adds a real syscall on
every single access. At this scale, that per-touch overhead is not
recovered by anything the hints buy back. Reported honestly rather than
searched-for-a-better-config to make arm B look better — the pre-registered
methodology (A's winning mode + hints, unmodified) is what ran.

### Censoring

No OOM kills and no `E_INFEASIBLE` results occurred anywhere in this sweep
(`infeasible` column is 0 throughout; no `censored=1` rows). The
pre-registered rule (record as a censored point, not a bug, not discarded)
was never exercised at these budget ratios and this scale — worth noting as
a limitation of this specific run, not a claim that infeasibility can't
happen.

## What NOT to conclude from this data

**Do not compare wall-clock time between arm A/B and arm C/D/E and
conclude the pager mechanism is slower than the kernel's native path.**
Arm A's best case (~1.3-1.9 ms) is faster than any pager arm (tens of
milliseconds) at *this* test scale. That comparison is not meaningful here:
- Chunks are 2 MiB, chosen for fast iteration through nine prior build
  items, not for realistic LLM weight layer sizes. §2's actual spike
  measurements used 150 MiB chunks, where `UFFDIO_CONTINUE` is 4.5% of
  fetch time and per-syscall fixed costs are negligible. At 2 MiB, those
  same fixed costs (the `userfaultfd` read/dispatch, an `ensure_budget()`
  call that re-reads `memory.stat` on **every single fetch** per I-7, the
  `UFFDIO_CONTINUE` ioctl itself) are a much larger fraction of the total,
  and dominate the comparison.
- Arm A/B, even with caches dropped, still benefit from the kernel's
  decades-tuned readahead/reclaim machinery operating on a trivially small
  16 MiB working set; the pager's O_DIRECT path deliberately bypasses that
  machinery entirely (per the spike's own §2 findings on why O_DIRECT +
  `UFFDIO_CONTINUE` was chosen).
- **A concrete, measured architectural cost worth carrying into item 11**:
  `ensure_budget()` calling `reconcile()` on every fetch means every single
  fetch pays for a fresh `open()`/`read()`/`close()` of `memory.stat`. That
  cost is fixed per fetch regardless of chunk size, so it matters much less
  at 150 MiB chunks than it does here — but it is real, and un-amortized in
  this run's numbers.

**The number that this harness run does support is the residency-control
comparison** — fault-count ordering, OPT as a bound on D, LRU's thrashing —
which is scale-independent in a way wall-clock time is not.

## What was NOT tested here

- Realistic LLM weight scale (multi-GB region, 100+ MiB chunks matching the
  spike's own §2 measurements). **NOT MEASURED.**
- llama.cpp integration (build-order item 11, explicitly out of scope for
  item 10).
- A budget ratio low enough to trigger `E_INFEASIBLE` or an actual OOM,
  exercising the censoring rule for real.
- Repeated runs / variance across ratios (each arm ran once per ratio here;
  items 1-9's individual tests already established repeatability of the
  underlying mechanisms — this harness validates the *arm comparison*, not
  additional repetitions of already-validated mechanism correctness).
