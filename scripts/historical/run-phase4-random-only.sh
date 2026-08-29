#!/bin/bash
# Dedicated pass for arm A random mode only, across all 5 Phase 4 ratios,
# so it isn't repeatedly squeezed to the end of a 10-min window and cut
# off right as it starts. Appends to the same CSV as run_phase4_AB_rerun.sh.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phase4_AB
CSV=$RESULTS/campaign12_phaseA_phase4_AB.csv
LOG=$RESULTS/campaign12_phaseA_phase4_AB_log.txt

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
RATIOS="0.25 0.375 0.5 0.625 0.75"

log() { echo "$@" | tee -a "$LOG"; }

log "=== random-only pass: machine exclusivity check ==="
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
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a "$LOG"; }

already_done() {
    local ratio=$1
    local n
    n=$(awk -F, -v r="$ratio" 'NR>1 && $1==r && $2=="A" && $3=="random" {n++} END{print n+0}' "$CSV")
    [ "$n" -ge 3 ]
}

run_one() {
    local ratio=$1 rep=$2; shift 2
    local out rc
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 220 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- RANDOM-ONLY ratio=$ratio rep=$rep rc=$rc ---"
    log "$out"

    local touches bytes_touched wall_ns iord
    touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
    wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    iord=$(echo "$out" | grep -oP 'ARM_CSV,.*io_read_bytes_delta=\K[0-9]+' | tail -1)

    echo "$ratio,A,random,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${iord:-},$rc" >> "$CSV"
    cleanup_cgroup
}

for ratio in $RATIOS; do
    if already_done "$ratio"; then
        log "SKIP (already 3 successful-or-attempted reps): ratio=$ratio arm=A random"
        continue
    fi
    budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    memmax=$((budget_bytes + MARGIN))
    for rep in 1 2 3; do
        fresh_cgroup "$memmax"
        drop_caches
        run_one "$ratio" "$rep" \
            "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$N_PASSES" random off
    done
done

rmdir "$CGROUP" 2>/dev/null
log "=== random-only pass complete ==="
uptime | tee -a "$LOG"
