# OVERNIGHT RUNBOOK — residctl

Place this and the three `WP*.md` files in `/root/residctl/docs/overnight/`.

Start the session with exactly:

> Read `docs/overnight/RUNBOOK.md` and follow it end to end. Work through the
> work packages in order. Do not stop to ask me anything — every decision you
> would ask about has a pre-decided answer in these files. If something is truly
> undecidable, record it in the blocker log and move to the next work package.

---

## HUMAN PRE-FLIGHT — do these before starting the session

1. **Push everything.** `git push origin main`. Confirm clean tree.
2. **Tag the current state:** `git tag pre-overnight && git push origin pre-overnight`.
   Everything tonight is recoverable to this point.
3. **Stop all other workloads on this machine.** No CN project, no `iperf3`, no
   builds. Confirm with `pgrep -af "cn-spike|iperf3|gate5"`.
4. **Place a GGUF model file** at `/root/residctl/models/model.gguf`.
   - Target size **1.5-3 GiB**. Larger than that will not oversubscribe cleanly
     on 7 GiB of RAM; smaller makes the budget ratios uninteresting.
   - A 3B-parameter model at Q4, or a 1B at Q8, is about right.
   - **The agent cannot download this** — the sandbox's allowed domains do not
     include model hosts. If this file is absent, WP2 will stop at its first
     gate and the night's most valuable work will not happen.
   - Verify: `ls -la /root/residctl/models/model.gguf`.
5. **Confirm disk headroom:** at least 40 GiB free on `/dev/sdd`.

---

## SESSION RULES

These override anything in the individual work packages.

1. **§0 non-negotiable rules apply throughout**: no fabricated numbers, no
   weakening a test to make it pass, no concluding success from absence of a
   crash, no silently fixing the environment, report contradictions rather than
   resolving them.

2. **Work packages run in order: WP1, then WP2, then WP3.** Do not reorder. WP1
   is short and near-certain; WP2 is long and risky. If the night ends early, the
   certain work is done.

3. **Never ask for a decision.** Every fork you would raise has a pre-decided
   answer in the work package. If you hit one that genuinely does not, write it
   to `results/overnight/BLOCKERS.md` with full context and move to the next
   work package. Do not idle.

4. **Commit after every completed phase**, with a message naming the phase.
   Push at the end of each work package. A crash at 4am must not lose six hours.

5. **Write each phase's report file as that phase completes**, not batched at the
   end.

6. **Machine exclusivity** checked at the start of each work package and at the
   end. If a foreign workload appears, record it, wait up to 10 minutes, and if
   it persists, note the contamination on every affected measurement and
   continue. Do not kill anything.

7. **Time-box discipline.** Each phase has a box. If a phase exceeds **2× its
   box**, stop that phase, record what was completed and what was not, and move
   to the next phase. Do not let one phase consume the night.

8. **If the background-task cap interrupts a sweep**, resume from a verified
   point against the existing CSV row count. Never silently restart.

9. **Do not modify** T-1..T-7, the reference-trace format, arm definitions, the
   spike results, or anything under `/root/spike/`.

10. **Do not attempt bare metal.** Do not modify `.wslconfig`.

---

## HARD STOP CONDITIONS

Stop the current work package, record fully, and move to the next, if any of
these occur:

- **T-7 fails** (a lost fault) at any point. This is a correctness regression and
  nothing measured after it is trustworthy.
- **OPT falls below the cyclic floor** at any configuration.
- **`reconcile()` reports divergence** beyond one chunk outside a known-transient
  window.
- **A build fails and is not fixed within 30 minutes.**
- Disk free falls below 10 GiB.

Record every hard stop in `results/overnight/BLOCKERS.md` with the exact command,
output, and what was completed before it.

---

## WORK PACKAGES

| WP | File | Box | Why it is in this order |
|---|---|---|---|
| 1 | `WP1_DECLARED_ORDER.md` | 3h | Short, near-certain, and fixes a thesis-alignment defect. Also produces a new experimental result (learned vs declared order) rather than only a fix. |
| 2 | `WP2_LLAMACPP.md` | 6h | The single biggest gap in the project — every finding to date is validated only against a synthetic cyclic scan. Risky and long, so it runs second. |
| 3 | `WP3_FIGURES.md` | 2h | Analysis and figure generation from data already on disk. Zero risk, and it is what the writeup actually needs. Runs last because it depends on WP1/WP2 output where available, but works without them. |

**WP3 must run even if WP1 and WP2 both fail.** It uses existing data. If the
night goes badly, WP3 is what makes the writeup possible.

---

## END OF SESSION

Write `results/overnight/OVERNIGHT_SUMMARY.md`:

```
# Overnight Session Summary

## What completed
Per work package, per phase: completed / partial / not started, with the
report file for each.

## What did not, and why
Include every hard stop, every time-box overrun, every blocker.

## New results, one line each
With the file and cell each came from.

## What changed in the code
Every file touched, and whether the change is a fix, an instrument, or a
new feature.

## Numbers that are now superseded
Anything tonight invalidated, for PROJECT_STATE.md §6.

## What the next session should do first
Ordered list, with reasons. No more than five items.
```

Then amend `results/PROJECT_STATE.md` §1, §2, §3, §5 and §6 with tonight's
results, following the structure already there. Do not rewrite sections tonight
did not touch.

Final commit and push.