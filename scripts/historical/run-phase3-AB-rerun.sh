#!/bin/bash
# Campaign 12 Phase A: repaired re-run of Phase 3's arm A only (Phase 3 had
# no arm B). Same grid as Phase 3: --chunk-size {32,64,128,256}MiB x 3
# ratios x n=3, single fixed 'sequential' madvise mode (as Phase 3 did).
# Uses the FIXED drop_caches pattern (pipe to tee, matching V2's original
# and every other sweep script in this project) and the new
# baseline_main guard (aborts loudly if io_read_bytes delta is 0).
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phase3_AB
CSV=$RESULTS/campaign12_phaseA_phase3_AB.csv
LOG=$RESULTS/campaign12_phaseA_phase3_AB_log.txt
> "$LOG"
> "$CSV"
echo "chunk_size,ratio,rep,touches,bytes_touched,wall_ns,io_read_bytes_delta,rc" > "$CSV"

REGION_LEN=2147483648
N_PASSES=5
MARGIN=67108864
CHUNK_SIZES="33554432 67108864 134217728 268435456"
RATIOS="0.25 0.5 0.75"

log() { echo "$@" | tee -a "$LOG"; }

log "=== machine exclusivity check (before) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
if pgrep -f "cn-spike|gate5r_driver|iperf3" > /dev/null 2>&1; then
    log "FATAL: a foreign workload appears to be running. Per instructions: do not kill it, stop and report."
    exit 1
fi

fresh_cgroup() {
    local max=$1
    if [ -d "$CGROUP" ]; then
        local procs
        procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
        if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
        rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo "$max" > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
}
cleanup_cgroup() {
    local procs
    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
}
# FIXED: pipe to tee (matches V2's original, run_task_c_sweep1/2.sh,
# run_task_d_sweep_b.sh) -- NOT a second ">> $LOG" redirect on the same
# command, which silently overrides fd1 and never touches the real file.
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a "$LOG"; }

run_one() {
    local cs=$1 ratio=$2 rep=$3; shift 3
    local out rc
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 120 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- chunk_size=$cs ratio=$ratio rep=$rep rc=$rc ---"
    log "$out"

    local touches bytes_touched wall_ns iord
    touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
    wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    iord=$(echo "$out" | grep -oP 'ARM_CSV,.*io_read_bytes_delta=\K[0-9]+' | tail -1)

    echo "$cs,$ratio,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${iord:-},$rc" >> "$CSV"
    cleanup_cgroup
}

for cs in $CHUNK_SIZES; do
    for ratio in $RATIOS; do
        budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
        memmax=$((budget_bytes + MARGIN))
        for rep in 1 2 3; do
            fresh_cgroup "$memmax"
            drop_caches
            run_one "$cs" "$ratio" "$rep" \
                "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$N_PASSES" sequential off
        done
    done
done

rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
log ""
log "=== Phase 3 arm A repair complete, results in $CSV ==="
