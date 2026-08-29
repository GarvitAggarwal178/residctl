#!/bin/bash
# LIVELOCK FIX Phase 3 -- real-model re-measurement, all four fixes in.
#
# Correctness gate FIRST: 32 tokens, mmap vs residctl, byte-identical token
# sequences. Defect 2 changes WHEN the notify fires, not what is computed. If
# the sequences differ -> STOP and report.
#
# Then arms C/D/E x r{0.25,0.375,0.5,0.625,0.75}, n=3, 64 tokens, fixed prompt,
# greedy. Equal budget exactly as final-session Phase 1: budget_bytes = B +
# uniform 128 MiB memory.max margin for the pager arms. --protect-current off.
# Arm A is unchanged by these fixes -> reuse results/data/real-model-bytes-by-budget.csv.
#
# Arm E at r=0.25 is the run most likely to livelock. A SIGUSR1 + policy-trace
# watchdog is wired from the start so expectation 1's evidence is captured
# without a re-run. If arm E still livelocks: report, STOP, no further fix.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
OUT=$RESIDCTL/results/livelock
MODEL=$RESIDCTL/models/model.gguf
CGROUP=/sys/fs/cgroup/residctl_ll_p3
CSV=$OUT/phase3_real_model.csv
GATE=$OUT/phase3_correctness_gate.txt
LOG=$OUT/phase3_console.txt
PROMPT="In a distant future, humanity spread across the stars, and the old questions returned"
N_TOK=64
NREPS=3
MARGIN=134217728
TIMEOUT=420
WATCHDOG=140

mkdir -p "$OUT"
log() { echo "$@" | tee -a "$LOG"; }
[ -f "$MODEL" ] || { log "no model"; exit 1; }
FSIZE=$(stat -c %s "$MODEL"); REGION=$(( (FSIZE + 4095) / 4096 * 4096 ))

fresh() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"; echo "$1" > "$CGROUP/memory.max"; echo 0 > "$CGROUP/memory.swap.max"
    echo 0 > "$CGROUP/memory.peak" 2>/dev/null || true
}
cleanup() { for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done; }
field() { echo "$1" | grep -oP "$2=\K[-0-9.]+" | tail -1; }

# ============ correctness gate ============
if [ ! -f "$GATE" ]; then
    > "$LOG"
    log "=== correctness gate: mmap vs residctl, 32 tokens, byte-identical ==="
    log "=== machine exclusivity (before) ==="; uptime | tee -a "$LOG"
    pgrep -af "cn-spike|iperf3|gate5" && log "WARNING foreign workload" || log "(clean)"
    RG=0.75
    B=$(awk "BEGIN{printf \"%d\", $REGION*$RG}")
    fresh "$B"
    sync; echo 3 > /proc/sys/vm/drop_caches
    bash -c "echo \$BASHPID > $CGROUP/cgroup.procs; exec timeout $TIMEOUT $SRC/wp2_gen -m $MODEL -n 32 -p \"\$1\" -t 8 --dump-tokens $OUT/gate_mmap.tok" _ "$PROMPT" > /dev/null 2>&1
    cleanup
    cfg=$OUT/phase3_gate.cfg
    cat > "$cfg" <<EOF
model=$MODEL
cgroup=$CGROUP
budget_bytes=$B
policy=layer_order_declared
prefetch=off
fetch_workers=4
eager_reconcile=0
protect_current=off
EOF
    fresh "$(( B + MARGIN ))"
    sync; echo 3 > /proc/sys/vm/drop_caches
    bash -c "echo \$BASHPID > $CGROUP/cgroup.procs; RESIDCTL_CONFIG=$cfg exec timeout $TIMEOUT $SRC/wp2_gen -m $MODEL -n 32 -p \"\$1\" -t 8 --dump-tokens $OUT/gate_residctl.tok" _ "$PROMPT" > /dev/null 2>&1
    cleanup
    rmdir "$CGROUP" 2>/dev/null
    {
        echo "mmap:     $(cat $OUT/gate_mmap.tok)"
        echo "residctl: $(cat $OUT/gate_residctl.tok)"
        if diff -q "$OUT/gate_mmap.tok" "$OUT/gate_residctl.tok" > /dev/null; then
            echo "GATE: PASS -- byte-identical token sequences"
        else
            echo "GATE: FAIL -- token sequences differ. STOP. Defect 2's pre-compute notify is not compute-equivalent."
        fi
    } | tee "$GATE" | tee -a "$LOG"
    if grep -q "GATE: FAIL" "$GATE"; then exit 1; fi
else
    log "=== resume: correctness gate already $(grep -o 'PASS\|FAIL' "$GATE" | head -1) ==="
    grep -q "GATE: FAIL" "$GATE" && { log "gate previously FAILED -- not proceeding"; exit 1; }
fi

# ============ arms C/D/E sweep ============
if [ ! -f "$CSV" ]; then
    echo "ratio,arm,policy,prefetch,rep,B_bytes,budget_bytes,memory_max,n_prompt,n_decoded,load_s,ttft_ms,p99_inter_ms,wall_s,tokens_s,io_gen_bytes,pager_bytes_fetched,absent_handled,evictions,prefetches,infeasible,pin_broken,dedup_fetching,prefetch_declined,fetching_timeout,resident_end,memory_peak,protect_current,signal_mode,rc" > "$CSV"
    log "=== sweep fresh start === REGION=$REGION MARGIN=$((MARGIN/1048576))MiB"
else
    log "=== sweep resume: $(wc -l < "$CSV") lines ==="
fi

rep_done() { awk -F, -v r="$1" -v a="$2" -v rp="$3" 'NR>1 && $1==r && $2==a && $5==rp {f=1} END{exit !f}' "$CSV"; }

