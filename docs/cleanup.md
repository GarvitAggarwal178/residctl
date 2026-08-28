# CLEANUP SESSION — Fix the one real bug, reconcile the docs, write the novelty argument

Work in `/root/residctl/`. Read `results/final/FINAL_SUMMARY.md`,
`results/final/BLOCKERS.md`, `results/final/WRITEUP_PACKAGE.md`, and
`results/overnight/CLAIMS.md` first.

**§0 non-negotiable rules apply in full.**

This is a cleanup session, not a measurement campaign. Three tasks. Nothing here
opens a new line of investigation. Estimated total: 3-4 hours.

Same session rules: run every phase, write each phase's file as it completes,
commit after each, push at the end, check machine exclusivity before and after
any phase that runs code.

---

## PHASE 1 — Fix the arm E deadlock · box: 2h

### What is broken

`BLOCKERS.md` FINDING 1. Phase 3 of the final session characterised it precisely
but did not fix it, because that session's spec capped Phase 3 at applying the
pre-decided mitigation.

**Signature**, from four independent SIGUSR1 dumps at the hang with
`protect_current=on`:

- `resident_bytes` 148-403 MiB against a **526 MiB budget** — the budget is not
  binding.
- 2-3 chunks RESIDENT and unpinned — victims are available.
- `stat_infeasible = 0`, `stat_pin_broken = 0` — neither escape path fired.
- **One chunk stuck in `FETCHING` with every fetch worker parked in
  `futex_do_wait`.**
- Main thread blocked forever in `handle_userfault`.

**Mechanism:** a demand fault dedupes against an in-flight prefetch and waits on
a `FETCHING` slot that no worker owns. The chunk never completes; the fault never
resolves; the process hangs.

This is a **7th concurrency-class issue**, of the same family as the six already
consolidated in `CLAUDE.md` — shared mutable state reached from a path that
became live when the architecture changed.

### Diagnose before fixing

Reproduce the hang under the *original* triggering configuration
(`--protect-current on --prefetch-retention pinned --prefetch-depth 2` at
r=0.25 on the real model). Then answer, with evidence from the trace and from
source:

1. **How does a chunk reach `FETCHING` with no worker owning it?** Candidates to
   check in `prefetch_pool.c` and `pager.c`, in this order:
   - `do_one_prefetch` sets `target->state = CHUNK_FETCHING` and then returns
     early on some path without resetting it to `ABSENT`. Check every early
     return.
   - A prefetch job is dropped from the queue (queue full, `pf_tracked` race,
     pool stop) after the state was already set.
   - The demand dispatcher enqueues, the worker takes it, hits the bounded
     `ensure_budget` retry loop, exhausts it, and returns without resetting
     state or waking waiters.
2. **Why does the deduped demand fault never get woken?** `handle_fault`'s
   `FETCHING` branch drops the message on the assumption that the in-flight
   fetch's `UFFDIO_CONTINUE` will wake it. If the fetch never completes, nothing
   ever does. Confirm this is the waiting path.

Report the exact file:line of the state transition that is not paired with a
reset, or state that you could not find one.

### The fix

**Pre-decided, do not evaluate alternatives.** Two parts, both required:

**(a) Every path that sets `CHUNK_FETCHING` must reset it.** Audit every
transition into `FETCHING` in `pager.c` and `prefetch_pool.c`. Every early
return, error path, and dropped-job path between setting `FETCHING` and reaching
`RESIDENT` must set the state back to `ABSENT` **and** issue `UFFDIO_WAKE` over
the chunk range so any deduped waiter refaults and retries. Add an assertion that
a chunk cannot leave a worker function in `FETCHING` state.

**(b) A watchdog on the `FETCHING` state.** Record a timestamp when a chunk
enters `FETCHING`. In the handler's dispatch loop (which never blocks, per A-5),
check for any chunk in `FETCHING` for longer than a configurable timeout
(default 30 s, `--fetching-timeout-ms`). On expiry: reset to `ABSENT`, wake
waiters, increment a new `stat_fetching_timeout` counter, and log the chunk id.

Part (a) fixes the cause. Part (b) makes the failure mode recoverable rather than
fatal even if another such path exists — which matters, because six of these have
been found already and a seventh has just been found.

**Do not** add a third mechanism, change `select_victim`, change any policy, or
alter arm definitions.

### Verification

1. **The original trigger no longer hangs.** `--protect-current on
   --prefetch-retention pinned --prefetch-depth 2` at r=0.25, n=5, real model.
   All five must complete. Report `stat_fetching_timeout` — **if it is non-zero,
   part (a) missed a path**; report which chunk and investigate that path
   specifically.
2. **T-1..T-7 with `--eager-reconcile`.** All must pass. T-7 (no lost fault) is
   the relevant one. A failure is a hard stop.
3. **No regression on the shipping default.** Re-run the final session's Phase 1
   grid, arm D only, r ∈ {0.25, 0.5, 0.75}, n=3, and confirm `read_bytes` matches
   `phase1_equal_budget.csv` within normal variance.
4. **Arm E re-measured at r ≤ 0.375** with the fix and the default config, n=3.
   Report whether prefetch is now merely non-advantageous (expected) or actually
   competitive (would be a new result — report it, do not chase it).

**Write `results/cleanup/phase1_deadlock_fix.md`.** The verdict must state
whether the trigger still hangs and whether `stat_fetching_timeout` fired.

---

## PHASE 2 — Reconcile CLAIMS.md · box: 45 min · documentation only

