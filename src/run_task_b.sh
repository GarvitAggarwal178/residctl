#!/bin/bash
# Item 10b Task B: prefetch depth sweep at V2 scale.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_taskb
CSV=$RESULTS/task_b_sweep.csv
LOG=$RESULTS/task_b_log.txt
> "$LOG"
> "$CSV"
echo "depth,ratio,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta" > "$CSV"

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
RATIOS="0.25 0.5 0.75"
DEPTHS="1 2 4 8"

log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$MODEL" ]; then log "FATAL: $MODEL missing"; exit 1; fi

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
    for depth in $DEPTHS; do
        for rep in 1 2 3; do
            reft="$SCRATCH/task_b_d${depth}_r${ratio}_rep${rep}.reftrace"
            fett="$SCRATCH/task_b_d${depth}_r${ratio}_rep${rep}.fetchtrace"
            rm -f "$reft" "$fett"
            fresh_cgroup "$memmax"
            out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 60 "$@"' -- \
                  "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
                  layer_order on "" "$reft" --prefetch-depth "$depth" --fetch-trace "$fett" 2>&1)
            rc=$?
            log "--- depth=$depth ratio=$ratio rep=$rep rc=$rc ---"
            log "$out"
            cleanup_cgroup

            touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
            bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
            wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
            absent=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[0-9]+' | tail -1)
            evictions=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[0-9]+' | tail -1)
            infeasible=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[0-9]+' | tail -1)
            prefetches=$(echo "$out" | grep -oP 'ARM_CSV,.*prefetches=\K[0-9]+' | tail -1)
            pbf=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
            iord=$(echo "$out" | grep -oP 'io_read_bytes_delta=\K[0-9]+' | tail -1)
            echo "$depth,$ratio,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${pbf:-},${iord:-}" >> "$CSV"
        done
    done
done
rmdir "$CGROUP" 2>/dev/null
log "Task B sweep complete"
