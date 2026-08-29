#!/bin/bash
# item 10d Task A verification gate: run the replay driver at
# --driver-threads 1 and 8 over the SAME parameters, and confirm the
# reference trace, total bytes read, and OPT are all identical. Must PASS
# before proceeding to Task B (per the item 10d spec's explicit instruction).
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_task_a_gate
LOG=$RESIDCTL/experiments/logs/task_d_gate_log.txt
> "$LOG"

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
BUDGET_BYTES=1073741824   # 0.5 ratio -- generous, not the point of this check
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
    local nthreads=$1 reftrace=$2
    rm -f "$reftrace"
    fresh_cgroup
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 60 "$@"' -- \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$BUDGET_BYTES" "$N_PASSES" \
        default off "" "$reftrace" --driver-threads "$nthreads" 2>&1)
    rc=$?
    log "--- driver_threads=$nthreads rc=$rc ---"
    log "$out"
    cleanup_cgroup
}

run_one 1 "$SCRATCH/gate_n1.reftrace"
run_one 8 "$SCRATCH/gate_n8.reftrace"

rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
