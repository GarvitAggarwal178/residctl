#!/bin/bash
# FINAL SESSION Phase 2 -- the exact consumption signal.
#
# Tests --consumption-signal {tid0,all-threads} x --protect-current {on,off}
# on the synthetic replay driver (replay_main + scratch/pattern_2g.bin), the
# same platform WP1 used. Four combos:
#   tid0 + on           -- current shipped default (baseline for the comparison)
#   tid0 + off          -- session-1 behaviour, the known-broken case
#   all-threads + off   -- the intended fix, alone
#   all-threads + on    -- both together
#
# Part A -- determinism: WP1 s1.3's A.2 six cells, n=5, report absent_handled
#   per rep. Expectation 1: all-threads+off is deterministic at all six.
# Part B -- sweep: 8 MiB & 128 MiB x r{0.25,0.5,0.75} x compute{0,400000},
#   arm D, n=3. Report read_bytes and D/OPT.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
OUT=$RESIDCTL/results/final
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_final_p2
DCSV=$OUT/phase2_determinism.csv
SCSV=$OUT/phase2_sweep.csv
OPTCSV=$OUT/phase2_opt.csv
LOG=$OUT/phase2_log.txt

REGION_LEN=2147483648
MARGIN=67108864
N_PASSES_DET=5
N_REPS_SWEEP=3

log() { echo "$@" | tee -a "$LOG"; }

combo_args() { # $1 = combo name -> echoes the two flags
    case "$1" in
        tid0_on)         echo "--consumption-signal tid0 --protect-current on" ;;
        tid0_off)        echo "--consumption-signal tid0 --protect-current off" ;;
        allthreads_off)  echo "--consumption-signal all-threads --protect-current off" ;;
        allthreads_on)   echo "--consumption-signal all-threads --protect-current on" ;;
    esac
}
COMBOS="tid0_on tid0_off allthreads_off allthreads_on"

if [ ! -f "$DCSV" ]; then
    > "$LOG"
    echo "combo,cell,threads,window,compute,chunk_size,rep,touches,absent_handled,evictions,infeasible,pager_bytes_fetched,rc" > "$DCSV"
    echo "combo,chunk_size,ratio,compute,rep,touches,absent_handled,evictions,pager_bytes_fetched,io_read_bytes_delta,wall_ns,rc" > "$SCSV"
    echo "chunk_size,ratio,budget_bytes,capacity_chunks,opt_misses,opt_bytes" > "$OPTCSV"
    log "=== fresh start ==="
else
    log "=== resume: det=$(wc -l < "$DCSV") sweep=$(wc -l < "$SCSV") ==="
fi

log "=== machine exclusivity (before) ==="
uptime | tee -a "$LOG"; ps aux --sort=-%cpu 2>/dev/null | head -5 | tee -a "$LOG"
pgrep -af "cn-spike|iperf3|gate5|wp2_gen" && log "WARNING foreign/other workload" || log "(clean)"

fresh() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"; echo "$1" > "$CGROUP/memory.max"; echo 0 > "$CGROUP/memory.swap.max"
}
cleanup() { for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done; }
g() { echo "$1" | grep -oP "$2=\K[0-9]+" | tail -1; }

# ---- Part A: determinism ------------------------------------------------
det_done() { awk -F, -v c="$1" -v cl="$2" -v rp="$3" 'NR>1 && $1==c && $2==cl && $7==rp {f=1} END{exit !f}' "$DCSV"; }
run_det_cell() {
    local combo=$1 cell=$2 threads=$3 window=$4 compute=$5 chunk=$6
    local budget=1073741824
    local flags; flags=$(combo_args "$combo")
    for rep in 1 2 3 4 5; do
        det_done "$combo" "$cell" "$rep" && { log "SKIP det $combo cell$cell rep$rep"; continue; }
        fresh "$((budget + MARGIN))"
        local out rc
        out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 200 "$@"' -- \
              "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$chunk" "$budget" "$N_PASSES_DET" \
              layer_order_declared off "" "" --fetch-workers 4 --driver-threads "$threads" \
              --lookahead-window "$window" --compute-ns-per-mib "$compute" $flags 2>&1)
        rc=$?
        log "--- det $combo cell$cell t=$threads w=$window c=$compute cs=$chunk rep$rep rc=$rc ---"
        echo "$out" | grep -E "ARM_CSV|consumption_signal|abort|FAIL" | tee -a "$LOG" >/dev/null
        local t a e inf pbf
        t=$(echo "$out"|grep -oP 'ARM_CSV,.*touches=\K[0-9]+'|tail -1)
        a=$(echo "$out"|grep -oP 'ARM_CSV,.*absent_handled=\K[0-9]+'|tail -1)
        e=$(echo "$out"|grep -oP 'ARM_CSV,.*evictions=\K[0-9]+'|tail -1)
        inf=$(echo "$out"|grep -oP 'ARM_CSV,.*infeasible=\K[0-9]+'|tail -1)
        pbf=$(echo "$out"|grep -oP 'pager_bytes_fetched=\K[0-9]+'|tail -1)
        echo "$combo,$cell,$threads,$window,$compute,$chunk,$rep,${t:-},${a:-},${e:-},${inf:-},${pbf:-},$rc" >> "$DCSV"
        cleanup
    done
}

