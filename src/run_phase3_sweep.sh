#!/bin/bash
# Campaign 11 Phase 3: chunk size sweep. Region fixed 2 GiB.
# --chunk-size in {32MiB,64MiB,128MiB,256MiB} (64,32,16,8 chunks).
# Arms A, C, D, E. 3 ratios, n=3, async, --fetch-workers 4
# --driver-threads 8 --lookahead-window 1 --prefetch-depth 2
# --prefetch-retention pinned --compute-ns-per-mib 400000 (C/D/E only --
# arm A is baseline_main, a separate single-threaded binary with no
# driver-threads/lookahead/compute concept, unchanged from every prior
# item's use of it).
#
# DISCLOSED SIMPLIFICATION: arm A uses a single fixed madvise mode
# (sequential -- this project's most consistent historical winner at this
# workload) rather than the full {normal,random,sequential} sweep Phase 4
# explicitly requests. Phase 3's own instructions do not request the mode
# sweep, and the time-box is a hard constraint on top of an already large
# 4-chunk-size x 3-ratio x 4-arm grid.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phase3
CSV=$RESULTS/phase3_chunk_size.csv
LOG=$RESULTS/phase3_chunk_size_log.txt

REGION_LEN=2147483648
N_PASSES=5
MARGIN=67108864
CHUNK_SIZES="33554432 67108864 134217728 268435456"
RATIOS="0.25 0.5 0.75"
COMPUTE=400000

log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$CSV" ]; then
    > "$LOG"
    > "$CSV"
    echo "chunk_size,ratio,arm,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta,dedup_resident,dedup_fetching,pin_broken,censored,exit_code" > "$CSV"
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
    local cs=$1 ratio=$2 arm=$3
    local n
    n=$(awk -F, -v c="$cs" -v r="$ratio" -v a="$arm" \
        'NR>1 && $1==c && $2==r && $3==a {n++} END{print n+0}' "$CSV")
    [ "$n" -ge 3 ]
}

run_row() {
    local cs=$1 ratio=$2 arm=$3 rep=$4; shift 4
    local out rc censored=0
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 120 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- chunk_size=$cs ratio=$ratio arm=$arm rep=$rep rc=$rc ---"
    log "$out"
    if [ $rc -eq 137 ]; then censored=1; log "CENSORED: OOM-killed"; fi

    local touches bytes_touched wall_ns absent evictions infeasible prefetches pbf iord dedr dedf pinb
    if [ "$arm" = "A" ]; then
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

    echo "$cs,$ratio,$arm,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${pbf:-},${iord:-},${dedr:-},${dedf:-},${pinb:-},$censored,$rc" >> "$CSV"
    cleanup_cgroup
}

run_cell() {
    local cs=$1 ratio=$2 arm=$3 budget_bytes=$4

    if already_done "$cs" "$ratio" "$arm"; then
        log "SKIP (already 3 reps): chunk_size=$cs ratio=$ratio arm=$arm"
        return
    fi

    for rep in 1 2 3; do
        fresh_cgroup "$(($budget_bytes + MARGIN))"
        case "$arm" in
            A)
                drop_caches
                run_row "$cs" "$ratio" "$arm" "$rep" \
                    "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$N_PASSES" sequential off
                ;;
            C)
                trace="$SCRATCH/phase3_C_cs${cs}_r${ratio}_rep${rep}.fetchtrace"
                rm -f "$trace"
                run_row "$cs" "$ratio" "$arm" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    lru off "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --compute-ns-per-mib "$COMPUTE"
                ;;
            D)
                trace="$SCRATCH/phase3_D_cs${cs}_r${ratio}_rep${rep}.fetchtrace"
                reftrace="$SCRATCH/phase3_D_cs${cs}_r${ratio}_rep${rep}.reftrace"
                rm -f "$trace" "$reftrace"
                run_row "$cs" "$ratio" "$arm" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    layer_order off "" "$reftrace" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --compute-ns-per-mib "$COMPUTE"
                ;;
            E)
                trace="$SCRATCH/phase3_E_cs${cs}_r${ratio}_rep${rep}.fetchtrace"
                rm -f "$trace"
                run_row "$cs" "$ratio" "$arm" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    layer_order on "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --prefetch-depth 2 --prefetch-retention pinned \
                    --compute-ns-per-mib "$COMPUTE"
                ;;
        esac
    done
}

for cs in $CHUNK_SIZES; do
    for ratio in $RATIOS; do
        budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
        for arm in A C D E; do
            run_cell "$cs" "$ratio" "$arm" "$budget_bytes"
        done
    done
done

rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
log ""
log "=== Phase 3 sweep complete (or resumed to completion), results in $CSV ==="
