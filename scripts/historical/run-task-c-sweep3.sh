#!/bin/bash
# item 10c Task C, Sweep 3: prefetch admission A/B, arm E only.
# guarded vs always, 3 ratios, n=3, --prefetch-depth in {2,4}. Async
# handler (default), --fetch-workers 4. --fetch-trace captured per run for
# the post-hoc prefetch hit-rate analysis (item 10b Task B's method: a
# prefetch is a "hit" if no later fetch event exists for that chunk_id
# before the run ends).
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_sweep3
CSV=$RESULTS/task_c_sweep3.csv
LOG=$RESULTS/task_c_sweep3_log.txt
> "$LOG"
> "$CSV"
echo "ratio,admission,depth,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta,prefetch_declined,censored,exit_code" > "$CSV"

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
RATIOS="0.25 0.5 0.75"

log() { echo "$@" | tee -a "$LOG"; }

mkdir -p "$SCRATCH" "$RESULTS"
if [ ! -f "$MODEL" ]; then log "FATAL: $MODEL missing"; exit 1; fi

log "=== resource headroom check ==="
free -h | tee -a "$LOG"
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

run_one() {
    local ratio=$1 admission=$2 depth=$3 rep=$4 budget_bytes=$5; shift 5
    local out rc censored=0
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 180 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- ratio=$ratio admission=$admission depth=$depth rep=$rep rc=$rc ---"
    log "$out"
    if [ $rc -eq 137 ]; then
        censored=1
        log "CENSORED: OOM-killed at ratio=$ratio admission=$admission depth=$depth rep=$rep"
    fi

    local touches bytes_touched wall_ns absent evictions infeasible prefetches pbf iord decl
    touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
    wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    absent=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[^,]+' | tail -1)
    evictions=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[^,]+' | tail -1)
    infeasible=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[^,]+' | tail -1)
    prefetches=$(echo "$out" | grep -oP 'ARM_CSV,.*prefetches=\K[^,]+' | tail -1)
    pbf=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
    iord=$(echo "$out" | grep -oP 'io_read_bytes_delta=\K[0-9]+' | tail -1)
    decl=$(echo "$out" | grep -oP 'prefetch_declined=\K[0-9]+' | tail -1)

    echo "$ratio,$admission,$depth,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${pbf:-},${iord:-},${decl:-},$censored,$rc" >> "$CSV"
    cleanup_cgroup
}

for ratio in $RATIOS; do
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    memmax=$((budget_bytes + MARGIN))
    log ""
    log "############################################################"
    log "# ratio=$ratio budget_bytes=$budget_bytes memory.max=$memmax"
    log "############################################################"

    for depth in 2 4; do
        for admission in guarded always; do
            for rep in 1 2 3; do
                trace="$SCRATCH/sweep3_${admission}_d${depth}_r${ratio}_rep${rep}.fetchtrace"
                reftrace="$SCRATCH/sweep3_${admission}_d${depth}_r${ratio}_rep${rep}.reftrace"
                rm -f "$trace" "$reftrace"
                fresh_cgroup "$memmax"
                run_one "$ratio" "$admission" "$depth" "$rep" "$budget_bytes" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" layer_order on "" "$reftrace" \
                    --fetch-trace "$trace" --fetch-workers 4 --prefetch-depth "$depth" --prefetch-admission "$admission"
            done
        done
    done
done

rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
log ""
log "=== free memory (after) ==="
free -h | tee -a "$LOG"
log ""
log "=== Sweep 3 complete, results in $CSV ==="
