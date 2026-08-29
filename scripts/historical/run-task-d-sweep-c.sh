#!/bin/bash
# item 10d Task C sweep: arm E only, --prefetch-retention {none,pinned},
# 3 ratios x depth {2,4} x n=3, async handler, --fetch-workers 4,
# --driver-threads 8. --fetch-trace captured per run for the post-hoc
# hit-rate analysis (item 10b/10c method, reused).
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_sweep_d_c
CSV=$RESULTS/task_d_sweep_c.csv
LOG=$RESULTS/task_d_sweep_c_log.txt
> "$LOG"
> "$CSV"
echo "ratio,retention,depth,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta,pin_broken,censored,exit_code" > "$CSV"

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

run_cell() {
    local ratio=$1 retention=$2 depth=$3 rep=$4 budget_bytes=$5; shift 5
    local trace="$SCRATCH/sweepc_${retention}_d${depth}_r${ratio}_rep${rep}.fetchtrace"
    rm -f "$trace"
    fresh_cgroup "$(($budget_bytes + MARGIN))"
    local out rc censored=0
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 60 "$@"' -- \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
        layer_order on "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
        --prefetch-depth "$depth" --prefetch-retention "$retention" 2>&1)
    rc=$?
    log "--- ratio=$ratio retention=$retention depth=$depth rep=$rep rc=$rc ---"
    log "$out"
    if [ $rc -eq 137 ]; then censored=1; log "CENSORED: OOM-killed"; fi

    local touches bytes_touched wall_ns absent evictions infeasible prefetches pbf iord pinb
    touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
    wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    absent=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[^,]+' | tail -1)
    evictions=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[^,]+' | tail -1)
    infeasible=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[^,]+' | tail -1)
    prefetches=$(echo "$out" | grep -oP 'ARM_CSV,.*prefetches=\K[^,]+' | tail -1)
    pbf=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
    iord=$(echo "$out" | grep -oP 'io_read_bytes_delta=\K[0-9]+' | tail -1)
    pinb=$(echo "$out" | grep -oP 'ARM_CSV,.*pin_broken=\K[0-9]+' | tail -1)

    echo "$ratio,$retention,$depth,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${pbf:-},${iord:-},${pinb:-},$censored,$rc" >> "$CSV"
    cleanup_cgroup
}

for ratio in $RATIOS; do
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    log ""
    log "############################################################"
    log "# ratio=$ratio budget_bytes=$budget_bytes"
    log "############################################################"
    for depth in 2 4; do
        for retention in none pinned; do
            for rep in 1 2 3; do
                run_cell "$ratio" "$retention" "$depth" "$rep" "$budget_bytes"
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
log "=== Sweep C complete, results in $CSV ==="
