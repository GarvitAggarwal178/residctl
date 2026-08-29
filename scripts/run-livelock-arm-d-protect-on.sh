#!/bin/bash
# LIVELOCK FIX Phase 3c -- the missing one-variable cell: arm D with
# protect_current ON + all four fixes + signal_mode=pre. Phase 3 ran arm D
# protect OFF (per spec); the protect-on baseline in phase1_equal_budget.csv
# also had the post-compute signal and no fixes, so expectation 2 conflated
# two variables. Phase 3b showed protect-on now beats protect-off for arm E,
# so this cell decides (a) the true arm-D byte reduction and (b) the
# residctl_llama.c default for spec amendment A-14.
# Arm D, prefetch off, protect on, 5 ratios x n=2.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
OUT=$RESIDCTL/results/livelock
MODEL=$RESIDCTL/models/model.gguf
CGROUP=/sys/fs/cgroup/residctl_ll_p3c
CSV=$OUT/phase3c_arm_d_protect_on.csv
LOG=$OUT/phase3c_console.txt
PROMPT="In a distant future, humanity spread across the stars, and the old questions returned"
N_TOK=64
MARGIN=134217728
TIMEOUT=420

mkdir -p "$OUT"
log() { echo "$@" | tee -a "$LOG"; }
FSIZE=$(stat -c %s "$MODEL"); REGION=$(( (FSIZE + 4095) / 4096 * 4096 ))

if [ ! -f "$CSV" ]; then
    > "$LOG"
    echo "ratio,rep,rc,n_decoded,wall_s,tokens_s,ttft_ms,p99_inter_ms,pager_bytes_fetched,absent_handled,evictions,infeasible,pin_broken,fetching_timeout,protect_current,signal_mode" > "$CSV"
    log "=== fresh $(date -u +%FT%TZ) ==="
fi
log "=== machine exclusivity (before) ==="; uptime | tee -a "$LOG"
pgrep -af "cn-spike|iperf3|gate5" && log "WARNING foreign" || log "(clean)"

fresh() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"; echo "$1" > "$CGROUP/memory.max"; echo 0 > "$CGROUP/memory.swap.max"
}
cleanup() { for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done; }
field() { echo "$1" | grep -oP "$2=\K[-0-9.]+" | tail -1; }
done_row() { awk -F, -v r="$1" -v rp="$2" 'NR>1 && $1==r && $2==rp {f=1} END{exit !f}' "$CSV"; }

run() {
    local ratio=$1 rep=$2
    done_row "$ratio" "$rep" && { log "SKIP $ratio rep$rep"; return; }
    local B budget memmax
    B=$(awk "BEGIN{printf \"%d\", $REGION*$ratio}"); budget=$B; memmax=$(( B + MARGIN ))
    fresh "$memmax"; sync; echo 3 > /proc/sys/vm/drop_caches
    local cfg=$OUT/phase3c_r${ratio}.cfg
    cat > "$cfg" <<EOF
model=$MODEL
cgroup=$CGROUP
budget_bytes=$budget
policy=layer_order_declared
prefetch=off
fetch_workers=4
eager_reconcile=0
protect_current=on
EOF
    local out rc
    out=$(bash -c "echo \$BASHPID > $CGROUP/cgroup.procs; RESIDCTL_CONFIG=$cfg exec timeout $TIMEOUT $SRC/wp2_gen -m $MODEL -n $N_TOK -p \"\$1\" -t 8" _ "$PROMPT" 2>&1); rc=$?
    log "--- r=$ratio rep$rep rc=$rc ---"
    echo "$out" | grep -E "WP2_CSV|RESIDCTL_STATS" | tee -a "$LOG" >/dev/null
    local nd ws ts tt p99 pbf ah ev inf pb ft
    nd=$(field "$out" n_decoded); ws=$(field "$out" wall_s); ts=$(field "$out" tokens_s)
    tt=$(field "$out" ttft_ms); p99=$(field "$out" p99_inter_ms)
    pbf=$(field "$out" pager_bytes_fetched); ah=$(field "$out" absent_handled); ev=$(field "$out" evictions)
    inf=$(field "$out" infeasible); pb=$(field "$out" pin_broken); ft=$(field "$out" stat_fetching_timeout)
    echo "$ratio,$rep,$rc,${nd:-},${ws:-},${ts:-},${tt:-},${p99:-},${pbf:-},${ah:-},${ev:-},${inf:-},${pb:-},${ft:-},on,pre" >> "$CSV"
    cleanup
}

for ratio in 0.25 0.375 0.5 0.625 0.75; do
    for rep in 1 2; do run "$ratio" "$rep"; done
done
rmdir "$CGROUP" 2>/dev/null
log "=== machine exclusivity (after) ==="; uptime | tee -a "$LOG"
log "=== Phase 3c complete $(date -u +%FT%TZ): $CSV ==="
