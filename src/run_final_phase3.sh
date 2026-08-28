#!/bin/bash
# FINAL SESSION Phase 3 -- arm E's collapse at tight budget (real model, r=0.25).
#
# Step 1: reproduce the collapse with --fetch-trace + --policy-trace active and
#   a 100 s watchdog that SIGUSR1-dumps residctl residency state and dumps
#   /proc/PID/task/*/wchan, then lets `timeout` kill it.
# Step 2: test the three pre-decided mitigations at r=0.25, n=3:
#   a) prefetch-retention none
#   b) prefetch-depth 1
#   c) both
#   d) protect-current off (Phase 2 may have made this the default already)
# Pre-decided outcome: whichever completes with the lowest read_bytes becomes
#   arm E's tight-budget config, recorded as a stated operating limit. If none
#   completes: arm E has no viable config at r=0.25 on this model -- say so.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
OUT=$RESIDCTL/results/final
MODEL=$RESIDCTL/models/model.gguf
CGROUP=/sys/fs/cgroup/residctl_final_p3
CSV=$OUT/phase3_arm_e.csv
LOG=$OUT/phase3_log.txt
PROMPT="In a distant future, humanity spread across the stars, and the old questions returned"
N_TOK=64
MARGIN=134217728
RATIO=0.25
TIMEOUT=360
WATCHDOG=100

log() { echo "$@" | tee -a "$LOG"; }
FSIZE=$(stat -c %s "$MODEL"); REGION=$(( (FSIZE+4095)/4096*4096 ))
B=$(awk "BEGIN{printf \"%d\", $REGION*$RATIO}")
MM=$(( B + MARGIN ))

if [ ! -f "$CSV" ]; then
    > "$LOG"
    echo "config,rep,rc,n_decoded,wall_s,tokens_s,io_gen_bytes,pager_bytes_fetched,absent_handled,evictions,prefetches,infeasible,pin_broken,resident_end,memory_peak" > "$CSV"
    log "=== fresh start === B=$((B/1048576))MiB memory.max=$((MM/1048576))MiB ratio=$RATIO"
else
    log "=== resume: $(wc -l < "$CSV") lines ==="
fi

log "=== machine exclusivity (before) ==="; uptime | tee -a "$LOG"
pgrep -af "cn-spike|iperf3|gate5" && log "WARNING foreign workload" || log "(clean)"

fresh() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"; echo "$MM" > "$CGROUP/memory.max"; echo 0 > "$CGROUP/memory.swap.max"
    echo 0 > "$CGROUP/memory.peak" 2>/dev/null || true
}
cleanup() { for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done; }
g() { echo "$1" | grep -oP "$2=\K[-0-9.]+" | tail -1; }

# $1 config-name  $2 rep  $3 extra-cfg-lines  $4 (opt) "trace"
run_cfg() {
    local name=$1 rep=$2 extra=$3 trace=${4:-}
    fresh
    sync; echo 3 > /proc/sys/vm/drop_caches
    local cfg=$OUT/phase3_${name}.cfg
    cat > "$cfg" <<EOF
model=$MODEL
cgroup=$CGROUP
budget_bytes=$B
policy=layer_order_declared
prefetch=on
prefetch_depth=2
retention=pinned
fetch_workers=4
eager_reconcile=0
$extra
EOF
    if [ "$trace" = trace ]; then
        echo "fetchtrace=$OUT/phase3_${name}_fetch.trace" >> "$cfg"
        echo "policytrace=$OUT/phase3_${name}_policy.trace" >> "$cfg"
    fi
    log "--- config=$name rep=$rep cfg: ---"; sed 's/^/    /' "$cfg" | tee -a "$LOG" >/dev/null

    ( sleep $WATCHDOG
      pid=$(cat "$CGROUP/cgroup.procs" 2>/dev/null | head -1)
      if [ -n "$pid" ]; then
        log ">>> WATCHDOG t=${WATCHDOG}s: SIGUSR1 + wchan dump for pid $pid"
        kill -USR1 "$pid" 2>/dev/null; sleep 0.3
        for t in /proc/$pid/task/*/wchan; do
            [ -r "$t" ] && log "   $(basename $(dirname $t)) wchan=$(cat $t 2>/dev/null)"
        done
        cat "$CGROUP/memory.events" 2>/dev/null | sed 's/^/   mem.events /' | tee -a "$LOG" >/dev/null
      fi
    ) &
    local wd=$!

    local out rc
    out=$(bash -c "echo \$BASHPID > $CGROUP/cgroup.procs; RESIDCTL_CONFIG=$cfg exec timeout $TIMEOUT $SRC/wp2_gen -m $MODEL -n $N_TOK -p \"\$1\" -t 8" _ "$PROMPT" 2>&1); rc=$?
    kill $wd 2>/dev/null; wait $wd 2>/dev/null
    log "--- config=$name rep=$rep rc=$rc ---"
    echo "$out" | grep -E "WP2_CSV|RESIDCTL_STATS|RESIDCTL_SIGDUMP|oom_kill|FETCH FAILED|infeasible=" | tee -a "$LOG" >/dev/null
    echo "$out" | tail -40 >> "$LOG"

    local nd ws ts iog pbf ah ev pf inf pb re mp
    nd=$(g "$out" n_decoded); ws=$(g "$out" wall_s); ts=$(g "$out" tokens_s)
    iog=$(g "$out" io_read_bytes_gen); pbf=$(g "$out" pager_bytes_fetched)
    ah=$(g "$out" absent_handled); ev=$(g "$out" evictions); pf=$(g "$out" prefetches)
    inf=$(g "$out" infeasible); pb=$(g "$out" pin_broken); re=$(g "$out" resident_bytes_end)
    mp=$(g "$out" memory_peak); [ -z "${mp:-}" ] && mp=$(cat "$CGROUP/memory.peak" 2>/dev/null)
    echo "$name,$rep,$rc,${nd:-},${ws:-},${ts:-},${iog:-},${pbf:-},${ah:-},${ev:-},${pf:-},${inf:-},${pb:-},${re:-},${mp:-}" >> "$CSV"
    cleanup
}

# Step 1: characterise (rep 0, traced)
run_cfg baseline_traced 0 "" trace

# Step 2: mitigations, n=3
for rep in 1 2 3; do
    run_cfg retention_none    $rep "retention=none"
    run_cfg depth1            $rep "prefetch_depth=1"
    run_cfg none_and_depth1   $rep "retention=none
prefetch_depth=1"
    run_cfg protect_off       $rep "protect_current=off"
    run_cfg baseline          $rep ""
done

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity (after) ==="; uptime | tee -a "$LOG"
log "=== Phase 3 complete: $CSV ==="
