#!/bin/bash
# Build-order item 10, CORRECTED (see CLAUDE.md "ITEM 10 CORRECTION"):
# harness, arms A-E/OPT, budget sweep (§11), at realistic scale, n=3/cell.
#
# region_len=2GiB chunk_size=128MiB (16 chunks) n_passes=5 -- the scale at
# which the spike's §2 measurements apply (UFFDIO_CONTINUE negligible vs
# fetch) and per-syscall fixed costs stop dominating (Defect 2's whole
# point). Budget ratios 0.25/0.5/0.75. memory.max = budget_bytes + 64MiB
# margin, identical across every arm at a given ratio. drop_caches before
# every arm A/B invocation (carried forward from the V1 fix). Fresh 2GiB
# pattern file, not the 16MiB one items 1-9 reused all session.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_item10v2
CSV=$RESULTS/harness_v2_sweep.csv
LOG=$RESULTS/item10_v2_harness_log.txt
> "$LOG"
> "$CSV"
echo "ratio,arm,detail,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta,censored,exit_code" > "$CSV"

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
RATIOS="0.25 0.5 0.75"
N_REPS=3

log() { echo "$@" | tee -a "$LOG"; }

mkdir -p "$SCRATCH" "$RESULTS"
if [ ! -f "$MODEL" ]; then
    log "FATAL: $MODEL missing, must be generated before this script runs"
    exit 1
fi

log "=== resource headroom check ==="
free -h | tee -a "$LOG"
log "model file: $(ls -la "$MODEL")"

log "=== machine exclusivity check (before) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
if pgrep -f "cn-spike|gate5r_driver|iperf3" > /dev/null 2>&1; then
    log "FATAL: a cn-spike-related workload appears to be running. Per instructions: do not kill it, stop and report."
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

# Runs "$@" inside the current cgroup, joined via BASHPID before exec.
# Appends one CSV row per call. Prints raw output for the caller to parse
# further (e.g. touches/sec for arm A's mode selection).
run_arm() {
    local ratio=$1 arm=$2 detail=$3 rep=$4; shift 4
    local out rc censored=0
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 180 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- ratio=$ratio arm=$arm detail=$detail rep=$rep rc=$rc ---"
    log "$out"

    if [ $rc -eq 137 ]; then
        censored=1
        log "CENSORED: OOM-killed at ratio=$ratio arm=$arm detail=$detail rep=$rep -- policy infeasible at this budget (§11 censoring rule)"
    fi

    local touches bytes_touched wall_ns absent evictions infeasible prefetches pbf iord
    touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
    wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    absent=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[^,]+' | tail -1)
    evictions=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[^,]+' | tail -1)
    infeasible=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[^,]+' | tail -1)
    prefetches=$(echo "$out" | grep -oP 'ARM_CSV,.*prefetches=\K[^,]+' | tail -1)
    pbf=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
    iord=$(echo "$out" | grep -oP 'io_read_bytes_delta=\K[0-9]+' | tail -1)

    echo "$ratio,$arm,$detail,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${pbf:-},${iord:-},$censored,$rc" >> "$CSV"
    cleanup_cgroup
    echo "$out"
}

for ratio in $RATIOS; do
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    memmax=$((budget_bytes + MARGIN))
    log ""
    log "############################################################"
    log "# ratio=$ratio budget_bytes=$budget_bytes memory.max=$memmax"
    log "############################################################"

    # --- Arm A: mmap baseline, madvise swept (n=3 each mode), best by median touches/sec ---
    best_mode=""; best_median=0
    for mode in normal random sequential; do
        rates=""
        for rep in 1 2 3; do
            fresh_cgroup "$memmax"
            drop_caches
            out=$(run_arm "$ratio" "A" "mmap_${mode}_off" "$rep" \
                  "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$N_PASSES" "$mode" off)
            rate=$(echo "$out" | grep -oP 'touches/sec=\K[0-9.]+' | tail -1)
            rates="$rates ${rate:-0}"
        done
        median=$(echo $rates | tr ' ' '\n' | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
        log "arm A mode=$mode rates=[$rates] median=$median"
        if awk "BEGIN{exit !($median > $best_median)}"; then best_median=$median; best_mode=$mode; fi
    done
    log "arm A best mode: $best_mode (median ${best_median} touches/sec)"

    # --- Arm B: A's best mode + hints, n=3 ---
    for rep in 1 2 3; do
        fresh_cgroup "$memmax"
        drop_caches
        run_arm "$ratio" "B" "mmap_${best_mode}_on" "$rep" \
            "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$N_PASSES" "$best_mode" on > /dev/null
    done

    # --- Arm C: pager, lru, prefetch off, n=3 ---
    for rep in 1 2 3; do
        fresh_cgroup "$memmax"
        run_arm "$ratio" "C" "lru_off" "$rep" \
            "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" lru off > /dev/null
    done

    # --- Arm D: pager, layer_order, prefetch off, n=3 (each rep's reference trace kept for OPT determinism check) ---
    for rep in 1 2 3; do
        d_trace="$SCRATCH/harness_v2_arm_d_ratio_${ratio}_rep${rep}.reftrace"
        rm -f "$d_trace"
        fresh_cgroup "$memmax"
        run_arm "$ratio" "D" "layer_order_off" "$rep" \
            "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" layer_order off "" "$d_trace" > /dev/null
    done

    # --- Arm E: pager, layer_order, prefetch on, n=3 ---
    for rep in 1 2 3; do
        fresh_cgroup "$memmax"
        run_arm "$ratio" "E" "layer_order_on" "$rep" \
            "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" layer_order on > /dev/null
    done

    # --- OPT: offline solver over EACH of D's 3 reference traces (determinism check) ---
    for rep in 1 2 3; do
        d_trace="$SCRATCH/harness_v2_arm_d_ratio_${ratio}_rep${rep}.reftrace"
        if [ -s "$d_trace" ]; then
            opt_out=$("$SRC/belady_main" "$d_trace" "$CHUNK_SIZE" "$budget_bytes" 2>&1)
            log "--- ratio=$ratio arm=OPT rep=$rep ---"
            log "$opt_out"
            opt_misses=$(echo "$opt_out" | grep -oP 'minimum_misses=\K[0-9]+')
            opt_bytes=$(echo "$opt_out" | grep -oP 'minimum_bytes_fetched=\K[0-9]+')
            echo "$ratio,OPT,belady_over_D_trace,$rep,,,,${opt_misses:-},,,,,${opt_bytes:-},0," >> "$CSV"
        else
            log "ratio=$ratio arm=OPT rep=$rep: SKIPPED, arm D produced no reference trace"
            echo "$ratio,OPT,belady_over_D_trace,$rep,,,,,,,,,,SKIPPED" >> "$CSV"
        fi
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
log "=== sweep complete, results in $CSV ==="
