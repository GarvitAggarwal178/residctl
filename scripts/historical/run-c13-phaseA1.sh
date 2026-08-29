#!/bin/bash
# Campaign 13 Phase A.1: reproduce arm D at 128MiB/r=0.5/compute=400000,
# n=10, report every rep individually.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_c13a1
CSV=$RESULTS/campaign13_phaseA1_reproduce.csv
LOG=$RESULTS/campaign13_phaseA1_reproduce_log.txt

REGION_LEN=2147483648
CHUNK_SIZE=134217728
BUDGET=1073741824
N_PASSES=5
MARGIN=67108864

log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$CSV" ]; then
    > "$LOG"
    > "$CSV"
    echo "rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,pager_bytes_fetched,io_read_bytes_delta,rc" > "$CSV"
    log "=== fresh start ==="
else
    log "=== resuming: existing CSV has $(wc -l < "$CSV") lines ==="
fi

log "=== machine exclusivity check ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"

fresh_cgroup() {
    if [ -d "$CGROUP" ]; then
        local procs
        procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
        if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
        rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo "$((BUDGET + MARGIN))" > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
}
cleanup_cgroup() {
    local procs
    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
}

rep_done() {
    local rp=$1
    awk -F, -v rp="$rp" 'NR>1 && $1==rp {found=1} END{exit !found}' "$CSV"
}

for rep in 1 2 3 4 5 6 7 8 9 10; do
    if rep_done "$rep"; then
        log "SKIP (done): rep=$rep"
        continue
    fi
    fresh_cgroup
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 180 "$@"' -- \
          "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$BUDGET" "$N_PASSES" \
          layer_order off "" "" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 \
          --compute-ns-per-mib 400000 2>&1)
    rc=$?
    log "--- rep=$rep rc=$rc ---"
    log "$out"

    touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
    wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    absent=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[0-9]+' | tail -1)
    evictions=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[0-9]+' | tail -1)
    infeasible=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[0-9]+' | tail -1)
    pbf=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
    iord=$(echo "$out" | grep -oP 'io_read_bytes_delta=\K[0-9]+' | tail -1)

    echo "$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${absent:-},${evictions:-},${infeasible:-},${pbf:-},${iord:-},$rc" >> "$CSV"
    cleanup_cgroup
done

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
log "=== A.1 complete, results in $CSV ==="
