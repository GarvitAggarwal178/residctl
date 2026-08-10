#!/bin/bash
# Resumes run_task_c_sweep3.sh after the harness's 10-minute background-task
# cap killed it mid-run (r=0.25 fully done; r=0.5 done through
# depth=4/guarded/rep2; r=0.75 not started at all). Appends to the SAME
# CSV/log files.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_sweep3
CSV=$RESULTS/task_c_sweep3.csv
LOG=$RESULTS/task_c_sweep3_log.txt

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864

log() { echo "$@" | tee -a "$LOG"; }

log ""
log "=== RESUME: machine exclusivity check ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"

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
    log "--- RESUME ratio=$ratio admission=$admission depth=$depth rep=$rep rc=$rc ---"
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

run_cell() {
    local ratio=$1 admission=$2 depth=$3 rep=$4
    local budget_bytes memmax trace reftrace
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    memmax=$((budget_bytes + MARGIN))
    trace="$SCRATCH/sweep3_${admission}_d${depth}_r${ratio}_rep${rep}.fetchtrace"
    reftrace="$SCRATCH/sweep3_${admission}_d${depth}_r${ratio}_rep${rep}.reftrace"
    rm -f "$trace" "$reftrace"
    fresh_cgroup "$memmax"
    run_one "$ratio" "$admission" "$depth" "$rep" "$budget_bytes" \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" layer_order on "" "$reftrace" \
        --fetch-trace "$trace" --fetch-workers 4 --prefetch-depth "$depth" --prefetch-admission "$admission"
}

# --- finish r=0.5, depth=4 ---
run_cell 0.5 guarded 4 3
for rep in 1 2 3; do run_cell 0.5 always 4 "$rep"; done

# --- r=0.75, all four combos ---
for depth in 2 4; do
    for admission in guarded always; do
        for rep in 1 2 3; do
            run_cell 0.75 "$admission" "$depth" "$rep"
        done
    done
done

rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity check (after resume) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
log ""
log "=== free memory (after resume) ==="
free -h | tee -a "$LOG"
log ""
log "=== Sweep 3 (resumed) complete, results in $CSV ==="
