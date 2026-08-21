#!/bin/bash
# item 10e Task B: sync vs async, overlap actually possible via the bounded
# lookahead window. Arm D (layer_order, prefetch off), V2 parameters, 3
# ratios x n=3 x --lookahead-window {0,1,2} x --driver-threads {1,8} x
# handler {sync, async --fetch-workers 4}. --fetch-trace captured per run.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_sweep_e_b
CSV=$RESULTS/task_e_sweep_b.csv
LOG=$RESULTS/task_e_sweep_b_log.txt
> "$LOG"
> "$CSV"
echo "ratio,window,threads,handler,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,dedup_resident,dedup_fetching,pager_bytes_fetched,io_read_bytes_delta,censored,exit_code" > "$CSV"

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
RATIOS="0.25 0.5 0.75"
WINDOWS="0 1 2"
THREAD_COUNTS="1 8"

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
    local ratio=$1 window=$2 threads=$3 handler=$4 rep=$5 budget_bytes=$6; shift 6
    local out rc censored=0
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 60 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- ratio=$ratio window=$window threads=$threads handler=$handler rep=$rep rc=$rc ---"
    log "$out"
    if [ $rc -eq 137 ]; then
        censored=1
        log "CENSORED: OOM-killed at ratio=$ratio window=$window threads=$threads handler=$handler rep=$rep"
    fi

    local touches bytes_touched wall_ns absent evictions infeasible dedr dedf pbf iord
    touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
    wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    absent=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[^,]+' | tail -1)
    evictions=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[^,]+' | tail -1)
    infeasible=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[^,]+' | tail -1)
    dedr=$(echo "$out" | grep -oP 'ARM_CSV,.*dedup_resident=\K[0-9]+' | tail -1)
    dedf=$(echo "$out" | grep -oP 'ARM_CSV,.*dedup_fetching=\K[0-9]+' | tail -1)
    pbf=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
    iord=$(echo "$out" | grep -oP 'io_read_bytes_delta=\K[0-9]+' | tail -1)

    echo "$ratio,$window,$threads,$handler,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${dedr:-},${dedf:-},${pbf:-},${iord:-},$censored,$rc" >> "$CSV"
    cleanup_cgroup
}

for ratio in $RATIOS; do
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    memmax=$((budget_bytes + MARGIN))
    log ""
    log "############################################################"
    log "# ratio=$ratio budget_bytes=$budget_bytes memory.max=$memmax"
    log "############################################################"

    for window in $WINDOWS; do
        for threads in $THREAD_COUNTS; do
            for rep in 1 2 3; do
                trace="$SCRATCH/sweepeb_sync_w${window}_t${threads}_r${ratio}_rep${rep}.fetchtrace"
                rm -f "$trace"
                fresh_cgroup "$memmax"
                run_one "$ratio" "$window" "$threads" "sync" "$rep" "$budget_bytes" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" layer_order off --fetch-trace "$trace" --sync-handler --driver-threads "$threads" --lookahead-window "$window"
            done
            for rep in 1 2 3; do
                trace="$SCRATCH/sweepeb_async_w${window}_t${threads}_r${ratio}_rep${rep}.fetchtrace"
                rm -f "$trace"
                fresh_cgroup "$memmax"
                run_one "$ratio" "$window" "$threads" "async" "$rep" "$budget_bytes" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" layer_order off --fetch-trace "$trace" --fetch-workers 4 --driver-threads "$threads" --lookahead-window "$window"
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
log "=== Sweep B (item 10e) complete, results in $CSV ==="
