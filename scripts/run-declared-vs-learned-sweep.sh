#!/bin/bash
# WP1 §1.4 learned-vs-declared sweep. Arms C, D (both policies), E (both
# policies), OPT. Grid: chunk {8 MiB, 128 MiB} x ratio {0.25, 0.5, 0.75} x
# --compute-ns-per-mib {0, 400000}, n=3. Fixed: async handler,
# --fetch-workers 4 --driver-threads 8 --lookahead-window 1
# --prefetch-depth 2 --prefetch-retention pinned. Arms A/B skipped per spec
# (policy comparison, baseline unchanged).
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results/overnight
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_wp1sweep
CSV=$RESULTS/wp1_sweep.csv
OPTCSV=$RESULTS/wp1_sweep_opt.csv
LOG=$RESULTS/wp1_sweep_log.txt

REGION_LEN=2147483648
N_PASSES=5
MARGIN=67108864
CHUNK_SIZES="8388608 134217728"
RATIOS="0.25 0.5 0.75"
COMPUTES="0 400000"

log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$CSV" ]; then
    > "$LOG"; > "$CSV"; > "$OPTCSV"
    echo "chunk_size,ratio,arm,policy,compute,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta,dedup_resident,dedup_fetching,pin_broken,rc" > "$CSV"
    echo "chunk_size,ratio,budget_bytes,capacity_chunks,opt_misses,opt_bytes" > "$OPTCSV"
    log "=== fresh start ==="
else
    log "=== resuming: existing CSV has $(wc -l < "$CSV") lines ==="
fi

log "=== machine exclusivity check (before) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -6 | tee -a "$LOG"
if pgrep -f "cn-spike|gate5|iperf3" > /dev/null 2>&1; then
    log "FATAL: foreign workload present -- stop and report per runbook"; exit 1
fi

fresh_cgroup() {
    local max=$1
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo "$max" > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
}
cleanup_cgroup() {
    for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
}
rep_done() {
    awk -F, -v cs="$1" -v r="$2" -v a="$3" -v p="$4" -v c="$5" -v rp="$6" \
        'NR>1 && $1==cs && $2==r && $3==a && $4==p && $5==c && $6==rp {f=1} END{exit !f}' "$CSV"
}

run_row() {
    local cs=$1 ratio=$2 arm=$3 policy=$4 compute=$5 rep=$6; shift 6
    local out rc
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 220 "$@"' -- "$@" 2>&1)
    rc=$?
    log "--- cs=$cs ratio=$ratio arm=$arm policy=$policy compute=$compute rep=$rep rc=$rc ---"
    log "$out"
    local t bt w a e inf pf pbf iord dr df pb
    t=$(echo "$out"   | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
    bt=$(echo "$out"  | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
    w=$(echo "$out"   | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
    a=$(echo "$out"   | grep -oP 'ARM_CSV,.*absent_handled=\K[0-9]+' | tail -1)
    e=$(echo "$out"   | grep -oP 'ARM_CSV,.*evictions=\K[0-9]+' | tail -1)
    inf=$(echo "$out" | grep -oP 'ARM_CSV,.*infeasible=\K[0-9]+' | tail -1)
    pf=$(echo "$out"  | grep -oP 'ARM_CSV,.*prefetches=\K[0-9]+' | tail -1)
    pbf=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
    iord=$(echo "$out"| grep -oP 'io_read_bytes_delta=\K[0-9]+' | tail -1)
    dr=$(echo "$out"  | grep -oP 'ARM_CSV,.*dedup_resident=\K[0-9]+' | tail -1)
    df=$(echo "$out"  | grep -oP 'ARM_CSV,.*dedup_fetching=\K[0-9]+' | tail -1)
    pb=$(echo "$out"  | grep -oP 'ARM_CSV,.*pin_broken=\K[0-9]+' | tail -1)
    echo "$cs,$ratio,$arm,$policy,$compute,$rep,${t:-},${bt:-},${w:-},${a:-},${e:-},${inf:-},${pf:-},${pbf:-},${iord:-},${dr:-},${df:-},${pb:-},$rc" >> "$CSV"
    cleanup_cgroup
}

for cs in $CHUNK_SIZES; do
    for ratio in $RATIOS; do
        budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
        memmax=$((budget_bytes + MARGIN))
        reftrace="$SCRATCH/wp1_ref_cs${cs}_r${ratio}.reftrace"

        for compute in $COMPUTES; do
            # Arm C -- lru, prefetch off
            for rep in 1 2 3; do
                rep_done "$cs" "$ratio" C lru "$compute" "$rep" && { log "SKIP C $cs $ratio $compute $rep"; continue; }
                fresh_cgroup "$memmax"
                run_row "$cs" "$ratio" C lru "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    lru off "" "" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 \
                    --prefetch-retention pinned --compute-ns-per-mib "$compute"
            done
            # Arm D -- layer_order_{learned,declared}, prefetch off
            for pol in layer_order_learned layer_order_declared; do
                for rep in 1 2 3; do
                    rep_done "$cs" "$ratio" D "$pol" "$compute" "$rep" && { log "SKIP D $pol $cs $ratio $compute $rep"; continue; }
                    fresh_cgroup "$memmax"
                    rt=""
                    # capture one reference trace per (cs,ratio) from declared/compute=0/rep1
                    if [ "$pol" = layer_order_declared ] && [ "$compute" = 0 ] && [ "$rep" = 1 ]; then rt="$reftrace"; rm -f "$rt"; fi
                    run_row "$cs" "$ratio" D "$pol" "$compute" "$rep" \
                        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                        "$pol" off "" "$rt" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 \
                        --prefetch-retention pinned --compute-ns-per-mib "$compute"
                done
            done
            # Arm E -- layer_order_{learned,declared}, prefetch on, depth 2, retention pinned
            for pol in layer_order_learned layer_order_declared; do
                for rep in 1 2 3; do
                    rep_done "$cs" "$ratio" E "$pol" "$compute" "$rep" && { log "SKIP E $pol $cs $ratio $compute $rep"; continue; }
                    fresh_cgroup "$memmax"
                    run_row "$cs" "$ratio" E "$pol" "$compute" "$rep" \
                        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                        "$pol" on "" "" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 \
                        --prefetch-depth 2 --prefetch-retention pinned --compute-ns-per-mib "$compute"
                done
            done
        done

        # OPT for this (cs, ratio)
        if [ -f "$reftrace" ] && ! awk -F, -v cs="$cs" -v r="$ratio" 'NR>1 && $1==cs && $2==r {f=1} END{exit !f}' "$OPTCSV"; then
            optout=$("$SRC/belady_main" "$reftrace" "$cs" "$budget_bytes" 2>&1)
            log "--- OPT cs=$cs ratio=$ratio ---"
            log "$optout"
            cap=$(echo "$optout" | grep -oP 'capacity=\K[0-9]+' | tail -1)
            misses=$(echo "$optout" | grep -oP 'minimum_misses=\K[0-9]+' | tail -1)
            obytes=$(echo "$optout" | grep -oP 'minimum_bytes_fetched=\K[0-9]+' | tail -1)
            echo "$cs,$ratio,$budget_bytes,${cap:-},${misses:-},${obytes:-}" >> "$OPTCSV"
        fi
    done
done

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -6 | tee -a "$LOG"
log "=== WP1 §1.4 sweep complete: $CSV / $OPTCSV ==="
