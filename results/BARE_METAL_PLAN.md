# Bare-Metal Readiness Package

Prepared per Campaign 13 Phase D. **Not attempted this session** —
dual-booting is a human decision outside this session's scope, per
instruction. This document is everything needed to run the bare-metal
session as one command plus a short manual checklist, without re-reading
any other report.

## Why this is needed

Campaign 12 Phase D found 11 of 20 arm A/B cells exceed the spike's
measured 3396 MiB/s `O_DIRECT` ceiling — the Windows VHDX host-side cache
signature, unreachable from inside the WSL2 guest and explicitly out of
scope to defeat (every report in this project's history, starting item
10b). The D-vs-A comparison — **the project's central claim** — cannot be
made reliably at those contaminated cells. Campaign 13 Phase C found 8
clean cells (spanning every ratio, never contaminated in both chunk
sizes at once) where D beats A with no exceptions, but a genuinely clean
platform would let every cell be trusted, not just 8 of 20.

## 1. The minimum sweep

**Reproduces Campaign 12 Phase D's exact grid**, with one deliberate
simplification (arm A single mode, not the full 3-mode sweep) to control
time and avoid `MADV_RANDOM`'s historically slow tail (160-220s/rep on
WSL2 — see Campaign 12 Phase A/D's own disclosed handling of this).
Reproducing the exact same cells, not a reduced subset, is deliberate:
it lets every one of Phase C's 5 points be independently re-checked on
bare metal, not just the D-vs-A claim.

- **Region**: 2 GiB, fixed (matches every prior report's scale).
- **Chunk sizes**: {8 MiB, 128 MiB} — both. 8 MiB is Campaign 12 Phase
  B's measured aggregate byte floor for arm D; 128 MiB is the
  historically comparable scale used by nearly every report back to V2.
  Testing both lets Phase C point 5's chunk-size divergence (bytes favor
  small chunks, wall-clock favors large ones) be independently verified
  on real hardware, where the WSL2 virtio layer's own contribution to
  that divergence — if any — would disappear.
- **Ratios**: {0.25, 0.375, 0.5, 0.625, 0.75} — the full 5-point grid,
  matching Campaign 12 Phase D exactly (needed so every ratio has a
  directly comparable bare-metal cell, per Phase C point 1's finding
  that clean cells exist at every ratio but never both chunk sizes at
  once).
- **Compute settings**: {0, 400000} — both. Compute=400000 is exactly
  where Campaign 13 Phase A found arm D's fault-dispatch-order
  non-determinism; re-running it on bare metal (different scheduler,
  different real timing) is itself informative about whether that
  finding is WSL2-specific or general.
- **Arms**: A (single `sequential` madvise mode — Phase 3's own
  established simplification, avoids `MADV_RANDOM` entirely), B
  (hints on), C (`lru`), D (`layer_order`, prefetch off), E
  (`layer_order`, prefetch on, `--prefetch-depth 2 --prefetch-retention
  pinned`) — the same 5 arms as every paper-table report.
- **n=3** per cell, matching this project's standard.

**Total runs**: `(A: 2×5×3=30) + (B: 2×5×3=30) + (C/D/E: 2×5×2×3×3=180)`
= **240 runs.**

### Time budget

