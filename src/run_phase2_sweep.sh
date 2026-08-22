#!/bin/bash
# Campaign 11 Phase 2 sweep: arm D and arm E, 3 ratios x n=3, async,
# --fetch-workers 4 --driver-threads 8 --lookahead-window 1,
# --prefetch-depth {1,2,4} x --prefetch-retention {none,pinned} for arm E,
# --compute-ns-per-mib {0,100000,400000}.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phase2
CSV=$RESULTS/phase2_compute.csv
LOG=$RESULTS/phase2_compute_log.txt

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
RATIOS="0.25 0.5 0.75"
COMPUTES="0 100000 400000"

log() { echo "$@" | tee -a "$LOG"; }

# Support resuming: only reset CSV/LOG if CSV doesn't exist yet.
if [ ! -f "$CSV" ]; then
    > "$LOG"
    > "$CSV"
    echo "ratio,arm,depth,retention,compute,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta,dedup_resident,dedup_fetching,pin_broken,compute_achieved,censored,exit_code" > "$CSV"
    log "=== fresh start ==="
else
    log "=== resuming: existing CSV has $(wc -l < "$CSV") lines ==="
fi

log "=== machine exclusivity check ==="
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

# already_done ratio arm depth retention compute -- checks CSV for 3 existing reps
already_done() {
    local ratio=$1 arm=$2 depth=$3 retention=$4 compute=$5
    local n
    n=$(awk -F, -v r="$ratio" -v a="$arm" -v d="$depth" -v rt="$retention" -v c="$compute" \
        'NR>1 && $1==r && $2==a && $3==d && $4==rt && $5==c {n++} END{print n+0}' "$CSV")
    [ "$n" -ge 3 ]
}

run_row() {
    local ratio=$1 arm=$2 depth=$3 retention=$4 compute=$5 rep=$6 budget_bytes=$7; shift 7
    local out rc censored=0
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 90 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- ratio=$ratio arm=$arm depth=$depth retention=$retention compute=$compute rep=$rep rc=$rc ---"
    log "$out"
    if [ $rc -eq 137 ]; then censored=1; log "CENSORED: OOM-killed"; fi

    local touches bytes_touched wall_ns absent evictions infeasible prefetches pbf iord dedr dedf pinb cach
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
    pinb=$(echo "$out" | grep -oP 'ARM_CSV,.*pin_broken=\K[0-9]+' | tail -1)
    cach=$(echo "$out" | grep -oP 'compute_achieved_ns_per_mib=\K[0-9.]+' | tail -1)

    echo "$ratio,$arm,$depth,$retention,$compute,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${pbf:-},${iord:-},${dedr:-},${dedf:-},${pinb:-},${cach:-},$censored,$rc" >> "$CSV"
    cleanup_cgroup
}

run_cell() {
    local ratio=$1 arm=$2 depth=$3 retention=$4 compute=$5 budget_bytes=$6

    if already_done "$ratio" "$arm" "$depth" "$retention" "$compute"; then
        log "SKIP (already 3 reps): ratio=$ratio arm=$arm depth=$depth retention=$retention compute=$compute"
        return
    fi

    for rep in 1 2 3; do
        fresh_cgroup "$(($budget_bytes + MARGIN))"
        if [ "$arm" = "D" ]; then
            trace="$SCRATCH/phase2_D_c${compute}_r${ratio}_rep${rep}.fetchtrace"
            rm -f "$trace"
            run_row "$ratio" "$arm" "n/a" "n/a" "$compute" "$rep" "$budget_bytes" \
                "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
                layer_order off "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                --lookahead-window 1 --compute-ns-per-mib "$compute"
        else
            trace="$SCRATCH/phase2_E_${retention}_d${depth}_c${compute}_r${ratio}_rep${rep}.fetchtrace"
            rm -f "$trace"
            run_row "$ratio" "$arm" "$depth" "$retention" "$compute" "$rep" "$budget_bytes" \
                "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
                layer_order on "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                --lookahead-window 1 --prefetch-depth "$depth" --prefetch-retention "$retention" \
                --compute-ns-per-mib "$compute"
        fi
    done
}

for ratio in $RATIOS; do
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    for compute in $COMPUTES; do
        run_cell "$ratio" "D" "n/a" "n/a" "$compute" "$budget_bytes"
        for depth in 1 2 4; do
            for retention in none pinned; do
                run_cell "$ratio" "E" "$depth" "$retention" "$compute" "$budget_bytes"
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
log "=== Phase 2 sweep complete (or resumed to completion), results in $CSV ==="
