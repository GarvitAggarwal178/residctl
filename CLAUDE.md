# residctl — project memory

Application-authoritative page residency for large read-only working sets
(userfaultfd + memfd_create + cgroup v2), built on top of the feasibility spike
at `/root/spike/`. That spike answered "can this work at all" (yes — see
`/root/spike/results/SPIKE_REPORT.md`, `SPIKE_ADDENDUM.md`,
`SPIKE_ADDENDUM2.md`). This project answers "build the thing."

## Source of truth

- **`docs/MECHANISM_SPEC.md`** — the implementation contract. Every invariant
  (I-1..I-10), every measured constraint (§2), the fixed build order (§12), and
  the correctness harness (§13) live there. Code that violates an invariant is
  wrong even if it runs and even if it's faster. Read it before touching src/.
- **`/root/spike/results/`** — where every number in MECHANISM_SPEC §2 came
  from. Do not re-derive them; if a number is needed that isn't there, measure
  it and record where.

## Rules carried forward from the spike (still binding)

- Never fabricate, estimate, or extrapolate a number. If untested, say
  "NOT MEASURED."
- Never weaken a test to make it pass. Report failures with the exact
  error/errno/command.
- Never conclude success from absence of a crash — check the stated pass
  criteria (T-1..T-5 in MECHANISM_SPEC §13, per-arm criteria in §11).
- Don't silently fix the environment. No kernel rebuilds, no package installs,
  no .wslconfig edits. Report what's missing and stop.
- Machine exclusivity during any measurement run: check for other workloads
  before and after (the spike was contaminated by exactly this once). If found,
  don't kill it — stop and report.
- Time-box; if exceeded, stop, record partial results, move on.

## Build order (fixed, do not reorder — see MECHANISM_SPEC §12)