for combo in $COMBOS; do
    run_det_cell "$combo" 1 1 0 0      134217728
    run_det_cell "$combo" 2 1 0 400000 134217728
    run_det_cell "$combo" 3 8 0 0      134217728
    run_det_cell "$combo" 4 8 1 0      134217728
    run_det_cell "$combo" 5 8 1 400000 134217728
    run_det_cell "$combo" 6 8 1 400000 8388608
done

# ---- OPT for the sweep grid (combo-independent: same declared cyclic order) --
for cs in 8388608 134217728; do
    for ratio in 0.25 0.5 0.75; do
        budget=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
        awk -F, -v cs="$cs" -v r="$ratio" 'NR>1 && $1==cs && $2==r {f=1} END{exit !f}' "$OPTCSV" && continue
        rt="$SCRATCH/p2_ref_cs${cs}_r${ratio}.reftrace"; rm -f "$rt"
        fresh "$((budget + MARGIN))"
        bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 200 "$@"' -- \
            "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget" 5 \
            layer_order_declared off "" "$rt" --fetch-workers 4 --driver-threads 8 \
            --lookahead-window 1 --compute-ns-per-mib 0 --consumption-signal all-threads --protect-current off >/dev/null 2>&1
        cleanup
        optout=$("$SRC/belady_main" "$rt" "$cs" "$budget" 2>&1)
        log "--- OPT cs=$cs r=$ratio ---"; echo "$optout" | tee -a "$LOG" >/dev/null
        cap=$(echo "$optout"|grep -oP 'capacity=\K[0-9]+'|tail -1)
        m=$(echo "$optout"|grep -oP 'minimum_misses=\K[0-9]+'|tail -1)
        ob=$(echo "$optout"|grep -oP 'minimum_bytes_fetched=\K[0-9]+'|tail -1)
        echo "$cs,$ratio,$budget,${cap:-},${m:-},${ob:-}" >> "$OPTCSV"
    done
done

# ---- Part B: sweep ----------------------------------------------------------
sw_done() { awk -F, -v c="$1" -v cs="$2" -v r="$3" -v cp="$4" -v rp="$5" 'NR>1 && $1==c && $2==cs && $3==r && $4==cp && $5==rp {f=1} END{exit !f}' "$SCSV"; }
run_sweep() {
    local combo=$1 cs=$2 ratio=$3 compute=$4
    local budget; budget=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
    local flags; flags=$(combo_args "$combo")
    for rep in $(seq 1 $N_REPS_SWEEP); do
        sw_done "$combo" "$cs" "$ratio" "$compute" "$rep" && { log "SKIP sweep $combo $cs $ratio $compute rep$rep"; continue; }
        fresh "$((budget + MARGIN))"
        local out rc
        out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 220 "$@"' -- \
              "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$budget" 5 \
              layer_order_declared off "" "" --fetch-workers 4 --driver-threads 8 --lookahead-window 1 \
              --prefetch-retention pinned --compute-ns-per-mib "$compute" $flags 2>&1)
        rc=$?
        log "--- sweep $combo cs=$cs r=$ratio c=$compute rep$rep rc=$rc ---"
        local t a e pbf iord w
        t=$(echo "$out"|grep -oP 'ARM_CSV,.*touches=\K[0-9]+'|tail -1)
        a=$(echo "$out"|grep -oP 'ARM_CSV,.*absent_handled=\K[0-9]+'|tail -1)
        e=$(echo "$out"|grep -oP 'ARM_CSV,.*evictions=\K[0-9]+'|tail -1)
        pbf=$(echo "$out"|grep -oP 'pager_bytes_fetched=\K[0-9]+'|tail -1)
        iord=$(echo "$out"|grep -oP 'io_read_bytes_delta=\K[0-9]+'|tail -1)
        w=$(echo "$out"|grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+'|tail -1)
        echo "$combo,$cs,$ratio,$compute,$rep,${t:-},${a:-},${e:-},${pbf:-},${iord:-},${w:-},$rc" >> "$SCSV"
        cleanup
    done
}
for combo in $COMBOS; do
    for cs in 8388608 134217728; do
        for ratio in 0.25 0.5 0.75; do
            for compute in 0 400000; do
                run_sweep "$combo" "$cs" "$ratio" "$compute"
            done
        done
    done
done

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity (after) ==="; uptime | tee -a "$LOG"
log "=== Phase 2 complete: $DCSV / $SCSV / $OPTCSV ==="
