#!/bin/bash
# Campaign 13 Phase A.3b: re-capture with BOTH --policy-trace AND a
# reference-trace-path from the same run, so anti-optimality can be
# checked via hindsight against the true future access order.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_c13a3b
LOG=$RESIDCTL/experiments/logs/campaign13_phaseA3b_trace_log.txt
> "$LOG"

REGION_LEN=2147483648
CHUNK_SIZE=134217728
BUDGET=1073741824
N_PASSES=5
MARGIN=67108864

log() { echo "$@" | tee -a "$LOG"; }

fresh_cgroup() {
    if [ -d "$CGROUP" ]; then
        local procs
        procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
        if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
        rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo "$((BUDGET + MARGIN))" > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
}
cleanup_cgroup() {
    local procs
    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
}

run_one() {
    local label=$1 threads=$2 window=$3 compute=$4
    local ptrace="$SCRATCH/c13_b_policytrace_${label}.bin"
    local reftrace="$SCRATCH/c13_b_reftrace_${label}.bin"
    rm -f "$ptrace" "$reftrace"
    fresh_cgroup
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 180 "$@"' -- \
          "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$BUDGET" "$N_PASSES" \
          layer_order off "" "$reftrace" --policy-trace "$ptrace" --fetch-workers 4 \
          --driver-threads "$threads" --lookahead-window "$window" --compute-ns-per-mib "$compute" 2>&1)
    rc=$?
    log "--- label=$label threads=$threads window=$window compute=$compute rc=$rc ---"
    log "$out"
    cleanup_cgroup
}

run_one nondegenerate_cell1 1 0 0
run_one degenerate_cell5 8 1 400000

rmdir "$CGROUP" 2>/dev/null
log "=== A.3b trace+reftrace capture complete ==="
