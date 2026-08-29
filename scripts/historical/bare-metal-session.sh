#!/bin/bash
# bare_metal_session.sh -- Campaign 13 Phase D bare-metal readiness
# script. Runs build, S0-S3 pre-flight, T-1..T-7, environment capture,
# and the full 240-run sweep, unattended, writing one CSV. Designed to
# be interrupted and resumed (per-rep resumability, same pattern this
# project's every prior sweep script uses). Run from the residctl repo
# root: bash src/bare_metal_session.sh
set -u
RESIDCTL="${RESIDCTL:-$HOME/residctl}"
SPIKE="${SPIKE:-$HOME/spike}"
SRC="$RESIDCTL/src"
RESULTS="$RESIDCTL/results"
SCRATCH="$RESIDCTL/scratch"
MODEL="$SCRATCH/pattern_2g.bin"
CGROUP=/sys/fs/cgroup/residctl_baremetal
CSV="$RESULTS/bare_metal_sweep.csv"
LOG="$RESULTS/bare_metal_sweep_log.txt"
mkdir -p "$RESULTS" "$SCRATCH"

echo "=== Step 1: build ==="
( cd "$SRC" && make clean && make all ) || { echo "BUILD FAILED"; exit 1; }

echo "=== Step 2: environment baseline ==="
[ -f "$MODEL" ] || "$SRC/gen_pattern" "$MODEL" 2147483648
bash "$SRC/bare_metal_env_baseline.sh" "$MODEL"

echo "=== Step 3: S0-S3 pre-flight ==="
( cd "$SPIKE/src" && gcc -O2 -o s0_uffd_probe s0_uffd_probe.c -pthread \
    && ./s0_uffd_probe ) || { echo "S0 FAILED -- stop and report"; exit 1; }
( cd "$SPIKE/src" && bash run_s1.sh ) || { echo "S1 FAILED -- stop and report"; exit 1; }
( cd "$SPIKE/src" && bash run_s2.sh ) || { echo "S2 FAILED -- stop and report"; exit 1; }
( cd "$SPIKE/src" && bash s3_setup_cgroup.sh && ./s3_eviction ) || { echo "S3 FAILED -- stop and report"; exit 1; }
echo "S0-S3 passed -- see $SPIKE/results/ for full logs. Proceeding."

echo "=== Step 4: T-1..T-7 correctness harness ==="
bash "$SRC/run_correctness_harness.sh" || { echo "T-1..T-5 FAILED -- stop and report"; exit 1; }
bash "$SRC/run_t6_t7.sh" || { echo "T-6/T-7 FAILED -- stop and report"; exit 1; }
grep -q "RESULT: PASS" "$RESULTS/correctness_harness_log.txt" || { echo "T-1..T-5 did not PASS"; exit 1; }
grep -q "RESULT: PASS" "$RESULTS/t6_t7_log.txt" || { echo "T-6/T-7 did not PASS"; exit 1; }
echo "T-1..T-7 all PASS. Proceeding to sweep."

echo "=== Step 5: machine exclusivity check ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"

REGION_LEN=2147483648
N_PASSES=5
MARGIN=67108864
CHUNK_SIZES="8388608 134217728"
RATIOS="0.25 0.375 0.5 0.625 0.75"
COMPUTES="0 400000"

log() { echo "$@" | tee -a "$LOG"; }

if [ ! -f "$CSV" ]; then
    > "$LOG"; > "$CSV"
    echo "chunk_size,ratio,arm,detail,compute,rep,touches,bytes_touched,wall_ns,absent_handled,evictions,infeasible,prefetches,pager_bytes_fetched,io_read_bytes_delta,dedup_resident,dedup_fetching,pin_broken,rc" > "$CSV"
    log "=== fresh start ==="
else
    log "=== resuming: existing CSV has $(wc -l < "$CSV") lines ==="
fi

fresh_cgroup() {
    local max=$1
    if [ -d "$CGROUP" ]; then
        local procs; procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
        if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
        rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo "$max" > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
    [ "$(cat "$CGROUP/memory.swap.max")" = "0" ] || { echo "FATAL: memory.swap.max not 0 (I-3 violated)"; exit 1; }
}
cleanup_cgroup() {
    local procs; procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
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
    log "--- cs=$cs ratio=$ratio arm=$arm detail=$detail compute=$compute rep=$rep rc=$rc ---"
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

        for rep in 1 2 3; do
            if rep_done "$cs" "$ratio" "A" "sequential" "n/a" "$rep"; then log "SKIP: A rep=$rep"; continue; fi
            fresh_cgroup "$memmax"; drop_caches
            run_row "$cs" "$ratio" "A" "sequential" "n/a" "$rep" \
                "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$N_PASSES" sequential off
        done
        for rep in 1 2 3; do
            if rep_done "$cs" "$ratio" "B" "normal" "n/a" "$rep"; then log "SKIP: B rep=$rep"; continue; fi
            fresh_cgroup "$memmax"; drop_caches
            run_row "$cs" "$ratio" "B" "normal" "n/a" "$rep" \
                "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$N_PASSES" normal on
        done

        for compute in $COMPUTES; do
            for rep in 1 2 3; do
                if rep_done "$cs" "$ratio" "C" "lru" "$compute" "$rep"; then log "SKIP: C rep=$rep"; continue; fi
                fresh_cgroup "$memmax"
                trace="$SCRATCH/baremetal_C_cs${cs}_c${compute}_r${ratio}_rep${rep}.fetchtrace"; rm -f "$trace"
                run_row "$cs" "$ratio" "C" "lru" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    lru off "" "" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --compute-ns-per-mib "$compute"
            done
            for rep in 1 2 3; do
                if rep_done "$cs" "$ratio" "D" "layer_order" "$compute" "$rep"; then log "SKIP: D rep=$rep"; continue; fi
                fresh_cgroup "$memmax"
                trace="$SCRATCH/baremetal_D_cs${cs}_c${compute}_r${ratio}_rep${rep}.fetchtrace"
                reftrace="$SCRATCH/baremetal_D_cs${cs}_c${compute}_r${ratio}_rep${rep}.reftrace"
                rm -f "$trace" "$reftrace"
                run_row "$cs" "$ratio" "D" "layer_order" "$compute" "$rep" \
                    "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget_bytes" "$N_PASSES" \
                    layer_order off "" "$reftrace" --fetch-trace "$trace" --fetch-workers 4 --driver-threads 8 \
                    --lookahead-window 1 --compute-ns-per-mib "$compute"
            done
            for rep in 1 2 3; do
                if rep_done "$cs" "$ratio" "E" "layer_order" "$compute" "$rep"; then log "SKIP: E rep=$rep"; continue; fi
                fresh_cgroup "$memmax"
                trace="$SCRATCH/baremetal_E_cs${cs}_c${compute}_r${ratio}_rep${rep}.fetchtrace"; rm -f "$trace"
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
log "=== bare-metal sweep complete, results in $CSV ==="
