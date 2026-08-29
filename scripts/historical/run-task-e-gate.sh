#!/bin/bash
# item 10e Task A verification gate: --lookahead-window {0,1,2} at
# --driver-threads 8, identical parameters (2 GiB, 128 MiB chunks, 5
# passes, ratio 0.5). Confirms reference trace / bytes / OPT identity
# across W, and that W=0 reproduces item 10d's exact gate numbers.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_task_e_gate
LOG=$RESIDCTL/results/task_e_gate_log.txt
> "$LOG"

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
BUDGET_BYTES=1073741824   # 0.5 ratio -- same as item 10d's own gate
MEMMAX=$((BUDGET_BYTES + 67108864))

log() { echo "$@" | tee -a "$LOG"; }

log "=== machine exclusivity check (before) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"

fresh_cgroup() {
    if [ -d "$CGROUP" ]; then
        local procs
        procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
        if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
        rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo "$MEMMAX" > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
}
cleanup_cgroup() {
    local procs
    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
}

run_one() {
    local window=$1 reftrace=$2
    rm -f "$reftrace"
    fresh_cgroup
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 60 "$@"' -- \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$BUDGET_BYTES" "$N_PASSES" \
        default off "" "$reftrace" --driver-threads 8 --lookahead-window "$window" 2>&1)
    rc=$?
    log "--- window=$window rc=$rc ---"
    log "$out"
    cleanup_cgroup
}

run_one 0 "$SCRATCH/gate_w0.reftrace"
run_one 1 "$SCRATCH/gate_w1.reftrace"
run_one 2 "$SCRATCH/gate_w2.reftrace"

rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
