# FINAL SESSION — Phase 3: arm E's collapse at tight budget

**Box:** 60 min. **Actual:** ~90 min (four 360 s `protect_current=on` timeouts
dominated; within 2× box). **Machine exclusive** before/after.

Real model (Qwen2.5-3B Q4_K_M), r = 0.25, B = 501 MiB, `memory.max = B + 128 MiB`
(Phase 1's equal-budget setup), 64 tokens, n = 3.

---

## Step 1 — characterisation (SIGUSR1 residency dump + `/proc/PID/task/*/wchan` at 100 s)

`protect_current=on` (WP2's config) collapses on every run. At the hang, all four
captured dumps show the **same signature**:

```
RESIDCTL_SIGDUMP resident=4-5  resident_unpinned=2-3  pinned=2  fetching=1
                 resident_bytes=148-403 MiB  budget_bytes=526 MiB
                 stat_pin_broken=0  stat_infeasible=0
```

```
tid <main>   wchan = handle_userfault      <- blocked on an unresolved fault
tid <pager>  wchan = poll_schedule_timeout
tid <8 fetch workers>  wchan = futex_do_wait   <- ALL idle, none doing I/O
```

**The collapse is NOT what WP2 guessed** ("too few evictable chunks" / budget
pressure):

- `resident_bytes` is **148–403 MiB against a 526 MiB budget** — budget is *not*
  the binding constraint.
- There are **2–3 RESIDENT + unpinned chunks** — victims *are* available.
- `stat_infeasible = 0`, `stat_pin_broken = 0` — the pager never declares the
  situation infeasible and (mostly) never breaks a pin; it just spins.
- Every fetch worker is parked in `futex_do_wait` — **nothing is actually
  fetching** the one chunk stuck in `FETCHING` state. The main thread waits in
  `handle_userfault` for that fetch to complete; it never does.

**Diagnosis:** a demand fault for a chunk already marked `FETCHING` (deduped
against an in-flight prefetch — `dedup_fetching`) waits on that prefetch, but the
prefetch worker has abandoned / never owned the slot while `--prefetch-retention
pinned` holds 2 chunks pinned and `--protect-current on` shields the live
chunk(s). The chunk sits in `FETCHING` with no owner; the fault never resolves.
This is a **latent deadlock in the prefetch + pinned-retention + protect-current
interaction at a budget tight enough to force the dedup path** — a 7th
concurrency-class issue, exposed here for the first time. **Recorded, not fixed**
(the spec caps Phase 3 at "apply the pre-decided mitigation, do not design a new
one"; and "if a task reveals something new, record it and do not pursue it").

The `--fetch-trace` / `--policy-trace` files are empty for the collapsed runs —
they flush only in `residctl_llama_teardown()`, which `timeout`'s SIGTERM skips.
The SIGUSR1 dump (written straight to stderr) is what survives, and it carries
every quantity Phase 3 asked for.

---

## Step 2/3/4 — the pre-decided configurations (r = 0.25, n = 3)

| config | protect | retention | depth | completes? | read (GB, median) | tokens/s | demand faults | prefetches |
|---|---|---|---|---|---|---|---|---|
| `protect_on` (WP2) | on | pinned | 2 | **NO — 3/3 collapse** | — | — | ~1160 (at hang) | ~130 (at hang) |
| `default_protect_off` (Phase-2 default) | off | pinned | 2 | yes (3/3) | 172.5 | 0.83–0.99 | 1698 | 1702 |
| **`off_retention_none`** | off | none | 2 | **yes (3/3)** | **166.5** | 0.95–1.04 | 1611 | 1672 |
| `off_depth1` | off | pinned | 1 | yes (3/3) | 171.6 | 0.84–0.95 | 2237 | 1214 |
| `off_none_depth1` | off | none | 1 | yes (3/3) | 173.6 | 0.82–0.89 | 2189 | 1214 |

- **`--protect-current off` (Phase 2's new default) alone resolves the
  collapse** — `default_protect_off` completes all 3 reps. Item 3's hypothesis
  holds: the WP0 heuristic *was* the deadlock trigger (it shielded the chunk the
  dedup path needed evicted).
- **`--prefetch-retention none` completes with the lowest `read_bytes`** among
  all completing configs (166.5 GB vs 172.5 for the default) — no pinned prefetch
  chunk means `pinned = 0` at all times, so the dedup deadlock cannot form.
- `--prefetch-depth 1` also completes but reads *more* (171.6) and is slower.

---

## Pre-decided outcome

> "Whichever configuration completes with the lowest `read_bytes` becomes arm E's
> configuration at tight budgets, and that is recorded as a **stated operating
> limit of the prefetch mechanism**, not hidden."

**Arm E's tight-budget (r ≤ 0.375) configuration: `--protect-current off
--prefetch-retention none`** (166.5 GB at r=0.25).

**But the stated operating limit is stronger than a config choice:** at r=0.25,
arm E's *best completing* config reads **166.5 GB — 32 % more than arm D
(126.1 GB, Phase 1) and 15 % more than the kernel LRU (arm C, 144.4 GB).**
Prefetch on this model provides **no benefit at r ≤ 0.375** — it either deadlocks
(protect-on) or thrashes with prefetch overhead on top (protect-off). The honest
recommendation for the writeup and any deployment:

> **Disable prefetch (run arm D) at budget ratios ≤ 0.375 on this model.** Arm E
> is only advantageous at r ≥ 0.5, where it trades ~7 % more bytes for ~2× fewer
> demand faults and lower p99 latency (Phase 1).

No configuration was found where arm E *beats* arm D at r=0.25; and the
protect-on deadlock is a real bug that the writeup should name, not paper over.

---

## Files

- `results/final/phase3_arm_e.csv` — 16 rows (5 configs × 3 reps + 1 traced).
- `results/final/phase3_log.txt` — full log incl. the 4 SIGDUMP + wchan captures.
- `src/run_final_phase3.sh`, `src/phase3_supervisor.sh`.

## Final check

- No number estimated: every value is a measured median of 3 runs from
  `phase3_arm_e.csv`; the hang-state counters are direct SIGUSR1 dumps.
- A hang is reported as a hang (rc = 124), with its mechanism, not smoothed over.
- The pre-decided mitigation was applied; no new mechanism designed. The
  newly-found deadlock is recorded and explicitly left unfixed.
- The pre-decided outcome (lowest-read_bytes completing config) is stated, and
  the harder truth (arm E has no *advantageous* config at r=0.25) is stated too.
