# Campaign 12 Phase A — Arm A/B Baseline Repair

`PROJECT_STATE.md` attributed arm A's `read_bytes = 0` (11/12 Phase 3
cells, 5/5 Phase 4 cells) to the Windows VHDX host-cache limitation. That
attribution was wrong. This phase diagnoses the real cause, fixes it, adds
a guard against recurrence, and re-runs arms A and B at both phases'
grids.

## Diagnosis

### 1. `drop_caches` invocation, file:line

**`src/run_phase3_sweep.sh:70`**:
```sh
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 >> "$LOG"; }
```
Called at `src/run_phase3_sweep.sh:127`, immediately before every arm A
`run_row` call. Present, invoked every time. Exit status never checked
(no `$?` inspection anywhere in the script).

**`src/run_phase4_sweep.sh:69`**, identical definition. Called at
`src/run_phase4_sweep.sh:125` (arm A) and `:138` (arm B). Same: present,
invoked, exit status never checked.

So **the call is not absent** (rules out option (a)) — it runs before
every single arm A/B invocation in both phases.

### 2. Comparison against V2's script

**`src/run_item10_v2_harness.sh:69`**:
```sh
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a "$LOG"; }
```

The difference is exactly one character's worth of structure: V2 **pipes**
`echo`'s output to `tee -a "$LOG"` (`|`); Campaign 11's Phase 3/4 scripts
**redirect** it a second time to the same file descriptor
(`>> "$LOG"`, no pipe).

This is not cosmetic. In `echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a
"$LOG"` (V2), the pipe sets `echo`'s stdout to `tee`'s stdin *first*; the
explicit `> /proc/sys/vm/drop_caches` redirect on the same command then
**overrides** that pipe target for `echo` specifically — `echo`'s actual
output goes to the special file, `2>&1` duplicates stderr there too, and
`tee` (receiving nothing, since `echo`'s stdout was claimed by the
explicit redirect) writes nothing to the log. The write reaches the
kernel.

