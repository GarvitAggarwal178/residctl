#!/bin/bash
# Build-order item 10: harness, arms A-E/OPT, budget sweep (§11).
#
# Fixed test parameters (same region/chunk/pass counts used throughout this
# project's item 1-9 tests, for continuity and because building a larger
# synthetic model file doesn't change what's being validated here):
#   region_len=16MiB chunk_size=2MiB (8 chunks) n_passes=5
# Budget ratios swept: 0.25, 0.5, 0.75 (spec requires "at least three").
# memory.max = budget_bytes + a small fixed margin (headroom for each
# binary's own tiny process footprint -- these are all small C programs,
# not an LLM inference process, so a few MiB is ample) -- IDENTICAL across
# every arm at a given ratio, per §11's explicit requirement.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_16m.bin
CGROUP=/sys/fs/cgroup/residctl_item10
CSV=$RESULTS/harness_sweep.csv
LOG=$RESULTS/item10_harness_log.txt
> "$LOG"
> "$CSV"
echo "ratio,arm,detail,touches,wall_ns,absent_handled,evictions,infeasible,prefetches,majflt_delta,pgscan_delta,pgsteal_delta,censored,exit_code" > "$CSV"

REGION_LEN=16777216
CHUNK_SIZE=2097152
N_PASSES=5
MARGIN=4194304
RATIOS="0.25 0.5 0.75"

log() { echo "$@" | tee -a "$LOG"; }

mkdir -p "$SCRATCH" "$RESULTS"
[ -f "$MODEL" ] || "$SRC/gen_pattern" "$MODEL" "$REGION_LEN"

# ---- machine exclusivity (before) ----
log "=== machine exclusivity check (before) ==="
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

# Runs "$@" inside the current cgroup, joined via BASHPID before exec (the
# addendum2-derived pattern used throughout this project). Captures output
# and exit code; detects OOM-kill (SIGKILL -> 128+9=137) for the censoring
# rule. Appends one CSV row.
run_arm() {
    local ratio=$1 arm=$2 detail=$3; shift 3
    local out rc censored=0
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 30 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- ratio=$ratio arm=$arm detail=$detail rc=$rc ---"
    log "$out"

    if [ $rc -eq 137 ]; then
        censored=1
        log "CENSORED: OOM-killed at ratio=$ratio arm=$arm ($detail) -- policy infeasible at this budget, not a bug (per §11's censoring rule) unless investigated further"
    fi

    local touches wall_ns absent evictions infeasible prefetches majflt pgscan pgsteal
    touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    absent=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[^,]+' | tail -1)
    evictions=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[^,]+' | tail -1)
    infeasible=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[^,]+' | tail -1)
    prefetches=$(echo "$out" | grep -oP 'ARM_CSV,.*prefetches=\K[^,]+' | tail -1)
    majflt=$(echo "$out" | grep -oP 'majflt_delta=\K[0-9]+' | tail -1)
    pgscan=$(echo "$out" | grep -oP 'pgscan_delta=\K[0-9]+' | tail -1)
    pgsteal=$(echo "$out" | grep -oP 'pgsteal_delta=\K[0-9]+' | tail -1)

    echo "$ratio,$arm,$detail,${touches:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${prefetches:-},${majflt:-},${pgscan:-},${pgsteal:-},$censored,$rc" >> "$CSV"

    cleanup_cgroup
    echo "$out"
}

for ratio in $RATIOS; do
    budget_bytes=$(python3 -c "print(int($REGION_LEN*$ratio))" 2>/dev/null || awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    memmax=$((budget_bytes + MARGIN))
    log ""
    log "############################################################"
    log "# ratio=$ratio budget_bytes=$budget_bytes memory.max=$memmax"
    log "############################################################"

    # --- Arm A: mmap baseline, madvise swept, best reported ---
    # drop_caches before every run: pattern_16m.bin has been touched
    # repeatedly by items 1-9's tests all session and sits warm in the
    # kernel's globally-shared page cache. Without dropping it first, arm
    # A/B measure page-cache-hit speed (microseconds, zero pgscan/pgsteal
    # ever) regardless of memory.max, which defeats the entire point of a
    # baseline arm (measuring real kernel-driven reclaim under pressure).
    # This was caught by exactly that symptom on the first sweep attempt.
    best_mode=""; best_rate=0
    for mode in normal random sequential; do
        fresh_cgroup "$memmax"
        sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a "$LOG"
        out=$(run_arm "$ratio" "A" "mmap_${mode}_off" \
              "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$N_PASSES" "$mode" off)
        rate=$(echo "$out" | grep -oP 'touches/sec=\K[0-9.]+' | tail -1)
        rate=${rate:-0}
        if awk "BEGIN{exit !($rate > $best_rate)}"; then best_rate=$rate; best_mode=$mode; fi
    done
    log "arm A best mode: $best_mode (${best_rate} touches/sec)"

    # --- Arm B: A's best mode + hints ---
    fresh_cgroup "$memmax"
    sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a "$LOG"
    run_arm "$ratio" "B" "mmap_${best_mode}_on" \
        "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$N_PASSES" "$best_mode" on > /dev/null

    # --- Arm C: pager, lru, prefetch off ---
    fresh_cgroup "$memmax"
    run_arm "$ratio" "C" "lru_off" \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" lru off > /dev/null

    # --- Arm D: pager, layer_order, prefetch off (trace captured for OPT) ---
    d_trace="$SCRATCH/harness_arm_d_ratio_${ratio}.trace"
    rm -f "$d_trace"
    fresh_cgroup "$memmax"
    run_arm "$ratio" "D" "layer_order_off" \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" layer_order off "$d_trace" > /dev/null

    # --- Arm E: pager, layer_order, prefetch on ---
    fresh_cgroup "$memmax"
    run_arm "$ratio" "E" "layer_order_on" \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" layer_order on > /dev/null

    # --- OPT: offline solver over D's trace ---
    if [ -s "$d_trace" ]; then
        opt_out=$("$SRC/belady_main" "$d_trace" "$CHUNK_SIZE" "$budget_bytes" 2>&1)
        log "--- ratio=$ratio arm=OPT ---"
        log "$opt_out"
        opt_misses=$(echo "$opt_out" | grep -oP 'minimum_misses=\K[0-9]+')
        echo "$ratio,OPT,belady_over_D_trace,,,${opt_misses:-},,,,,,,0," >> "$CSV"
    else
        log "ratio=$ratio arm=OPT: SKIPPED, arm D produced no trace"
        echo "$ratio,OPT,belady_over_D_trace,,,,,,,,,,,SKIPPED" >> "$CSV"
    fi
done

rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"

log ""
log "=== sweep complete, results in $CSV ==="
cat "$CSV"
