#!/bin/bash
# Campaign 12 Phase B verification gate: generate one reference trace per
# chunk size {4,8,16,32}MiB at ratio=0.5 (compute=0, since reference trace
# content is compute-independent -- fast, just for the gate check), then
# check OPT >= cyclic floor via belady_main at all 3 ratios for each size.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phaseB_gate
LOG=$RESIDCTL/results/campaign12_phaseB_gate_log.txt
> "$LOG"

REGION_LEN=2147483648
N_PASSES=5
CHUNK_SIZES="4194304 8388608 16777216 33554432"

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

for cs in $CHUNK_SIZES; do
    budget_bytes=1073741824  # ratio 0.5
    memmax=$((budget_bytes + 67108864))
    reftrace="$SCRATCH/phaseB_gate_cs${cs}.reftrace"
    rm -f "$reftrace"
    fresh_cgroup "$memmax"
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 60 "$@"' -- \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
        layer_order off "" "$reftrace" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 --compute-ns-per-mib 0 2>&1)
    rc=$?
    log "--- chunk_size=$cs gate-generation rc=$rc ---"
    log "$out" | tail -5
    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
done
rmdir "$CGROUP" 2>/dev/null

log ""
log "=== OPT / cyclic floor check at each (chunk_size, ratio) ==="
for cs in $CHUNK_SIZES; do
    reftrace="$SCRATCH/phaseB_gate_cs${cs}.reftrace"
    for ratio in 0.25 0.5 0.75; do
        budget=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
        log "--- chunk_size=$cs ratio=$ratio budget=$budget ---"
        "$SRC/belady_main" "$reftrace" "$cs" "$budget" 2>&1 | tee -a "$LOG"
    done
done
