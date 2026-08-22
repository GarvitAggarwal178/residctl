#!/bin/bash
# Campaign 11 Phase 4: consolidated sweep. 2 GiB region, --chunk-size 128MiB
# (unchanged from V2), async, --fetch-workers 4 --driver-threads 8
# --lookahead-window 1 --prefetch-depth 2 --prefetch-retention pinned.
# Varied: --compute-ns-per-mib {0,400000} x ratio {0.25,0.375,0.5,0.625,0.75}
# x arms {A,B,C,D,E}, n=3. OPT computed offline from D's reference traces
# (chunk-size/ratio dependent only, NOT compute-dependent -- the reference
# trace records touch order, unaffected by the compute phase's timing).
#
# DISCLOSED: baseline_main (arms A/B) has no --compute-ns-per-mib concept
# (a separate, single-threaded binary, unchanged architecture since item
# 10 -- same disclosed asymmetry as Phase 3). Arms A/B are therefore run
# ONCE per ratio (not duplicated per compute level) and labelled
# compute=n/a in the CSV, rather than wastefully re-running an identical-
# in-nature config twice.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phase4
CSV=$RESULTS/phase4_consolidated.csv
LOG=$RESULTS/phase4_consolidated_log.txt

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
RATIOS="0.25 0.375 0.5 0.625 0.75"
COMPUTES="0 400000"

log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$CSV" ]; then
    > "$LOG"
    > "$CSV"
    echo "ratio,arm,detail,compute,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta,dedup_resident,dedup_fetching,pin_broken,censored,exit_code" > "$CSV"
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
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 >> "$LOG"; }

already_done() {
    local ratio=$1 arm=$2 detail=$3 compute=$4
    local n
    n=$(awk -F, -v r="$ratio" -v a="$arm" -v d="$detail" -v c="$compute" \
        'NR>1 && $1==r && $2==a && $3==d && $4==c {n++} END{print n+0}' "$CSV")
    [ "$n" -ge 3 ]
}

run_row() {
    local ratio=$1 arm=$2 detail=$3 compute=$4 rep=$5; shift 5
    local out rc censored=0
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 120 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- ratio=$ratio arm=$arm detail=$detail compute=$compute rep=$rep rc=$rc ---"
    log "$out"
    if [ $rc -eq 137 ]; then censored=1; log "CENSORED: OOM-killed"; fi

    local touches bytes_touched wall_ns absent evictions infeasible prefetches pbf iord dedr dedf pinb
    if [ "$arm" = "A" ] || [ "$arm" = "B" ]; then
        touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
        bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
        wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
        iord=$(echo "$out" | grep -oP 'ARM_CSV,.*io_read_bytes_delta=\K[0-9]+' | tail -1)
        absent="n/a"; evictions="n/a"; infeasible="n/a"; prefetches="n/a"; pbf="n/a"; dedr="n/a"; dedf="n/a"; pinb="n/a"
    else
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
    fi

    echo "$ratio,$arm,$detail,$compute,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${pbf:-},${iord:-},${dedr:-},${dedf:-},${pinb:-},$censored,$rc" >> "$CSV"
    cleanup_cgroup
}

for ratio in $RATIOS; do
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    memmax=$((budget_bytes + MARGIN))

    # --- Arm A: 3 madvise modes, compute=n/a ---
    for mode in normal random sequential; do
        if already_done "$ratio" "A" "$mode" "n/a"; then
            log "SKIP: ratio=$ratio arm=A mode=$mode"
        else
            for rep in 1 2 3; do
                fresh_cgroup "$memmax"
                drop_caches
                run_row "$ratio" "A" "$mode" "n/a" "$rep" \
                    "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$N_PASSES" "$mode" off
            done
        fi
    done

    # --- Arm B: hints on, normal mode, compute=n/a ---
    if already_done "$ratio" "B" "normal" "n/a"; then
        log "SKIP: ratio=$ratio arm=B"
    else
        for rep in 1 2 3; do
            fresh_cgroup "$memmax"
            drop_caches
            run_row "$ratio" "B" "normal" "n/a" "$rep" \
                "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$N_PASSES" normal on
        done
    fi

    # --- Arms C, D, E: x 2 compute levels ---
    for compute in $COMPUTES; do
        if already_done "$ratio" "C" "lru" "$compute"; then
            log "SKIP: ratio=$ratio arm=C compute=$compute"
        else
            for rep in 1 2 3; do
                fresh_cgroup "$memmax"
                run_row "$ratio" "C" "lru" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
                    lru off "" "" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 --compute-ns-per-mib "$compute"
            done
        fi

        if already_done "$ratio" "D" "layer_order" "$compute"; then
            log "SKIP: ratio=$ratio arm=D compute=$compute"
        else
            for rep in 1 2 3; do
                fresh_cgroup "$memmax"
                reftrace="$SCRATCH/phase4_D_c${compute}_r${ratio}_rep${rep}.reftrace"
                rm -f "$reftrace"
                run_row "$ratio" "D" "layer_order" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
                    layer_order off "" "$reftrace" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 --compute-ns-per-mib "$compute"
            done
        fi

        if already_done "$ratio" "E" "layer_order" "$compute"; then
            log "SKIP: ratio=$ratio arm=E compute=$compute"
        else
            for rep in 1 2 3; do
                fresh_cgroup "$memmax"
                run_row "$ratio" "E" "layer_order" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
                    layer_order on "" "" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 \
                    --prefetch-depth 2 --prefetch-retention pinned --compute-ns-per-mib "$compute"
            done
        fi
    done
done

rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
log ""
log "=== Phase 4 sweep complete (or resumed to completion), results in $CSV ==="