In `echo 3 > /proc/sys/vm/drop_caches 2>&1 >> "$LOG"` (Campaign 11 Phase
3/4), there is no pipe — both `>` and `>>` are redirects on the *same*
file descriptor (`fd1`, stdout) for the *same* command, applied
left-to-right. The **second** redirect (`>> "$LOG"`) silently overrides
the first: `echo`'s actual stdout destination, at the moment `echo`
executes, is `$LOG`, not `/proc/sys/vm/drop_caches`. `2>&1` (evaluated
between the two, per bash's left-to-right redirect processing) duplicates
stderr to whatever fd1 was *at that point* (the drop_caches file) — moot,
since `echo` never produces stderr. **The kernel interface is never
touched at all.**

### 3. Standalone verification that `drop_caches` still works on this machine

Isolated, direct test of the exact broken function (no baseline_main
involved):
```
before: buff/cache = 456 MiB
[call the broken function: echo 3 > /proc/sys/vm/drop_caches 2>&1 >> "$LOG"]
log file contents: "3\n" (2 bytes) -- confirms echo's actual output landed in $LOG, not the kernel file
after:  buff/cache = 456 MiB  (unchanged)
exit status: 0 (no error reported)
```

Isolated test of the fixed pattern (pipe to `tee`), on the same machine,
same session:
```
before:                  buff/cache = 413 MiB
after warming (cat the 2 GiB model file): buff/cache = 2333 MiB (+1920 MiB, file cached)
after drop_caches (fixed):                buff/cache = 189 MiB  (dropped well below baseline)
```

**`drop_caches` itself works correctly on this machine right now** — the
defect is entirely in the redirect syntax of the two Campaign 11 scripts.

### 4. Classification

**(c) the call fails silently.** Not absent (a) — it runs every time.
Not "undone before the measured section" (b) — it never touched the
target file to begin with; there was nothing to undo. Not some other
unidentified mechanism (d). The precise mechanism: a redirect-ordering
bug causes `echo`'s actual output to go to the log file instead of the
kernel interface, with exit status 0 and no error message anywhere —
silent in every practical sense.

**Scope check, not requested but relevant to `PROJECT_STATE.md` §6's
superseded-list accuracy:** grepped every other sweep script in this
project (`run_task_c_sweep1.sh`, `run_task_c_sweep2.sh`,
`run_task_d_sweep_b.sh`, `run_item10_harness.sh`) — all use the correct
pipe pattern, matching V2. **The bug is isolated to Campaign 11's Phase 3
and Phase 4 scripts specifically**, introduced there, not a project-wide
defect. Every arm A/B number from V2, item 10c, and item 10d stands
unaffected.

## The guard

Added to `src/baseline_main.c` (after the measured section, before the
summary print): reads `/proc/self/io:read_bytes` before and after the
touch loop (already being read for the report), and if `bytes_touched >
0` but the delta is exactly 0, aborts with `BASELINE FAILED` and a
message naming the cause (guest page cache almost certainly served
everything), returning exit code 1 rather than reporting a silent zero.

**Verified both directions, not merely written and trusted:**
- **Negative test**: warmed the guest cache (`cat` the model file), then
  ran `baseline_main` with NO `drop_caches` call at all →
  `BASELINE FAILED: bytes_touched=10737418240 but io_read_bytes delta is
  0 ...`, exit code 1. The guard fires.
- **Positive test**: same run, preceded by a correct (piped)
  `drop_caches` → `io_read_bytes_delta=10737455104`, wall 4.068s ≈ 2517
  MiB/s (within the spike's O_DIRECT ceiling), `PASS`, exit code 0. The
  guard does not false-positive on a genuine measurement.

## Re-run

Both re-run scripts (`src/run_phase3_AB_rerun.sh`,
`src/run_phase4_AB_rerun.sh`) use the corrected (piped) `drop_caches`
pattern and the guard-equipped `baseline_main`. Phase 3's grid kept the
single fixed `sequential` mode, as Phase 3 originally did (noted, not
changed). Phase 4's grid used the full 3-mode sweep + arm B, as Phase 4
originally did.

**Process note, disclosed:** the re-run needed real-time troubleshooting
beyond the diagnosis itself. `MADV_RANDOM` mode (arm A) is slow enough
(180-220s per rep, consistent with item 10c's own prior finding of
160-180s) that it was repeatedly squeezed to the very end of the
harness's background-task window and cut off just as it started,
producing no logged result at all (not a wrong result — simply lost, no
CSV row written, because the orchestrating shell that would have written
it was already gone). Diagnosed by direct process inspection
(`ps`/`ELAPSED`), not guessed. Fixed by running `MADV_RANDOM`'s 15 reps
individually in the foreground with a widened 220s timeout
(`src/run_one_random_rep.sh`), rather than end-to-end inside the
harness's backgrounded orchestrator. **Separately, a real self-inflicted
contamination was caught and corrected**: an early resume accidentally
launched a second, duplicate orchestrator instance concurrently with the
first (not noticed until both were found racing on the same cgroup via
`ps`) — the two competed for the same memory-limited cgroup, and 3 rows
(`ratio=0.25, arm=A, mode=random`, all 3 reps) came back with `rc=137`
(consistent with an OOM kill from the duplicate's own memory pressure,
not a genuine measurement). The duplicate was killed immediately (a
self-created duplicate process, not a third-party foreign workload — the
machine-exclusivity "don't kill it" rule governs contamination from
*other* work, not cleanup of one's own orchestration mistake), the 3
contaminated rows were deleted, and that cell was redone cleanly from a
verified single-instance state.

Even with the widened timeout and dedicated single-rep runs,
`MADV_RANDOM` still failed 8 of 15 reps with `rc=124` (genuine timeout,
not a bug) — `0.625` timed out on all 3 reps (0/3 success), `0.25`/`0.5`
on 2/3, `0.375` on 1/3, `0.75` succeeded on all 3. Consistent with, and
about as severe as, item 10c's own documented `MADV_RANDOM` coverage gap
at this workload scale. `MADV_RANDOM` has never won `best_mode` in any
report in this project's history and does not here either (see below) —
the gap does not affect which mode gets reported as the arm A baseline.

## Results

### Phase 3 (arm A, `sequential` mode only, matching Phase 3's original grid): bandwidth by (chunk size, ratio)

| Chunk size | Ratio | Median bandwidth (MiB/s) | Exceeds 3396 MiB/s? |
|---|---|---|---|
| 32 MiB | 0.25 | 3188.7 | No |
| 32 MiB | 0.50 | 2993.4 | No |
| 32 MiB | 0.75 | 3405.2 | **Yes** (+0.3%) |
| 64 MiB | 0.25 | 3383.2 | No |
| 64 MiB | 0.50 | 3313.6 | No |
| 64 MiB | 0.75 | 3370.8 | No |
| 128 MiB | 0.25 | 3489.9 | **Yes** (+2.8%) |
| 128 MiB | 0.50 | 3365.0 | No |
| 128 MiB | 0.75 | 2729.3 | No |
| 256 MiB | 0.25 | 3390.7 | No |
| 256 MiB | 0.50 | 3405.8 | **Yes** (+0.3%) |
| 256 MiB | 0.75 | 3135.3 | No |

### Phase 4 (arms A all 3 modes + B, 128 MiB chunks, matching Phase 4's original grid): bandwidth by (ratio, arm, mode)

| Ratio | Arm | Mode | Median bandwidth (MiB/s) | n | Exceeds 3396 MiB/s? |
|---|---|---|---|---|---|
| 0.25 | A | normal | 2137.3 | 3 | No |
| 0.25 | A | random | 51.1 | 1 | No |
| 0.25 | A | sequential | 2913.9 | 3 | No |
| 0.25 | B | normal | 2925.9 | 3 | No |
| 0.375 | A | normal | 2873.0 | 3 | No |
| 0.375 | A | random | 53.5 | 2 | No |
| 0.375 | A | sequential | 3652.9 | 3 | **Yes** (+7.6%) |
| 0.375 | B | normal | 3934.4 | 3 | **Yes** (+15.9%) |
| 0.5 | A | normal | 3062.9 | 3 | No |
| 0.5 | A | random | 50.3 | 1 | No |
| 0.5 | A | sequential | 3280.8 | 3 | No |
| 0.5 | B | normal | 3688.7 | 3 | **Yes** (+8.6%) |
| 0.625 | A | normal | 2966.1 | 3 | No |
| 0.625 | A | random | — | 0 | N/A (0/3 succeeded) |
| 0.625 | A | sequential | 3003.7 | 3 | No |
| 0.625 | B | normal | 3665.1 | 3 | **Yes** (+7.9%) |
| 0.75 | A | normal | 2832.2 | 3 | No |
| 0.75 | A | random | 49.8 | 3 | No |
| 0.75 | A | sequential | 2942.5 | 3 | No |
| 0.75 | B | normal | 3314.6 | 3 | No |

**7 of 32 measured cells exceed 3396 MiB/s, all by 0.3-15.9%** — a
different, much smaller signature than the old defect (which produced
either exactly 0 or, in V2's original contamination, 4785 MiB/s, 41%
over). All 7 over-ceiling cells are arm B or arm A `sequential`, never
arm A `normal`/`random`, and never by more than 16%. Plausible, not
independently confirmed: buffered reads can legitimately exceed
`O_DIRECT`'s median under favorable readahead (the spike's own buffered-
read measurement had a 6× spread, `MECHANISM_SPEC.md` §2), and arm B's
`MADV_WILLNEED` hint specifically requests aggressive readahead — a
mechanism distinct from an unreachable host cache. Flagged as
instructed, not resolved further.

`MADV_RANDOM`'s bandwidth (49.8-53.5 MiB/s) is 60-70× slower than
`sequential`/`normal` at every ratio measured — consistent with item
10c's finding and confirming `MADV_RANDOM` never wins `best_mode` here
either.

## Correctness

Not applicable this phase — `baseline_main` has no pager invariants
(I-1..I-10); the guard added is a new startup-style assertion specific to
this binary, verified directly above (negative and positive tests), not
by the §13 harness.

## Final check

- No number estimated, inferred, or copied from documentation — every
  bandwidth value is a direct computation (`io_read_bytes_delta / wall_ns`)
  over real re-run data in `results/campaign12_phaseA_phase3_AB.csv` and
  `results/campaign12_phaseA_phase4_AB.csv`; the `drop_caches` diagnosis
  is from direct `free -m` measurements and direct source inspection with
  file:line citations, not inferred.
- No test was weakened — the new guard makes `baseline_main` MORE strict
  (it now aborts on a case it used to silently report as `read_bytes=0`),
  and was verified to fire correctly in both directions before being
  trusted.
- The specific classification question (a/b/c/d) was answered with direct
  evidence (the isolated redirect test), not assumed from the fix working.
- Every cell exceeding 3396 MiB/s is reported explicitly in the table
  above, not buried; `MADV_RANDOM`'s coverage gap (8/15 reps) is disclosed
  with its exact cause (timeout, not the mechanism under test), matching
  item 10c's own precedent for the same submode at the same workload
  scale.
