#!/bin/bash
# LIVELOCK FIX Phase 2 -- synthetic re-measurement.
# REVISED expectation: the synthetic (--consumption-signal all-threads) path
# must be UNCHANGED vs results/final/phase2_{determinism,sweep}.csv. Post-mode
# Defect 1 (d0=1) is a no-op refactor of the old `for d=1..seq_len` loop; any
# difference here is a regression to investigate, not a finding.
#
# A -- determinism grid: 6 A.2 cells, layer_order_declared, all-threads,
#      protect off, n=5. Compare absent_handled to phase2_determinism.csv.
# B -- sweep: layer_order_{declared,learned}, arm D, 8/128 MiB x r{.25,.5,.75}
#      x compute{0,400000}, all-threads, protect {off,on}, n=3. Compare
#      pager_bytes_fetched / absent_handled to phase2_sweep.csv (declared)
#      and compute D/OPT from phase2_opt.csv.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
OUT=$RESIDCTL/results/livelock
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_ll_p2
DCSV=$OUT/phase2_determinism.csv
SCSV=$OUT/phase2_sweep.csv
LOG=$OUT/phase2_console.txt

REGION_LEN=2147483648
MARGIN=67108864
N_PASSES=5

mkdir -p "$OUT"
log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$DCSV" ]; then
    > "$LOG"
    echo "cell,threads,window,compute,chunk_size,rep,touches,absent_handled,evictions,infeasible,pager_bytes_fetched,signal_mode,rc" > "$DCSV"
    echo "policy,protect,chunk_size,ratio,compute,rep,touches,absent_handled,evictions,pager_bytes_fetched,io_delta,signal_mode,rc" > "$SCSV"
    log "=== fresh start $(date -u +%FT%TZ) ==="
else
    log "=== resume $(date -u +%FT%TZ): det=$(wc -l < "$DCSV") sweep=$(wc -l < "$SCSV") ==="
fi

log "=== machine exclusivity (before) ==="
uptime | tee -a "$LOG"
pgrep -af "cn-spike|iperf3|gate5|wp2_gen" | tee -a "$LOG" && log "WARNING other workload" || log "(clean)"

fresh() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"; echo "$1" > "$CGROUP/memory.max"; echo 0 > "$CGROUP/memory.swap.max"
}
cleanup() { for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done; }
g() { echo "$1" | grep -oP "$2=\K[-0-9]+" | tail -1; }
gs() { echo "$1" | grep -oP "$2=\K[a-z-]+" | tail -1; }

# ---- Part A: determinism -----------------------------------------------
det_done() { awk -F, -v c="$1" -v rp="$2" 'NR>1 && $1==c && $6==rp {f=1} END{exit !f}' "$DCSV"; }
run_det() {
    local cell=$1 threads=$2 window=$3 compute=$4 chunk=$5 budget=1073741824
    for rep in 1 2 3 4 5; do
        det_done "$cell" "$rep" && { log "SKIP det cell$cell rep$rep"; continue; }
        fresh "$((budget + MARGIN))"
        local out rc
        out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 200 "$@"' -- \
              "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$chunk" "$budget" "$N_PASSES" \
              layer_order_declared off "" "" --fetch-workers 4 --driver-threads "$threads" \
              --lookahead-window "$window" --compute-ns-per-mib "$compute" \
              --consumption-signal all-threads --protect-current off 2>&1)
        rc=$?
        local t a e inf pbf sm
        t=$(g "$out" touches); a=$(g "$out" absent_handled); e=$(g "$out" evictions)
        inf=$(g "$out" infeasible); pbf=$(g "$out" pager_bytes_fetched); sm=$(gs "$out" signal_mode)
        log "det cell$cell rep$rep rc=$rc absent_handled=$a evictions=$e pbf=$pbf signal_mode=$sm"
        echo "$cell,$threads,$window,$compute,$chunk,$rep,${t:-},${a:-},${e:-},${inf:-},${pbf:-},${sm:-},$rc" >> "$DCSV"
        cleanup
    done
}
run_det 1 1 0 0      134217728
run_det 2 1 0 400000 134217728
run_det 3 8 0 0      134217728
run_det 4 8 1 0      134217728
run_det 5 8 1 400000 134217728
run_det 6 8 1 400000 8388608

# ---- Part B: sweep ----------------------------------------------------
sw_done() { awk -F, -v p="$1" -v pr="$2" -v cs="$3" -v r="$4" -v cp="$5" -v rp="$6" \
    'NR>1 && $1==p && $2==pr && $3==cs && $4==r && $5==cp && $6==rp {f=1} END{exit !f}' "$SCSV"; }
run_sw() {
    local policy=$1 protect=$2 cs=$3 ratio=$4 compute=$5
    local budget; budget=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    for rep in 1 2 3; do
        sw_done "$policy" "$protect" "$cs" "$ratio" "$compute" "$rep" && { log "SKIP sw $policy $protect $cs $ratio $compute rep$rep"; continue; }
        fresh "$((budget + MARGIN))"
        local out rc
        out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 240 "$@"' -- \
              "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget" "$N_PASSES" \
              "$policy" off "" "" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 \
              --prefetch-retention pinned --compute-ns-per-mib "$compute" \
              --consumption-signal all-threads --protect-current "$protect" 2>&1)
        rc=$?
        local t a e pbf iod sm
        t=$(g "$out" touches); a=$(g "$out" absent_handled); e=$(g "$out" evictions)
        pbf=$(g "$out" pager_bytes_fetched); iod=$(g "$out" io_read_bytes_delta); sm=$(gs "$out" signal_mode)
        log "sw $policy protect=$protect cs=$cs r=$ratio c=$compute rep$rep rc=$rc absent_handled=$a pbf=$pbf signal_mode=$sm"
        echo "$policy,$protect,$cs,$ratio,$compute,$rep,${t:-},${a:-},${e:-},${pbf:-},${iod:-},${sm:-},$rc" >> "$SCSV"
        cleanup
    done
}
for policy in layer_order_declared layer_order_learned; do
    protects="off on"
    [ "$policy" = layer_order_learned ] && protects="off"   # --protect-current is a no-op for learned
    for protect in $protects; do
        for cs in 8388608 134217728; do
            for ratio in 0.25 0.5 0.75; do
                for compute in 0 400000; do
                    run_sw "$policy" "$protect" "$cs" "$ratio" "$compute"
                done
            done
        done
    done
done

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity (after) ==="; uptime | tee -a "$LOG"
log "=== Phase 2 complete $(date -u +%FT%TZ): $DCSV / $SCSV ==="
