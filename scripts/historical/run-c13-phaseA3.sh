#!/bin/bash
# Campaign 13 Phase A.3: capture --policy-trace on a non-degenerate run
# (cell 1: fully serial, deterministic) and degenerate runs (cell 5's
# config: threads=8, window=1, compute=400000, 128MiB) -- 2 reps of the
# degenerate config to also check trace-to-trace consistency within the
# same nominal config.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_c13a3
LOG=$RESIDCTL/experiments/logs/campaign13_phaseA3_trace_log.txt
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
    local trace="$SCRATCH/c13_policytrace_${label}.bin"
    rm -f "$trace"
    fresh_cgroup
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 180 "$@"' -- \
          "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$BUDGET" "$N_PASSES" \
          layer_order off "" "" --policy-trace "$trace" --fetch-workers 4 \
          --driver-threads "$threads" --lookahead-window "$window" --compute-ns-per-mib "$compute" 2>&1)
    rc=$?
    log "--- label=$label threads=$threads window=$window compute=$compute rc=$rc ---"
    log "$out"
    cleanup_cgroup
}

run_one nondegenerate_cell1 1 0 0
run_one degenerate_cell5_rep1 8 1 400000
run_one degenerate_cell5_rep2 8 1 400000

rmdir "$CGROUP" 2>/dev/null
log "=== A.3 trace capture complete ==="
