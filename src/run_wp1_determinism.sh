#!/bin/bash
# WP1 §1.3 determinism check: layer_order_declared at Campaign 13 Phase A's
# exact A.2 grid -- the 6 cells crossing threads / window / compute -- n=5
# each, r=0.5, 128 MiB (cell 6 at 8 MiB). Report absent_handled for every
# rep of every cell. Expected: all reps identical within a cell, including
# cell 5 where layer_order_learned was non-deterministic.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results/overnight
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_wp1det
CSV=$RESULTS/wp1_determinism.csv
LOG=$RESULTS/wp1_determinism_log.txt

REGION_LEN=2147483648
BUDGET=1073741824
N_PASSES=5
MARGIN=67108864

log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$CSV" ]; then
    > "$LOG"; > "$CSV"
    echo "cell,threads,window,compute,chunk_size,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,pager_bytes_fetched,rc" > "$CSV"
    log "=== fresh start ==="
else
    log "=== resuming: existing CSV has $(wc -l < "$CSV") lines ==="
fi

log "=== machine exclusivity check (before) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -6 | tee -a "$LOG"

fresh_cgroup() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo "$((BUDGET + MARGIN))" > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
}
cleanup_cgroup() {
    for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
}
rep_done() {
    awk -F, -v c="$1" -v rp="$2" 'NR>1 && $1==c && $6==rp {found=1} END{exit !found}' "$CSV"
}

run_cell() {
    local cell=$1 threads=$2 window=$3 compute=$4 chunk=$5
    for rep in 1 2 3 4 5; do
        if rep_done "$cell" "$rep"; then log "SKIP (done): cell=$cell rep=$rep"; continue; fi
        fresh_cgroup
        out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 180 "$@"' -- \
              "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$chunk" "$BUDGET" "$N_PASSES" \
              layer_order_declared off "" "" --fetch-workers 4 --driver-threads "$threads" \
              --lookahead-window "$window" --compute-ns-per-mib "$compute" 2>&1)
        rc=$?
        log "--- cell=$cell threads=$threads window=$window compute=$compute chunk=$chunk rep=$rep rc=$rc ---"
        log "$out"
        t=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
        bt=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
        w=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
        a=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[0-9]+' | tail -1)
        e=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[0-9]+' | tail -1)
        inf=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[0-9]+' | tail -1)
        pbf=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
        echo "$cell,$threads,$window,$compute,$chunk,$rep,${t:-},${bt:-},${w:-},${a:-},${e:-},${inf:-},${pbf:-},$rc" >> "$CSV"
        cleanup_cgroup
    done
}

run_cell 1 1 0 0      134217728
run_cell 2 1 0 400000 134217728
run_cell 3 8 0 0      134217728
run_cell 4 8 1 0      134217728
run_cell 5 8 1 400000 134217728
run_cell 6 8 1 400000 8388608

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -6 | tee -a "$LOG"
log ""
log "=== per-cell absent_handled summary ==="
for c in 1 2 3 4 5 6; do
    vals=$(awk -F, -v c="$c" 'NR>1 && $1==c {printf "%s ", $10}' "$CSV")
    uniq=$(echo "$vals" | tr ' ' '\n' | sort -u | grep -c .)
    log "cell $c: [$vals] -> $([ "$uniq" = 1 ] && echo DETERMINISTIC || echo "NON-DETERMINISTIC ($uniq distinct)")"
done
log "=== WP1 §1.3 complete, results in $CSV ==="
