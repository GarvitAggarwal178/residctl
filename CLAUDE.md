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
| 5 | Trace recorder + metrics (§9) | 2h | not started |
| 6 | Trace-replay driver | 2h | not started |
| 7 | `lru` and `layer_order` policies (§8) | 2h | not started |
| 8 | Prefetch (§6.3) | 1h | not started |
| 9 | Belady solver (§10) | 2h | not started |
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
