#!/bin/bash
# Campaign 12 Phase A: repaired re-run of Phase 4's arm A (3 madvise modes)
# and arm B (hints on, normal mode), at Phase 4's original grid: 5 ratios x
# n=3, 128MiB chunk size (Phase 4's fixed value). Uses the FIXED
# drop_caches pattern and the new baseline_main guard.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phase4_AB
CSV=$RESULTS/campaign12_phaseA_phase4_AB.csv
LOG=$RESULTS/campaign12_phaseA_phase4_AB_log.txt
if [ ! -f "$CSV" ]; then
    > "$LOG"
    > "$CSV"
    echo "ratio,arm,mode,rep,touches,bytes_touched,wall_ns,io_read_bytes_delta,rc" > "$CSV"
    echo "=== fresh start ===" | tee -a "$LOG"
else
    echo "=== resuming: existing CSV has $(wc -l < "$CSV") lines ===" | tee -a "$LOG"
fi

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
RATIOS="0.25 0.375 0.5 0.625 0.75"

log() { echo "$@" | tee -a "$LOG"; }

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
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a "$LOG"; }

already_done() {
    local ratio=$1 arm=$2 mode=$3
    local n
    n=$(awk -F, -v r="$ratio" -v a="$arm" -v m="$mode" \
        'NR>1 && $1==r && $2==a && $3==m {n++} END{print n+0}' "$CSV")
    [ "$n" -ge 3 ]
}

run_one() {
    local ratio=$1 arm=$2 mode=$3 rep=$4; shift 4
    local out rc
    # 220s, not 120s: item 10c's ASYNC_REPORT documented MADV_RANDOM under
    # full-chunk consumption running 160-180s, uncomfortably close to a
    # 180s ceiling there too. Widened here rather than accepting a repeat
    # of that same coverage gap when a more generous timeout is cheap.
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 220 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- ratio=$ratio arm=$arm mode=$mode rep=$rep rc=$rc ---"
    log "$out"

    local touches bytes_touched wall_ns iord
    touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
    wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    iord=$(echo "$out" | grep -oP 'ARM_CSV,.*io_read_bytes_delta=\K[0-9]+' | tail -1)

    echo "$ratio,$arm,$mode,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${iord:-},$rc" >> "$CSV"
    cleanup_cgroup
}

for ratio in $RATIOS; do
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    memmax=$((budget_bytes + MARGIN))

    for mode in normal random sequential; do
        if already_done "$ratio" "A" "$mode"; then
            echo "SKIP (already 3 reps): ratio=$ratio arm=A mode=$mode" | tee -a "$LOG"
            continue
        fi
        for rep in 1 2 3; do
            fresh_cgroup "$memmax"
            drop_caches
            run_one "$ratio" "A" "$mode" "$rep" \
                "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$N_PASSES" "$mode" off
        done
    done

    if already_done "$ratio" "B" "normal"; then
        echo "SKIP (already 3 reps): ratio=$ratio arm=B" | tee -a "$LOG"
    else
        for rep in 1 2 3; do
            fresh_cgroup "$memmax"
            drop_caches
            run_one "$ratio" "B" "normal" "$rep" \
                "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$N_PASSES" normal on
        done
    fi
done

rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
log ""
log "=== Phase 4 arm A/B repair complete, results in $CSV ==="
