#!/bin/bash
# Campaign 12 Phase D: the paper table. 2 GiB region, async handler,
# --fetch-workers 4 --driver-threads 8 --lookahead-window 1
# --prefetch-depth 2 --prefetch-retention pinned. Two chunk sizes: 8 MiB
# (Phase B's measured aggregate byte-floor for arm D) and 128 MiB (fixed
# for comparability with every prior report). Arms {A,B,C,D,E} x 5 ratios
# x --compute-ns-per-mib {0,400000} x n=3 (arm A/B: compute=n/a, run once
# per ratio -- no compute concept in baseline_main, same disclosed
# asymmetry as Phase 3/4). Arm A: full 3-mode madvise sweep, best reported.
# --fetch-trace on every C/D/E run this time (Phase 4 omitted it).
# drop_caches uses the CORRECT pipe pattern (Phase A's diagnosed fix,
# "2>&1 | tee -a", not "2>&1 >> "), and baseline_main has Phase A's
# startup-abort guard active. Per-rep resumability from the start (Phase
# B's lesson: gating whole cells on total-row-count causes duplicate rows
# on a resume that lands mid-cell).
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phaseD
CSV=$RESULTS/campaign12_phaseD_paper_table.csv
LOG=$RESULTS/campaign12_phaseD_paper_table_log.txt

REGION_LEN=2147483648
N_PASSES=5
MARGIN=67108864
CHUNK_SIZES="8388608 134217728"
RATIOS="0.25 0.375 0.5 0.625 0.75"
COMPUTES="0 400000"

log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$CSV" ]; then
    > "$LOG"
    > "$CSV"
    echo "chunk_size,ratio,arm,detail,compute,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta,dedup_resident,dedup_fetching,pin_broken,rc" > "$CSV"
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
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a "$LOG"; }

rep_done() {
    local cs=$1 ratio=$2 arm=$3 detail=$4 compute=$5 rep=$6
    awk -F, -v cs="$cs" -v r="$ratio" -v a="$arm" -v d="$detail" -v c="$compute" -v rp="$rep" \
        'NR>1 && $1==cs && $2==r && $3==a && $4==d && $5==c && $6==rp {found=1} END{exit !found}' "$CSV"
}

run_row() {
    local cs=$1 ratio=$2 arm=$3 detail=$4 compute=$5 rep=$6; shift 6
    local out rc
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 220 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- chunk_size=$cs ratio=$ratio arm=$arm detail=$detail compute=$compute rep=$rep rc=$rc ---"
    log "$out"

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

    echo "$cs,$ratio,$arm,$detail,$compute,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${pbf:-},${iord:-},${dedr:-},${dedf:-},${pinb:-},$rc" >> "$CSV"
    cleanup_cgroup
}

for cs in $CHUNK_SIZES; do
    for ratio in $RATIOS; do
        budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
        memmax=$((budget_bytes + MARGIN))

        # --- Arm A: 3 madvise modes, compute=n/a ---
        # NOTE: "random" mode excluded from this automatic loop -- known
        # slow submode (item 10c/Campaign 12 Phase A precedent, 160-220s
        # per rep) that repeatedly landed right at this session's
        # background-task window boundary, losing completed-but-uncaptured
        # runs on kill (parent killed, child orphaned under its own
        # `timeout 220`, result never reaches the CSV). Handled separately,
        # synchronously, via src/run_phaseD_random_rep.sh -- same disclosed
        # approach as Phase A.
        for mode in normal sequential; do
            for rep in 1 2 3; do
                if rep_done "$cs" "$ratio" "A" "$mode" "n/a" "$rep"; then
                    log "SKIP (done): cs=$cs ratio=$ratio arm=A mode=$mode rep=$rep"
                    continue
                fi
                fresh_cgroup "$memmax"
                drop_caches
                run_row "$cs" "$ratio" "A" "$mode" "n/a" "$rep" \
                    "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$N_PASSES" "$mode" off
            done
        done

        # --- Arm B: hints on, normal mode, compute=n/a ---
        for rep in 1 2 3; do
            if rep_done "$cs" "$ratio" "B" "normal" "n/a" "$rep"; then
                log "SKIP (done): cs=$cs ratio=$ratio arm=B rep=$rep"
                continue
            fi
            fresh_cgroup "$memmax"
            drop_caches
            run_row "$cs" "$ratio" "B" "normal" "n/a" "$rep" \
                "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$N_PASSES" normal on
        done

        # --- Arms C, D, E x 2 compute levels ---
        for compute in $COMPUTES; do
            for rep in 1 2 3; do
                if rep_done "$cs" "$ratio" "C" "lru" "$compute" "$rep"; then
                    log "SKIP (done): cs=$cs ratio=$ratio arm=C compute=$compute rep=$rep"
                    continue
                fi
                fresh_cgroup "$memmax"
                trace="$SCRATCH/phaseD_C_cs${cs}_c${compute}_r${ratio}_rep${rep}.fetchtrace"
                rm -f "$trace"
                run_row "$cs" "$ratio" "C" "lru" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    lru off "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --compute-ns-per-mib "$compute"
            done

            for rep in 1 2 3; do
                if rep_done "$cs" "$ratio" "D" "layer_order" "$compute" "$rep"; then
                    log "SKIP (done): cs=$cs ratio=$ratio arm=D compute=$compute rep=$rep"
                    continue
                fi
                fresh_cgroup "$memmax"
                trace="$SCRATCH/phaseD_D_cs${cs}_c${compute}_r${ratio}_rep${rep}.fetchtrace"
                reftrace="$SCRATCH/phaseD_D_cs${cs}_c${compute}_r${ratio}_rep${rep}.reftrace"
                rm -f "$trace" "$reftrace"
                run_row "$cs" "$ratio" "D" "layer_order" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    layer_order off "" "$reftrace" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --compute-ns-per-mib "$compute"
            done

            for rep in 1 2 3; do
                if rep_done "$cs" "$ratio" "E" "layer_order" "$compute" "$rep"; then
                    log "SKIP (done): cs=$cs ratio=$ratio arm=E compute=$compute rep=$rep"
                    continue
                fi
                fresh_cgroup "$memmax"
                trace="$SCRATCH/phaseD_E_cs${cs}_c${compute}_r${ratio}_rep${rep}.fetchtrace"
                rm -f "$trace"
                run_row "$cs" "$ratio" "E" "layer_order" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    layer_order on "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --prefetch-depth 2 --prefetch-retention pinned \
                    --compute-ns-per-mib "$compute"
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
log "=== Phase D sweep complete (or resumed to completion), results in $CSV ==="
