#!/bin/bash
# Campaign 11 Phase 2 STOP-AND-REPORT gate: --compute-ns-per-mib 0 must
# reproduce item 10e's numbers exactly at ratio=0.5, --driver-threads 8,
# --lookahead-window 1 (arm D: layer_order, prefetch off, async default).
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phase2_gate
LOG=$RESIDCTL/results/phase2_gate_log.txt
> "$LOG"

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
BUDGET_BYTES=1073741824   # ratio 0.5
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

for rep in 1 2 3; do
    reftrace="$SCRATCH/phase2_gate_rep${rep}.reftrace"
    rm -f "$reftrace"
    fresh_cgroup
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 60 "$@"' -- \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$BUDGET_BYTES" "$N_PASSES" \
        layer_order off "" "$reftrace" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 --compute-ns-per-mib 0 2>&1)
    rc=$?
    log "--- rep=$rep rc=$rc ---"
    log "$out"
    cleanup_cgroup
done

rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
