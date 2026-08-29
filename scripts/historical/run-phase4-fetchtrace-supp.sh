#!/bin/bash
# Supplementary to Phase 4: the main sweep did not pass --fetch-trace (an
# oversight, disclosed in the report), so device-busy/concurrently-
# outstanding/per-fetch-timing metrics are unavailable from it. This
# backfills exactly those metrics with ONE additional rep per (ratio, arm,
# compute) cell for arms C/D/E, --fetch-trace captured. NOT blended into
# the main sweep's n=3 byte/fault/wall-clock medians -- used only for the
# fetch-trace-derived metrics, labelled as n=1 supplementary in the report.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phase4_supp
LOG=$RESULTS/phase4_fetchtrace_supp_log.txt
> "$LOG"

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
RATIOS="0.25 0.375 0.5 0.625 0.75"
COMPUTES="0 400000"

log() { echo "$@" | tee -a "$LOG"; }

log "=== machine exclusivity check ==="
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
    local ratio=$1 arm=$2 compute=$3 budget_bytes=$4; shift 4
    fresh_cgroup "$(($budget_bytes + MARGIN))"
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 120 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- SUPP ratio=$ratio arm=$arm compute=$compute rc=$rc ---"
    log "$out"
    cleanup_cgroup
}

for ratio in $RATIOS; do
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    for compute in $COMPUTES; do
        trace="$SCRATCH/phase4supp_C_c${compute}_r${ratio}.fetchtrace"
        rm -f "$trace"
        run_one "$ratio" "C" "$compute" "$budget_bytes" \
            "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
            lru off "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 --compute-ns-per-mib "$compute"

        trace="$SCRATCH/phase4supp_D_c${compute}_r${ratio}.fetchtrace"
        rm -f "$trace"
        run_one "$ratio" "D" "$compute" "$budget_bytes" \
            "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
            layer_order off "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 --compute-ns-per-mib "$compute"

        trace="$SCRATCH/phase4supp_E_c${compute}_r${ratio}.fetchtrace"
        rm -f "$trace"
        run_one "$ratio" "E" "$compute" "$budget_bytes" \
            "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$budget_bytes" "$N_PASSES" \
            layer_order on "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 \
            --prefetch-depth 2 --prefetch-retention pinned --compute-ns-per-mib "$compute"
    done
done

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
log "=== supplementary fetch-trace pass complete ==="