| # | Item | Est. | Status |
|---|---|---|---|
| 1 | Region setup + startup assertions (§4) | 2h | done |
| 2 | Handler loop + chunk table + state machine (§3, §5) | 4h | done (see note) |
| 3 | Fetch path with alignment (§6.1–6.2) | 3h | done, built together with item 2 |
| 4 | Eviction + budget + reconcile (§7) | 2h | done (temporary victim selector, see note) |
| 5 | Trace recorder + metrics (§9) | 2h | done |
| 6 | Trace-replay driver | 2h | done |
| 7 | `lru` and `layer_order` policies (§8) | 2h | done |
| 8 | Prefetch (§6.3) | 1h | done (inline, not a separate thread -- see note) |
| 9 | Belady solver (§10) | 2h | done (spec's own sanity formula turned out wrong -- see note) |
| 10 | Harness, arms, sweep (§11) | 3h | not started |
| 11 | llama.cpp integration (separate spec, stretch) | 7h | out of scope for now |

The replay driver (item 6) comes before any engine integration so there is
always a complete, runnable result even if integration runs long.

**Deviation from the fixed order, disclosed rather than silently done:**
items 2 and 3 were implemented and committed together. §5's handle_absent()
pseudocode calls fetch_chunk() directly, so the handler loop cannot be
tested in isolation from the fetch path -- there's no meaningful "state
machine works" checkpoint that doesn't also exercise §6. Everything else in
the fixed order stays separate.

**Known gap in item 2/3's own test** (`test_pager.c`): the dedup branches in
`handle_fault()` (I-8's RESIDENT/FETCHING cases) are implemented per spec and
believed correct by inspection, but a deliberate barrier-synchronized,
8-thread race onto a single cold chunk produced exactly one uffd message in
5/5 observed runs, not several -- the race window wasn't hit. This is
reported, not silently fixed by adding a test-only delay hook into
production fetch code. §13's T-3 (sustained concurrent storm with real
eviction cycling, once item 4 exists) is the right place to actually confirm
this path fires.

**Item 9 note -- the spec's own required sanity check turned out to be
wrong, investigated rather than silently loosened or silently trusted.**
§10 requires: "on a strictly cyclic reference string at budget ratio r, the
solver must return approximately (1-r) x W bytes per pass. If it doesn't,
the solver is wrong, not the theory." Implemented that check first and it
failed: for W=20 items/cycle, K=5 slots (r=0.25), steady-state measured
15.56 misses/pass against an expected 15.00 -- small but stable and
reproducible (checked P=10 through P=1000, converges to ~15.79..15.8, not
15). Per the spec's own instruction, treated this as "solver is wrong" and
investigated by hand: traced a small W=4,K=2 example step by step and found
the naive "(1-r)*W" derivation assumes you can permanently pin K items in
cache -- false under strict demand paging, because every miss for a
"cold" item still needs a slot *right now*, which necessarily evicts one of
the supposedly-pinned items. The naive bound isn't achievable; the true
optimal is higher, matching what was measured.

Rather than trust that reasoning alone, cross-checked `belady_simulate()`
(the heap-based, lazily-deleted O(n log n) implementation) against a
deliberately naive O(n^2) reference (linear forward scan for the true next
occurrence at every eviction -- nothing clever to get wrong) across 300
random reference strings/capacities: 300/300 exact matches. That random
cross-check is now `belady_main --selftest`'s actual pass/fail gate; the
cyclic-pattern check from §10 is still computed and printed (per the
spec's letter) but as an informational diagnostic, explicitly labeled "NOT
a pass/fail bound," not asserted against the wrong formula. Also verified:
on a real trace from the item 8 scenario (layer_order+prefetch, 5 passes),
OPT reported 19 minimum misses against the online policy's actual 28 --
OPT <= online always must hold, and does.

**Item 8 note -- a design deviation disclosed up front, and a real bug
caught before it shipped.**
§6.3 says "enqueue chunk N+1 for asynchronous fetch," which could mean a
dedicated prefetch OS thread. `prefetch.c` does NOT add one: §4 step 10 is
explicit that multi-threading "must be justified by measured handler queue
depth, not assumed," and no such measurement exists, and a real second
thread would need a new region-wide accounting lock (`resident_bytes`/
`ensure_budget`/`evict_chunk` currently assume a single caller). Instead,
prefetch runs INLINE in the single handler thread immediately after the
triggering fetch completes -- still satisfies §6.3's observable requirement
(a correctly predicted chunk generates zero fault later) without new SMP
surface, and "one outstanding prefetch maximum" holds trivially since it's
synchronous by construction. Full reasoning in `prefetch.h`'s header
comment.

While building this, found a real deadlock risk before it ever ran: inside
`maybe_prefetch()`, calling `ensure_budget()` for the prefetch target lets
the policy's `select_victim()` legitimately choose the chunk that JUST
became resident (`just_resident`) as its own victim -- under `layer_order`
specifically, a chunk is "unreachable from itself" in the successor walk,
i.e. it looks infinitely far away, exactly the profile `select_victim`
prefers to evict. `just_resident`'s lock is already held by the calling
`handle_absent()` (I-6), so `evict_chunk()` locking it again would deadlock
the single handler thread against itself. Fixed by pinning `just_resident`
(the existing I-4 mechanism) for the duration of the prefetch's
`ensure_budget()` call. `test_prefetch.c` is deliberately wrapped in
`timeout` so a regression here hangs and fails loud, not silent. 3/3 clean
runs: `lru` (whose `predict_next` is always -1) never prefetches (0/0);
`layer_order` + prefetch measurably beats the item 7 no-prefetch baseline
on the identical scenario (28 real faults vs. 32, 13 successful prefetches).

**Known measurement gap, disclosed:** a touch on an already-resident chunk
(prefetched or not) generates no uffd event at all, so "prefetch hits
(faults avoided)" can't be directly counted -- `stat_prefetches` (successful
prefetch completions) is the honest proxy actually available, not a true
hit count. Whether each prefetch was later touched (a real hit) or evicted
unused (wasted) isn't tracked in v1.

**Item 7 note -- an unplanned but genuinely useful observation.**
`policy.c` implements `lru` (evict min `last_fault_seq` among RESIDENT+
unpinned -- note this is age-since-LAST-FETCH, not general access time,
because that's literally the only recency signal a fault-driven design has:
touching an already-resident page installs no PTE change and generates no
event at all, so there's nothing to observe) and `layer_order` (learns a
successor map online from the ACTUAL fetch sequence via `on_fault`, never
from a declared/assumed order per §8; `select_victim` walks the learned
chain from "now" and evicts whichever RESIDENT+unpinned chunk has the
largest next-use distance, treating never-observed chunks as infinitely far
-- the standard Belady approximation). `ensure_budget()` (item 4) now
dispatches to `r->policy->select_victim()` when a policy is set, falling
back to the old lowest-index rule when `r->policy` is NULL, so items 4-6's
tests keep passing unchanged. `test_policy.c` unit-tests both policies
directly (no memfd/uffd needed) against hand-derived cases, including one
built specifically to distinguish Belady-style prediction from "lowest
index": residents `{2,3}` with chunk 2 due next and chunk 3 due later must
evict 3, not 2. 3/3 clean.

Smoke-testing `replay_main` (8 chunks, 3-chunk budget, 5 cyclic passes)
across all three policies turned up something worth keeping: `lru` produced
40/40 faults (100% miss rate -- textbook LRU "sequential flooding": strict
recency-based eviction thrashes completely on a cyclic scan larger than the
cache, because by the time the scan wraps around, everything just got
evicted in fetched order anyway), while `layer_order` tied the `default`
fallback at 32/40. This isn't a designed correctness test, but it's a real,
reproducible demonstration of exactly the effect item 10/11's harness is
supposed to measure -- informed eviction beating recency-based eviction on
an LLM-like cyclic access pattern -- and it fell out of an ordinary smoke
test, not a rigged one. Numbers are from one WSL2 run, not a formal
benchmark; treat as illustrative until item 10 does this properly.

**Item 6 note, a test bug caught and fixed, not the implementation:**
`replay_cyclic()` (`replay.c`) sweeps chunks 0..n_chunks-1 for n_passes,
touching each once via `map_a` -- this is the synthetic "known cyclic
order" workload standing in for llama.cpp (item 11) per §12. The first
version of `test_replay.c` asserted a pure-cyclic trace (every touch faults,
chunk_id cycles 0..7 forever) and failed: 20 faults observed, not 24. This
was the TEST's bug, not `ensure_budget()`'s -- the temporary FIFO-by-
lowest-index victim selector (item 4) makes the two highest-indexed chunks
in a sweep permanently resident after the first pass (they're never the
*lowest* index among residents, so they're never picked as victims again).
Verified independently in Python before touching the test (see the
commit), then rewrote the test around a reference-oracle simulation of the
same selection rule instead of a closed-form guess. 3/3 clean runs after
the fix, oracle sequence `[0,1,2,3,4,5,6,7, 0,1,2,3,4,5, 0,1,2,3,4,5]`
matches the trace exactly. `replay_main` (the standalone CLI, reusable by
item 10's harness) smoke-tested separately and is self-consistent with the
same pattern extended to 4 passes.

**Item 5 note:** the region_t bare counters from items 2-4 (fault/dedup/
eviction) were kept as-is rather than folded into `metrics_t` -- they're
needed by items 2-4's own tests even when `metrics` is NULL. `metrics_t`
(`metrics.c`) adds what those didn't cover: a handler-latency histogram
(plain log2-bucketed, explicitly NOT a real HDR histogram library -- see the
comment in `metrics.h`) and queue-depth high-water. `trace.c` implements
the binary trace writer; `cgroup_stat.c` is the shared single-buffer
cgroup-file reader §9 asks for, and `reconcile()` (item 4) was refactored
onto it so there's exactly one "read a cgroup stat file safely"
implementation. `test_trace.c`: 3/3 clean runs, trace records read back
independently of process state with correct monotonic seq, chunk_id, and
fault type.

**Item 4's victim selector is a placeholder, disclosed not hidden.**
`ensure_budget()` (`budget.c`) needs `policy->select_victim()`, which is
item 7's `lru`/`layer_order` and doesn't exist yet. Until then it picks the
lowest layer_id among RESIDENT, unpinned chunks -- a deterministic FIFO-ish
choice that exists only to make the reconcile/evict/infeasible *mechanism*
testable, not to produce a real replacement policy. `test_eviction.c`
verifies this mechanism exactly (predicted vs actual resident-chunk
membership through an 8-chunk/3-slot sequence, 3/3 clean runs), including a
punch-refetch round trip and I-7's reconcile() never tripping. Item 7 will
swap the selector for the real `policy_t` call; item 4's evict/reconcile
code underneath does not change.

## Layout

```
residctl/
  CLAUDE.md              this file
  docs/
    MECHANISM_SPEC.md     the contract
  src/                    implementation, one file group per build-order item
  results/                run manifests, csv/log artifacts, correctness-harness
                           output, sensitivity curves
  scratch/                throwaway; not tracked in git
```

## Git / GitHub

- Repo is committed under the user's own name/email (git config is set
  globally to match their GitHub account), pushed to their GitHub.
- Commit at meaningful checkpoints — roughly one commit per build-order item
  once it compiles and its own local checks pass, plus doc/spec commits when
  those change. Not a commit per file edit; not one giant commit for everything.
- Do not force-push. Do not rewrite history that's already pushed.

## Design constraints inherited from the spike (do not relitigate)

- **uffd must be `O_NONBLOCK`; `EAGAIN` is the only authoritative empty-queue
  signal**, not `poll()` readiness. Hung 5/5 spike runs before this was found.
  See I-2.
- **`memory.swap.max` must be `0`** or the kernel silently evicts weights the
  pager believes are resident (S3e measured 236 MiB swapped out per run when
  swap was left available). See I-3.
- **`UFFDIO_CONTINUE`, not `UFFDIO_COPY`**: COPY costs 48% more per fetch for a
  benefit (avoiding a double-buffer) that doesn't matter here. See §2.
- Fault *type* (MISSING vs MINOR) is not a valid dispatch key — both occur in
  normal operation. Dispatch on chunk *state*. See I-8.
- `MAP_SHARED|MAP_ANONYMOUS` allocations charge to `memory.stat[shmem]` (spike
  caught a 4096-byte step from exactly this). Any such allocation in the pager
  itself must be counted in the accounting's "known overhead" term.
