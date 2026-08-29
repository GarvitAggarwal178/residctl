# Phase 1 — Platform I/O Concurrency, Pager Removed

Standalone microbenchmark (`src/bench_concurrent_read.c`, no pager, no
uffd, no cgroup), asking directly: can concurrent `O_DIRECT` reads
overlap on this filesystem at all? Each thread reads a distinct,
non-overlapping, block-aligned range of `scratch/pattern_2g.bin`.
`drop_caches` before every trial. Grid and script: `src/run_phase1_sweep.sh`.

**Grid note, disclosed rather than silently padded:** the campaign's own
text states "64 configurations × 5 = 320 short runs," but the four listed
parameters (`--threads` {1,2,4,8}, `--block-size` {4MiB,128MiB},
{shared-fd,fd-per-thread}, {direct,buffered}) multiply to 4×2×2×2 = 32
configurations, not 64 — the arithmetic in the campaign text does not
match its own listed grid. Ran the full literal grid as specified: 32
configurations × n=5 = 160 runs.

## Machine exclusivity

**Before this phase:** a foreign workload (`cc1plus` compiling
`/root/souffle-master`, 98% CPU, load average 6.89) was running at the
start of this campaign session, during Phase 0. It was not killed, per
instruction — waited for it to exit naturally. Rechecked immediately
before Phase 1's sweep: clean (`ps aux` showed only routine
systemd/WSL/docker-desktop-proxy services, load average had decayed to
0.00-1.13, `pgrep -f` for known foreign-workload names clean).

**After Phase 1:** load average 1.62 (transient, from the sweep's own
just-finished CPU usage — 160 short trials in quick succession), no
foreign process, machine otherwise idle.

## Results

### Aggregate throughput (median of n=5, MiB/s)

| Block size | FD mode | I/O mode | 1 thread | 2 threads | 4 threads | 8 threads |
|---|---|---|---|---|---|---|
| 4 MiB | shared-fd | direct | 2094.3 | 3397.0 | 4238.8 | 4131.5 |
| 4 MiB | fd-per-thread | direct | 2595.5 | 3251.6 | 4133.2 | 4035.5 |
| 128 MiB | shared-fd | direct | 3165.9 | 3852.6 | 3835.8 | 3430.3 |
| 128 MiB | fd-per-thread | direct | 3279.2 | 4054.4 | 3767.1 | 3693.7 |
| 4 MiB | shared-fd | buffered | 10645.9 | 21649.1 | 20382.2 | 20324.6 |
| 4 MiB | fd-per-thread | buffered | 12620.6 | 21826.4 | 19908.2 | 20351.9 |
| 128 MiB | shared-fd | buffered | 7040.8 | 9710.2 | 11337.1 | 12752.9 |
| 128 MiB | fd-per-thread | buffered | 6807.3 | 9721.0 | 11331.8 | 12470.1 |

### Implied per-thread throughput (aggregate ÷ thread count), `direct` only

| Block size | FD mode | 1 thread | 2 threads | 4 threads | 8 threads |
|---|---|---|---|---|---|
| 4 MiB | shared-fd | 2094.3 | 1698.5 | 1059.7 | 516.4 |
| 4 MiB | fd-per-thread | 2595.5 | 1625.8 | 1033.3 | 504.4 |
| 128 MiB | shared-fd | 3165.9 | 1926.3 | 958.9 | 428.8 |
| 128 MiB | fd-per-thread | 3279.2 | 2027.2 | 941.8 | 461.7 |

## Pre-registered interpretation