`results/overnight/CLAIMS.md` carries final-session amendments at the top, but
**the claim bodies below are stale and contradict them.** Someone writing the
report reads the body, not the header.

Known stale content — verify each and fix:

- **Claim 1, Caveats:** says "WP2 would have confirmed on a real model; not run —
  BLOCKER 1." WP2 ran and confirmed it. Replace with the real-model evidence.
- **Claim 6, Caveats:** says the real-compute test "was not run (BLOCKER 1)." It
  ran, and contradicted the synthetic finding.
- **Claim 8, Caveat (2):** still asserts "Arm D ≈ arm A at r=0.25." Final-session
  Phase 1 superseded that at equal budget.
- **Claim 7, Caveats (2) and (3):** reference the arm E collapse and the untried
  driver-side signal. Both are now resolved — update or delete.
- **Claim 2 and Claim 4:** their bodies quote the pre-Phase-2 D/OPT numbers.

**Task:** rewrite each claim body so it is correct as it stands, without needing
the amendment header. Then delete the amendment header — its content should now
live in the bodies. Keep the `Superseded prior claims` field in each entry; that
is the audit trail and must survive.

Add **Claim 10** if Phase 1's fix succeeds:

> The prefetch retention and current-chunk protection mechanisms, each correct in
> isolation, combined into a deadlock; it was found by instrumentation, diagnosed
> from thread state, and fixed by pairing every `FETCHING` transition with a
> reset and adding a watchdog.

Then re-check `WRITEUP_PACKAGE.md` §1 and §3 against the reconciled claims and
fix any number that moved.

**Write `results/cleanup/phase2_claims_reconciled.md`** listing every edit made.

---

## PHASE 3 — The related-work and novelty section · box: 60 min · writing only

The novelty argument currently exists implicitly across eight reports. The report
needs it as one section that can be pointed at.

Write `results/cleanup/RELATED_WORK.md`. No new research — use the literature
already cited in the proposal and in `MECHANISM_SPEC.md`, plus the prior-art
findings recorded in `CLAUDE.md` from the original kill-check.

Structure:

**§1 — Application-controlled memory management (the idea).** Mach external
pagers (Young et al., 1987), Appel & Li (ASPLOS 1991), Harty & Cheriton (ASPLOS
1992), exokernel (Engler et al., SOSP 1995). State the position they argued and
that it was accepted in principle. State plainly that the idea is not ours.

**§2 — What Linux actually shipped, and why it is insufficient.** `madvise`,
`posix_fadvise`, `mlock`. Advisory in one direction, binding only for retention.
`cache_ext` (2025) measured that the hints are weak levers. `MADV_PAGEOUT`
returns `EINVAL` on `mlock`'d ranges. `mlock` cannot express eviction and cannot
oversubscribe.

**§3 — Contemporary approaches, and where each sits.** Reproduce the positioning
table from the proposal, extended: classical kernel replacement; programmable
kernel eviction (`cache_ext`, `sched_ext` as the scheduling analogue); DAMON;
framework-level model memory management (vLLM/PagedAttention, FlexGen, Pie);
weight-streaming work (LLM in a Flash, P2Cache, ssd-llm). For each: where control
resides, whether it is binding, and why it does not cover this project's position.

**§4 — `userfaultfd`'s established uses, and the gap.** Post-copy live migration,
CRIU lazy restore. Both resolve a fault from a *remote or snapshot* source. State
that it has not been reported as a residency-control mechanism for a large
read-only working set, and cite the January 2026 `vm_uffd_ops` RFC as evidence
the interface is under active development and currently limited to anonymous,
shmem and hugetlb.

**§5 — The contribution, stated precisely.** Four sentences, no more:

1. The mechanism composition: `userfaultfd` over a double-mapped `memfd`, with
   `FALLOC_FL_PUNCH_HOLE` eviction under `memory.swap.max = 0`, giving residency
   control the kernel cannot override — demonstrated causally (the kernel enters
   direct reclaim 37 times and scans zero pages).
2. The evaluation against a computable optimum, which this workload uniquely
   permits because its reference string is known in advance.
3. The declared-order policy reaching that optimum exactly (D/OPT = 1.000),
   which is the concrete form of the claim that application knowledge beats
   inference.
4. The end-to-end validation: byte-identical output from a real model, 2.1×
   throughput scaling where the kernel gives none.

**§6 — What we do not claim.** Explicitly: we did not invent
application-controlled memory management; we did not invent `userfaultfd`; we do
not claim generality beyond one dense 3B model on one platform; and the
prefetching component does not pay on real inference. Naming these is what makes
§5 credible.

Keep the whole thing under 1200 words. It is a section, not a survey.

---

## WHAT NOT TO DO

- Do not run a new measurement sweep beyond Phase 1's verification.
- Do not change `select_victim`, any policy, arm definitions, the
  reference-trace format, or T-1..T-7.
- Do not attempt bare metal or a second model.
- Do not add a prefetch mechanism, retention variant, or admission rule.
- Do not modify anything under `/root/spike/`.

## FINAL

Write `results/cleanup/CLEANUP_SUMMARY.md`: what was fixed, what the verification
showed, every document edited, and anything that remains open.

Amend `results/PROJECT_STATE.md` §3 (the arm E limitation — update it if the
deadlock is fixed), §4 (add the 7th concurrency bug to the index), and §5 (add a
spec amendment for the `FETCHING` watchdog if one is warranted).

Each phase file confirms: no number estimated or inferred; no test weakened to
pass; every verification checked against a measured value.