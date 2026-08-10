#!/bin/bash
# Resumes run_task_c_sweep2.sh after it was killed by the harness's own
# 10-minute background-task cap partway through ratio=0.75 (arms A-D for
# all three ratios had already completed and been appended to the CSV/log;
# only ratio=0.75's arm E and OPT were missing). Appends to the SAME
# CSV/log files -- does not truncate.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_sweep2
CSV=$RESULTS/task_c_sweep2.csv
LOG=$RESULTS/task_c_sweep2_log.txt

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
ratio=0.75

log() { echo "$@" | tee -a "$LOG"; }

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

run_arm() {
    local ratio=$1 arm=$2 detail=$3 rep=$4; shift 4
    local out rc censored=0
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 180 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- ratio=$ratio arm=$arm detail=$detail rep=$rep rc=$rc ---"
    log "$out"
    if [ $rc -eq 137 ]; then
        censored=1
        log "CENSORED: OOM-killed at ratio=$ratio arm=$arm detail=$detail rep=$rep"
    fi

    local touches bytes_touched wall_ns absent evictions infeasible prefetches pbf iord dedr dedf decl
    touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
    wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    absent=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[^,]+' | tail -1)
    evictions=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[^,]+' | tail -1)
    infeasible=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[^,]+' | tail -1)
    prefetches=$(echo "$out" | grep -oP 'ARM_CSV,.*prefetches=\K[^,]+' | tail -1)
    pbf=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
    iord=$(echo "$out" | grep -oP 'io_read_bytes_delta=\K[0-9]+' | tail -1)
    dedr=$(echo "$out" | grep -oP 'ARM_CSV,.*dedup_resident=\K[0-9]+' | tail -1)
    dedf=$(echo "$out" | grep -oP 'ARM_CSV,.*dedup_fetching=\K[0-9]+' | tail -1)
    decl=$(echo "$out" | grep -oP 'prefetch_declined=\K[0-9]+' | tail -1)

    echo "$ratio,$arm,$detail,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${pbf:-},${iord:-},${dedr:-},${dedf:-},${decl:-},$censored,$rc" >> "$CSV"
    cleanup_cgroup
    echo "$out"
}

budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
memmax=$((budget_bytes + MARGIN))
log ""
log "############################################################"
log "# RESUME: ratio=$ratio budget_bytes=$budget_bytes memory.max=$memmax"
log "############################################################"

for rep in 1 2 3; do
    fresh_cgroup "$memmax"
    run_arm "$ratio" "E" "layer_order_on" "$rep" \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" layer_order on "" "" --fetch-workers 4 --prefetch-depth 2 --prefetch-admission guarded > /dev/null
done

for rep in 1 2 3; do
    d_trace="$SCRATCH/sweep2_arm_d_ratio_${ratio}_rep${rep}.reftrace"
    if [ -s "$d_trace" ]; then
        opt_out=$("$SRC/belady_main" "$d_trace" "$CHUNK_SIZE" "$budget_bytes" 2>&1)
        log "--- ratio=$ratio arm=OPT rep=$rep ---"
        log "$opt_out"
        opt_misses=$(echo "$opt_out" | grep -oP 'minimum_misses=\K[0-9]+')
        opt_bytes=$(echo "$opt_out" | grep -oP 'minimum_bytes_fetched=\K[0-9]+')
        echo "$ratio,OPT,belady_over_D_trace,$rep,,,,${opt_misses:-},,,,,${opt_bytes:-},,,,0," >> "$CSV"
    else
        log "ratio=$ratio arm=OPT rep=$rep: SKIPPED, arm D produced no reference trace"
        echo "$ratio,OPT,belady_over_D_trace,$rep,,,,,,,,,,,,,SKIPPED" >> "$CSV"
    fi
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
log "=== Sweep 2 (resumed) complete, results in $CSV ==="
