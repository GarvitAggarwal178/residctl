#!/bin/bash
# Item 10b Task A: fetch-trace instrumentation, arm D at V2 scale, n=3/ratio.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_taska
LOG=$RESULTS/task_a_log.txt
> "$LOG"

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
RATIOS="0.25 0.5 0.75"

log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$MODEL" ]; then
    log "FATAL: $MODEL missing"
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

for ratio in $RATIOS; do
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    memmax=$((budget_bytes + MARGIN))
    for rep in 1 2 3; do
        ft="$SCRATCH/task_a_ratio_${ratio}_rep${rep}.fetchtrace"
        rm -f "$ft"
        fresh_cgroup "$memmax"
        out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 60 "$@"' -- \
              "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
              layer_order off --fetch-trace "$ft" 2>&1)
        rc=$?
        log "--- ratio=$ratio rep=$rep rc=$rc ---"
        log "$out"
        cleanup_cgroup
    done
done
rmdir "$CGROUP" 2>/dev/null
log "Task A runs complete"