run_one() {
    local ratio=$1 arm=$2 policy=$3 prefetch=$4 rep=$5 trace=${6:-}
    rep_done "$ratio" "$arm" "$rep" && { log "SKIP $ratio $arm rep$rep"; return; }
    local B budget memmax
    B=$(awk "BEGIN{printf \"%d\", $REGION*$ratio}")
    budget=$B; memmax=$(( B + MARGIN ))
    fresh "$memmax"
    sync; echo 3 > /proc/sys/vm/drop_caches
    local cfg=$OUT/phase3_${arm}_r${ratio}.cfg
    cat > "$cfg" <<EOF
model=$MODEL
cgroup=$CGROUP
budget_bytes=$budget
policy=$policy
prefetch=$prefetch
prefetch_depth=2
retention=pinned
fetch_workers=4
eager_reconcile=0
protect_current=off
EOF
    [ "$arm" = D ] && [ "$rep" = 1 ] && echo "reftrace=$OUT/phase3_reftrace_r${ratio}.bin" >> "$cfg"
    if [ "$trace" = trace ]; then
        echo "policytrace=$OUT/phase3_${arm}_r${ratio}_rep${rep}_policy.trace" >> "$cfg"
    fi

    # SIGUSR1 + wchan watchdog (arm E especially)
    ( sleep $WATCHDOG
      pid=$(cat "$CGROUP/cgroup.procs" 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+$' | tail -1)
      if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        log ">>> WATCHDOG t=${WATCHDOG}s $ratio $arm rep$rep pid=$pid: SIGUSR1"
        kill -USR1 "$pid" 2>/dev/null; sleep 0.5
        for t in /proc/$pid/task/*/wchan; do [ -r "$t" ] && log "   tid $(basename $(dirname $t)) wchan=$(cat $t 2>/dev/null)"; done
        cat "$CGROUP/memory.events" 2>/dev/null | sed 's/^/   mem.events /' | tee -a "$LOG" >/dev/null
      fi
    ) & local wd=$!

    local out rc
    out=$(bash -c "echo \$BASHPID > $CGROUP/cgroup.procs; RESIDCTL_CONFIG=$cfg exec timeout $TIMEOUT $SRC/wp2_gen -m $MODEL -n $N_TOK -p \"\$1\" -t 8" _ "$PROMPT" 2>&1); rc=$?
    kill $wd 2>/dev/null; wait $wd 2>/dev/null
    log "--- ratio=$ratio arm=$arm rep=$rep rc=$rc B=$((B/1048576))MiB budget=$((budget/1048576))MiB memmax=$((memmax/1048576))MiB ---"
    echo "$out" | grep -E "WP2_CSV|RESIDCTL_STATS|RESIDCTL_SIGDUMP|WP2_TOKENS|FATAL|FAILED|oom" | tee -a "$LOG" >/dev/null

    local np nd ld tt p99 ws ts iog pbf ah ev pf inf pb df pd ft re mp pc sm
    np=$(field "$out" n_prompt); nd=$(field "$out" n_decoded)
    ld=$(field "$out" load_s); tt=$(field "$out" ttft_ms); p99=$(field "$out" p99_inter_ms)
    ws=$(field "$out" wall_s); ts=$(field "$out" tokens_s); iog=$(field "$out" io_read_bytes_gen)
    pbf=$(field "$out" pager_bytes_fetched); ah=$(field "$out" absent_handled); ev=$(field "$out" evictions)
    pf=$(field "$out" prefetches); inf=$(field "$out" infeasible); pb=$(field "$out" pin_broken)
    df=$(field "$out" dedup_fetching); pd=$(field "$out" stat_prefetch_declined); ft=$(field "$out" stat_fetching_timeout)
    re=$(field "$out" resident_bytes_end); mp=$(field "$out" memory_peak)
    pc=$(echo "$out" | grep -oP 'RESIDCTL_STATS,.*protect_current=\K[a-z]+' | tail -1)
    sm=$(echo "$out" | grep -oP 'RESIDCTL_STATS,.*signal_mode=\K[a-z]+' | tail -1)
    [ -z "${mp:-}" ] && mp=$(cat "$CGROUP/memory.peak" 2>/dev/null)
    echo "$ratio,$arm,$policy,$prefetch,$rep,$B,$budget,$memmax,${np:-},${nd:-},${ld:-},${tt:-},${p99:-},${ws:-},${ts:-},${iog:-},${pbf:-},${ah:-},${ev:-},${pf:-},${inf:-},${pb:-},${df:-},${pd:-},${ft:-},${re:-},${mp:-},${pc:-},${sm:-},$rc" >> "$CSV"
    cleanup
}

log "=== machine exclusivity (before sweep) ==="; uptime | tee -a "$LOG"
pgrep -af "cn-spike|iperf3|gate5" && log "WARNING foreign workload" || log "(clean)"

for ratio in 0.25 0.375 0.5 0.625 0.75; do
    for rep in $(seq 1 $NREPS); do
        run_one "$ratio" C "lru"                  off "$rep"
        # arm D rep1 at each ratio gets a policy-trace; arm E rep1 at r<=0.375 too
        dtr=""; [ "$rep" = 1 ] && dtr=trace
        run_one "$ratio" D "layer_order_declared" off "$rep" "$dtr"
        etr=""; [ "$rep" = 1 ] && awk "BEGIN{exit !($ratio <= 0.375)}" && etr=trace
        run_one "$ratio" E "layer_order_declared" on  "$rep" "$etr"
    done
done

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity (after) ==="; uptime | tee -a "$LOG"
log "=== Phase 3 complete: $CSV ==="