**Neither "platform scales" nor "platform serialises" holds cleanly for
`O_DIRECT` — a third pattern, not one of the two pre-registered, is what
the data actually shows: partial scaling followed by a ceiling.**
Aggregate throughput rises materially from 1→2 threads (+62% at 4MiB/
shared-fd, +25% at 4MiB/fd-per-thread, +22% at 128MiB/shared-fd, +24% at
128MiB/fd-per-thread) — this is NOT "flat in thread count," ruling out a
clean "platform serialises" reading. But it does NOT continue rising
past 2-4 threads: at 8 threads every one of the four `direct`
configurations is flat-to-slightly-BELOW its 4-thread value (4238.8→4131.5;
4133.2→4035.5; 3835.8→3430.3; 3767.1→3693.7) — this is NOT "rises
materially with thread count" beyond that point, ruling out a clean
"platform scales" reading too. The implied per-thread throughput table
shows why: each doubling of thread count roughly HALVES per-thread
throughput (2094→1699→1060→516 MiB/s at 4MiB/shared-fd), consistent with
a shared aggregate ceiling around 3400-4300 MiB/s that gets divided
among however many threads are contending for it, not independent
per-thread channels. Reported as this specific pattern, not forced into
either pre-registered box.

**"Shared-fd only" (item 10e's specific hypothesis) does NOT hold.**
`fd-per-thread` does not meaningfully outperform `shared-fd` at any
thread count: at 8 threads, 4MiB block, `shared-fd`=4131.5 vs
`fd-per-thread`=4035.5 (fd-per-thread slightly LOWER); at 8 threads,
128MiB block, `shared-fd`=3430.3 vs `fd-per-thread`=3693.7 (fd-per-thread
+7.7%, the largest gap observed, still far from the large effect the
hypothesis would predict if a single shared fd were the actual
constraint). Giving each thread its own file descriptor does not lift
the ceiling. This directly tests and does not support the specific
mechanism item 10e's report offered as a plausible (not verified)
explanation for `residctl`'s own concurrently-outstanding=1.00 finding —
`r->model_fd` being shared is not, on this evidence, the dominant cause.

**`--direct` vs `--buffered`, and block size, reported regardless of
which case held (as instructed):**
- `buffered` throughput is 3-6× higher than `direct` at every matching
  configuration, and its absolute values (up to 21,826 MiB/s) substantially
  exceed even the spike's own measured maximum `O_DIRECT` bandwidth
  (3396 MiB/s, `MECHANISM_SPEC.md` §2) and the spike's own buffered-read
  measurement (128 ms median for 150 MiB ≈ 1172 MiB/s). This is the same
  signature item 10b's `DIAGNOSTIC_REPORT.md` already identified for arm
  A's V2 throughput (4785 MiB/s) — consistent with the buffered path being
  served by the Windows VHDX host cache, which guest-side `drop_caches`
  cannot reach (an explicitly accepted, not-to-be-defeated limitation per
  this campaign's own instructions). Not independently re-verified beyond
  noting the same signature recurs; buffered numbers in this table should
  not be read as genuine disk-level throughput.
- Block size: at `direct`, 128 MiB blocks show somewhat higher 1-thread
  throughput than 4 MiB blocks (3165.9-3279.2 vs 2094.3-2595.5 MiB/s) —
  consistent with per-`pread()` fixed overhead being amortized better at
  the larger block size, matching item 10b's Task A finding about
  per-fetch overhead. The scaling SHAPE (rise then ceiling) is the same at
  both block sizes.

## Correctness

Not applicable — this phase's binary reads a file and reports throughput;
no pager, no invariants (I-1..I-10) apply. Every trial exited rc=0 (160/160,
confirmed in `results/phase1_platform_io_log.txt`); no read errors, no
EOF-before-expected-bytes aborts.

## Final check

- No number estimated, inferred, or copied from documentation — every
  value in both tables is a direct read from
  `results/phase1_platform_io.csv` (median of n=5 real runs) or a
  disclosed arithmetic derivation (implied per-thread = aggregate ÷
  threads).
- No test modified (no existing test touched this phase).
- Pre-registered expectations checked against measured values: neither
  "scales" nor "serialises" held cleanly for `direct` — reported as the
  actual pattern (partial rise then ceiling), not forced into either box;
  the "shared-fd only" hypothesis was directly tested and did not hold;
  the direct/buffered and block-size contrasts were reported regardless
  of outcome, as instructed.
- No verification gate is defined for Phase 1; none was skipped.