| Component | Estimate |
|---|---|
| Build (`make clean && make all`) | 1-2 min |
| S0-S3 pre-flight (see §3) | 15-25 min |
| T-1..T-7 correctness harness | 5-10 min |
| 240-run sweep, ~12s/run average (WSL2's own per-run times, 2.4-10.2s, padded for cgroup/drop_caches overhead; bare metal should be faster or equal, never slower, on the same disk/CPU per the stated assumption) | 45-50 min |
| Margin for anything slower than estimated (padding, retries) | up to ~90 min |
| **Total** | **well under 3 hours even with generous padding** |

If time runs short, drop the 8 MiB chunk size first (halves the sweep to
120 runs) — the 128 MiB grid alone still fully settles the central D-vs-A
claim and points 1-4 of Phase C; only point 5 (chunk-size divergence)
would be lost.

## 2. Environment values to record

Extends `spike/src/env_baseline.sh`'s own pattern (already used once for
this project's original WSL2 baseline) with the additional fields this
comparison specifically needs. Run this **before** the sweep and save the
output — it is the single most important artifact for judging whether any
difference from WSL2 is platform-real or an artifact of a different
environment.

```bash
#!/bin/bash
# bare_metal_env_baseline.sh -- run once, before anything else.
MODEL_PATH="$1"   # path to the model file the sweep will read from
{
echo "=== uname -r ==="
uname -r
echo
echo "=== nproc ==="
nproc
echo
echo "=== free -m ==="
free -m
echo
echo "=== mount (root fs) ==="
mount | grep " / "
echo
echo "=== filesystem of model file ==="
df -T "$MODEL_PATH"
echo
echo "=== cgroup version ==="
stat -fc %T /sys/fs/cgroup/    # "cgroup2fs" = v2 (required); "tmpfs" = v1
echo
echo "=== transparent_hugepage/enabled ==="
cat /sys/kernel/mm/transparent_hugepage/enabled
echo
echo "=== transparent_hugepage/shmem_enabled ==="
cat /sys/kernel/mm/transparent_hugepage/shmem_enabled
echo
echo "=== unprivileged_userfaultfd ==="
cat /proc/sys/vm/unprivileged_userfaultfd 2>&1
echo
echo "=== swaps ==="
cat /proc/swaps
echo

# Resolve the block device backing the model file's filesystem.
DEV=$(df "$MODEL_PATH" | tail -1 | awk '{print $1}')
DEVNAME=$(basename "$DEV" | sed 's/[0-9]*$//')  # strip partition number, e.g. sda1 -> sda

echo "=== device model ($DEVNAME) ==="
cat "/sys/block/$DEVNAME/device/model" 2>&1
echo
echo "=== scheduler ($DEVNAME) ==="
cat "/sys/block/$DEVNAME/queue/scheduler" 2>&1
echo
echo "=== read_ahead_kb ($DEVNAME) ==="
cat "/sys/block/$DEVNAME/queue/read_ahead_kb" 2>&1
echo
echo "=== logical_block_size ($DEVNAME) ==="
cat "/sys/block/$DEVNAME/queue/logical_block_size" 2>&1
echo
echo "=== rotational (0=SSD/NVMe, 1=spinning) ($DEVNAME) ==="
cat "/sys/block/$DEVNAME/queue/rotational" 2>&1
} | tee bare_metal_env_baseline.txt
```

**`memory.swap.max`** cannot be read at the root cgroup (cgroup v2's root
cgroup has no such file — confirmed directly: WSL2's own root cgroup
gives "No such file or directory" for this). Check it at the SPECIFIC
cgroup the sweep script creates instead — the sweep script below reads
and asserts `memory.swap.max == 0` itself at every cgroup it creates
(matching I-3, this project's own invariant), so no separate manual check
is needed once the script is running; to check it standalone first:
`mkdir -p /sys/fs/cgroup/residctl_test && echo 0 >
/sys/fs/cgroup/residctl_test/memory.swap.max && cat
/sys/fs/cgroup/residctl_test/memory.swap.max`.

## 3. Pre-flight checklist — spike tests S0-S3

Re-run before the sweep, in order. Source: `/root/spike/src/`. All four
scripts already exist in this repo's sibling `spike/` directory; assume
the bare-metal checkout includes it (`spike/src/*.c`, same as this
machine).

| Test | Program | What it confirms | Pass criteria |
|---|---|---|---|
| S0 | `s0_uffd_probe` | `userfaultfd` is available and both required feature bits are supported | `UFFDIO_API` succeeds; `UFFD_FEATURE_MISSING_SHMEM` and `UFFD_FEATURE_MINOR_SHMEM` both present in the returned bitmask; a memfd+mmap+`UFFDIO_REGISTER` with `MODE_MISSING\|MODE_MINOR` succeeds |
| S1 | `s1_bench` (via `run_s1.sh`) | Real fetch bandwidth on THIS machine's disk — **this re-establishes the `O_DIRECT` ceiling number itself**, since the whole point of bare metal is that this number may differ from WSL2's 3396 MiB/s (no host-level cache confound should exist here at all) | No fixed pass/fail threshold — record the new median/min/max `O_DIRECT` `pread()` bandwidth (WSL2 path 2 in the original spike) as the new ceiling reference for THIS platform's arm A/B bandwidth-exceeds-ceiling check |
| S2 | `s2_range_resolution` (via `run_s2.sh`) | A single `UFFDIO_CONTINUE` resolves the full requested range; no further faults for in-range reads afterward | `mapped` field exactly equals the requested range length (157,286,400 bytes at the spike's own test size); `poll()` after resolution times out with zero further uffd messages for in-range reads, 5/5 runs |
| S3 | `s3_eviction` (via `s3_setup_cgroup.sh` then the binary) | Hole-punch frees memory immediately (S3a), the punched range re-faults as MISSING not MINOR (S3b), the kernel does not reclaim the region under memory pressure without cooperation (S3c) | S3a: `memory.stat[shmem]` drop matches the punched size within ±5%; S3b: a uffd message arrives for a 1-byte read in the punched range with `MISSING` flag (not `MINOR`); S3c: `memory.stat[shmem]` for the never-punched region stays at its pre-pressure value under a memory-pressure hog, `memory.events[oom]` and `oom_kill` stay 0 (or are explained, not silently accepted, if nonzero) |

**If any of S0-S3 fails or gives a qualitatively different result than
the original WSL2 spike, stop and report before running the sweep** — a
failure here means the mechanism's own preconditions don't hold on this
platform, and every downstream number would be measuring something
different from what this project has measured throughout.

Additionally, per this campaign's own Correctness section: **re-run
T-1..T-7 after the build**, exactly as every phase in this project does
(`src/run_correctness_harness.sh` then `src/run_t6_t7.sh`, both already
in the repo). All must PASS before the sweep begins.

## 4. WSL2 numbers to compare against

### Environment (this machine, for reference)

| Field | WSL2 value |
|---|---|
| Kernel | `6.18.33.2-microsoft-standard-WSL2` |
| `nproc` | 16 |
| Root filesystem | ext4, `/dev/sdd` |
| cgroup version | cgroup2fs (v2) |
| `transparent_hugepage/enabled` | `always [madvise] never` |
| `transparent_hugepage/shmem_enabled` | `always within_size advise [never] deny force` |
| Device model | "Virtual Disk" (WSL2's virtio-backed VHDX, not a real device model) |
| Scheduler | `[none] mq-deadline kyber` |
| `read_ahead_kb` | 8192 |
| `logical_block_size` | 512 |

### Performance ceiling

- Spike S1's max measured `O_DIRECT` `pread()` bandwidth: **3396 MiB/s**
  (median 2450 MiB/s) — used as this project's "exceeds ceiling = host-
  cache signature" threshold throughout. **On bare metal, re-derive this
  number fresh from the new S1 run (§3) rather than reusing 3396 — a
  different, uncontended real disk may have a genuinely different
  ceiling, and the whole exercise only works if arm A/B are compared
  against THIS platform's own real maximum, not WSL2's.**
- `UFFDIO_CONTINUE`, 150 MiB: 2.9 ms median (50,900 MiB/s), ~4.5% of
  fetch time.

### Central claim — D vs A on bytes (Campaign 13 Phase C, clean cells only)

| Chunk | Ratio | Compute | A bytes | D bytes |
|---|---|---|---|---|
| 8MiB | 0.25 | 0 | 10,821,021,696 | 9,193,914,368 |
| 8MiB | 0.375 | 0 | 10,737,455,104 | 8,338,276,352 |
| 8MiB | 0.5 | 0 | 10,743,746,560 | 7,549,747,200 |
| 8MiB | 0.5 | 400000 | 10,743,746,560 | 9,219,080,192 |
| 8MiB | 0.625 | 0 | 10,737,389,568 | 6,727,663,616 |
| 8MiB | 0.625 | 400000 | 10,737,389,568 | 7,600,078,848 |
| 128MiB | 0.75 | 0 | 10,737,455,104 | 5,502,926,848 |
| 128MiB | 0.75 | 400000 | 10,737,455,104 | 6,710,886,400 |

D beat A in all 8 of 8 WSL2 clean comparisons. On bare metal, EVERY cell
should be clean (no host cache to contaminate arm A/B at all) — compare
D-vs-A at all 20 (chunk,ratio,compute) cells, not just these 8.

### OPT per (chunk, ratio) — should reproduce near-exactly (same reference pattern)

| Ratio | OPT (8 MiB) | OPT (128 MiB) |
|---|---|---|
| 0.25 | 8,589,934,592 | 8,724,152,320 |
| 0.375 | 7,516,192,768 | 7,516,192,768 |
| 0.5 | 6,442,450,944 | 6,442,450,944 |
| 0.625 | 5,368,709,120 | 5,368,709,120 |
| 0.75 | 4,294,967,296 | 4,294,967,296 |

### Other WSL2 headline numbers to check against

- C's miss rate: 1.000 (100%) in all 20 WSL2 cells — should reproduce
  exactly (deterministic, `lru` thrashes by construction).
- D/OPT range (non-degenerate WSL2 cells): 1.070-1.775.
- E beats D on bytes at 8 MiB in 5 of 8 cells (including 3 at
  compute=0 — never seen at 128 MiB); at 128 MiB, D wins 6 of 7
  non-degenerate cells.
- Arm D's fault-dispatch-order non-determinism (Campaign 13 Phase A):
  reproduced at `128MiB/r=0.5/compute=400000` and worse elsewhere in the
  grid (`128MiB/r=0.25` and `8MiB/r=0.25`, both `compute=400000`, where
  D exceeded arm C's byte count). If bare metal's scheduler behaves
  differently under `--driver-threads 8 --lookahead-window 1
  --compute-ns-per-mib 400000`, this pattern may not reproduce
  identically — worth explicit note either way, not assumed.

## 5. Build/run script

Assumes the bare-metal machine has this repository checked out at
`~/residctl` (or wherever — adjust `RESIDCTL` below) with `spike/` as a
sibling directory, a fresh Ubuntu install, gcc/make available, and root
or `sudo` access for cgroup v2 setup. **Single command to run the whole
session**: `bash bare_metal_session.sh`.

```bash
#!/bin/bash
# bare_metal_session.sh -- Campaign 13 Phase D bare-metal readiness
# script. Runs build, S0-S3 pre-flight, T-1..T-7, environment capture,
# and the full 240-run sweep, unattended, writing one CSV. Designed to
# be interrupted and resumed (per-rep resumability, same pattern this
# project's every prior sweep script uses).
set -u
RESIDCTL="${RESIDCTL:-$HOME/residctl}"
SPIKE="${SPIKE:-$HOME/spike}"
SRC="$RESIDCTL/src"
RESULTS="$RESIDCTL/results"
SCRATCH="$RESIDCTL/scratch"
MODEL="$SCRATCH/pattern_2g.bin"
CGROUP=/sys/fs/cgroup/residctl_baremetal
CSV="$RESULTS/bare_metal_sweep.csv"
LOG="$RESULTS/bare_metal_sweep_log.txt"
mkdir -p "$RESULTS" "$SCRATCH"

echo "=== Step 1: build ==="
( cd "$SRC" && make clean && make all ) || { echo "BUILD FAILED"; exit 1; }

echo "=== Step 2: environment baseline ==="
[ -f "$MODEL" ] || "$SRC/gen_pattern" "$MODEL" 2147483648
bash "$(dirname "$0")/bare_metal_env_baseline.sh" "$MODEL"

echo "=== Step 3: S0-S3 pre-flight ==="
( cd "$SPIKE/src" && gcc -O2 -o s0_uffd_probe s0_uffd_probe.c -pthread \
    && ./s0_uffd_probe ) || { echo "S0 FAILED -- stop and report"; exit 1; }
( cd "$SPIKE/src" && bash run_s1.sh ) || { echo "S1 FAILED -- stop and report"; exit 1; }
( cd "$SPIKE/src" && bash run_s2.sh ) || { echo "S2 FAILED -- stop and report"; exit 1; }
( cd "$SPIKE/src" && bash s3_setup_cgroup.sh && ./s3_eviction ) || { echo "S3 FAILED -- stop and report"; exit 1; }
echo "S0-S3 passed -- see $SPIKE/results/ for full logs. Proceeding."

echo "=== Step 4: T-1..T-7 correctness harness ==="
bash "$SRC/run_correctness_harness.sh" || { echo "T-1..T-5 FAILED -- stop and report"; exit 1; }
bash "$SRC/run_t6_t7.sh" || { echo "T-6/T-7 FAILED -- stop and report"; exit 1; }
grep -q "RESULT: PASS" "$RESULTS/correctness_harness_log.txt" || { echo "T-1..T-5 did not PASS"; exit 1; }
grep -q "RESULT: PASS" "$RESULTS/t6_t7_log.txt" || { echo "T-6/T-7 did not PASS"; exit 1; }
echo "T-1..T-7 all PASS. Proceeding to sweep."

echo "=== Step 5: machine exclusivity check ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"

REGION_LEN=2147483648
N_PASSES=5
MARGIN=67108864
CHUNK_SIZES="8388608 134217728"
RATIOS="0.25 0.375 0.5 0.625 0.75"
COMPUTES="0 400000"

log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$CSV" ]; then
    > "$LOG"; > "$CSV"
    echo "chunk_size,ratio,arm,detail,compute,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta,dedup_resident,dedup_fetching,pin_broken,rc" > "$CSV"
    log "=== fresh start ==="
else
    log "=== resuming: existing CSV has $(wc -l < "$CSV") lines ==="
fi

fresh_cgroup() {
    local max=$1
    if [ -d "$CGROUP" ]; then
        local procs; procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
        if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
        rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo "$max" > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
    [ "$(cat "$CGROUP/memory.swap.max")" = "0" ] || { echo "FATAL: memory.swap.max not 0 (I-3 violated)"; exit 1; }
}
cleanup_cgroup() {
    local procs; procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
}
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a "$LOG"; }

rep_done() {
    local cs=$1 ratio=$2 arm=$3 detail=$4 compute=$5 rep=$6
    awk -F, -v cs="$cs" -v r="$ratio" -v a="$arm" -v d="$detail" -v c="$compute" -v rp="$rep" \
        'NR>1 && $1==cs && $2==r && $3==a && $4==d && $5==c && $6==rp {found=1} END{exit !found}' "$CSV"
}

run_row() {
    local cs=$1 ratio=$2 arm=$3 detail=$4 compute=$5 rep=$6; shift 6
    local out rc
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 220 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- cs=$cs ratio=$ratio arm=$arm detail=$detail compute=$compute rep=$rep rc=$rc ---"
    log "$out"
    local touches bytes_touched wall_ns absent evictions infeasible prefetches pbf iord dedr dedf pinb
    if [ "$arm" = "A" ] || [ "$arm" = "B" ]; then
        touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
        bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
        wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
        iord=$(echo "$out" | grep -oP 'ARM_CSV,.*io_read_bytes_delta=\K[0-9]+' | tail -1)
        absent="n/a"; evictions="n/a"; infeasible="n/a"; prefetches="n/a"; pbf="n/a"; dedr="n/a"; dedf="n/a"; pinb="n/a"
    else
        touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
        bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
        wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
        absent=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[^,]+' | tail -1)
        evictions=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[^,]+' | tail -1)
        infeasible=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[^,]+' | tail -1)
        prefetches=$(echo "$out" | grep -oP 'ARM_CSV,.*prefetches=\K[^,]+' | tail -1)
        pbf=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
        iord=$(echo "$out" | grep -oP 'io_read_bytes_delta=\K[0-9]+' | tail -1)
        dedr=$(echo "$out" | grep -oP 'ARM_CSV,.*dedup_resident=\K[0-9]+' | tail -1)
        dedf=$(echo "$out" | grep -oP 'ARM_CSV,.*dedup_fetching=\K[0-9]+' | tail -1)
        pinb=$(echo "$out" | grep -oP 'ARM_CSV,.*pin_broken=\K[0-9]+' | tail -1)
    fi
    echo "$cs,$ratio,$arm,$detail,$compute,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${pbf:-},${iord:-},${dedr:-},${dedf:-},${pinb:-},$rc" >> "$CSV"
    cleanup_cgroup
}

for cs in $CHUNK_SIZES; do
    for ratio in $RATIOS; do
        budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
        memmax=$((budget_bytes + MARGIN))

        for rep in 1 2 3; do
            if rep_done "$cs" "$ratio" "A" "sequential" "n/a" "$rep"; then log "SKIP: A rep=$rep"; continue; fi
            fresh_cgroup "$memmax"; drop_caches
            run_row "$cs" "$ratio" "A" "sequential" "n/a" "$rep" \
                "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$N_PASSES" sequential off
        done
        for rep in 1 2 3; do
            if rep_done "$cs" "$ratio" "B" "normal" "n/a" "$rep"; then log "SKIP: B rep=$rep"; continue; fi
            fresh_cgroup "$memmax"; drop_caches
            run_row "$cs" "$ratio" "B" "normal" "n/a" "$rep" \
                "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$N_PASSES" normal on
        done

        for compute in $COMPUTES; do
            for rep in 1 2 3; do
                if rep_done "$cs" "$ratio" "C" "lru" "$compute" "$rep"; then log "SKIP: C rep=$rep"; continue; fi
                fresh_cgroup "$memmax"
                trace="$SCRATCH/baremetal_C_cs${cs}_c${compute}_r${ratio}_rep${rep}.fetchtrace"; rm -f "$trace"
                run_row "$cs" "$ratio" "C" "lru" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    lru off "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --compute-ns-per-mib "$compute"
            done
            for rep in 1 2 3; do
                if rep_done "$cs" "$ratio" "D" "layer_order" "$compute" "$rep"; then log "SKIP: D rep=$rep"; continue; fi
                fresh_cgroup "$memmax"
                trace="$SCRATCH/baremetal_D_cs${cs}_c${compute}_r${ratio}_rep${rep}.fetchtrace"
                reftrace="$SCRATCH/baremetal_D_cs${cs}_c${compute}_r${ratio}_rep${rep}.reftrace"
                rm -f "$trace" "$reftrace"
                run_row "$cs" "$ratio" "D" "layer_order" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    layer_order off "" "$reftrace" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --compute-ns-per-mib "$compute"
            done
            for rep in 1 2 3; do
                if rep_done "$cs" "$ratio" "E" "layer_order" "$compute" "$rep"; then log "SKIP: E rep=$rep"; continue; fi
                fresh_cgroup "$memmax"
                trace="$SCRATCH/baremetal_E_cs${cs}_c${compute}_r${ratio}_rep${rep}.fetchtrace"; rm -f "$trace"
                run_row "$cs" "$ratio" "E" "layer_order" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    layer_order on "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --prefetch-depth 2 --prefetch-retention pinned \
                    --compute-ns-per-mib "$compute"
            done
        done
    done
done

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
log "=== bare-metal sweep complete, results in $CSV ==="
```

Save both scripts (`bare_metal_env_baseline.sh` and
`bare_metal_session.sh`) in `$RESIDCTL/src/` before the session; the run
command is then just `bash src/bare_metal_session.sh` from the repo root.
If interrupted (background-task cap, terminal closed, etc.), re-running
the same command resumes from the last completed rep — every cell is
gated by `rep_done()` before it runs, matching every resumable sweep
script this project has used since Campaign 12.

## Final check

- No number in this plan was estimated for the bare-metal machine itself
  — every WSL2 comparison number is a direct citation from
  `results/campaign13_phaseC_claims.md` and `spike/results/SPIKE_REPORT.md`,
  and every environment value in §4's WSL2 table was read live from this
  machine, not recalled from memory.
- The sweep was not run; per instruction, this phase produces the plan
  only. The build/run script is complete and self-contained but has not
  been executed.
- The minimum sweep's size (240 runs) and time budget are justified
  explicitly against Phase C's 5 points, not chosen arbitrarily — and a
  fallback (drop 8 MiB, halving to 120 runs) is given if time runs short.
- No configuration or next step is recommended beyond what this plan
  itself is — running it, or not, remains the human decision the
  campaign instructions reserve.
