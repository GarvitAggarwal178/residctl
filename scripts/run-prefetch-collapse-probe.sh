#!/bin/bash
# FINAL SESSION Phase 3 -- arm E's collapse at tight budget (real model, r=0.25).
#
# Phase 2 flipped the default to protect-current OFF. So:
#   Step 1  reproduce the ORIGINAL collapse: arm E, protect_current=on (WP2's
#           config), --fetch-trace + --policy-trace + a 100 s watchdog that
#           SIGUSR1-dumps residctl residency state and /proc/PID/task/*/wchan.
#   Step 2  does the new default fix it? arm E, protect off (default), n=3.
#   Step 3  the pre-decided mitigations (only meaningful if step 2 still
#           collapses): --prefetch-retention none / --prefetch-depth 1 / both,
#           n=3, protect off.
#   Step 4  protect-on + each mitigation, n=3, to attribute the fix.
# Pre-decided: whichever config completes with the lowest read_bytes becomes
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
    log "=== fresh === B=$((B/1048576))MiB memory.max=$((MM/1048576))MiB ratio=$RATIO"
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
done_cfg() { awk -F, -v c="$1" -v r="$2" 'NR>1 && $1==c && $2==r {f=1} END{exit !f}' "$CSV"; }

# $1 name  $2 rep  $3 extra-cfg-lines  $4 trace?(trace|"")
run_cfg() {
    local name=$1 rep=$2 extra=$3 trace=${4:-}
    done_cfg "$name" "$rep" && { log "SKIP $name rep$rep"; return; }
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
    log "--- config=$name rep=$rep ---"; sed 's/^/    /' "$cfg" | tee -a "$LOG" >/dev/null

    ( sleep $WATCHDOG
      pid=$(cat "$CGROUP/cgroup.procs" 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+$' | tail -1)
      if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        log ">>> WATCHDOG t=${WATCHDOG}s pid=$pid: SIGUSR1 + wchan"
        kill -USR1 "$pid" 2>/dev/null; sleep 0.5
        for t in /proc/$pid/task/*/wchan; do
            [ -r "$t" ] && log "   tid $(basename $(dirname $t)) wchan=$(cat $t 2>/dev/null)"
        done
        cat "$CGROUP/memory.events" 2>/dev/null | sed 's/^/   mem.events /' | tee -a "$LOG" >/dev/null
      fi
    ) & local wd=$!

    local out rc
    out=$(bash -c "echo \$BASHPID > $CGROUP/cgroup.procs; RESIDCTL_CONFIG=$cfg exec timeout $TIMEOUT $SRC/wp2_gen -m $MODEL -n $N_TOK -p \"\$1\" -t 8" _ "$PROMPT" 2>&1); rc=$?
    kill $wd 2>/dev/null; wait $wd 2>/dev/null
    log "--- config=$name rep=$rep rc=$rc ---"
    echo "$out" | grep -E "WP2_CSV|RESIDCTL_STATS|RESIDCTL_SIGDUMP|oom_kill|FETCH FAILED|infeasible=" | tee -a "$LOG" >/dev/null

    local nd ws ts iog pbf ah ev pf inf pb re mp
    nd=$(g "$out" n_decoded); ws=$(g "$out" wall_s); ts=$(g "$out" tokens_s)
    iog=$(g "$out" io_read_bytes_gen); pbf=$(g "$out" pager_bytes_fetched)
    ah=$(g "$out" absent_handled); ev=$(g "$out" evictions); pf=$(g "$out" prefetches)
    inf=$(g "$out" infeasible); pb=$(g "$out" pin_broken); re=$(g "$out" resident_bytes_end)
    mp=$(g "$out" memory_peak); [ -z "${mp:-}" ] && mp=$(cat "$CGROUP/memory.peak" 2>/dev/null)
    echo "$name,$rep,$rc,${nd:-},${ws:-},${ts:-},${iog:-},${pbf:-},${ah:-},${ev:-},${pf:-},${inf:-},${pb:-},${re:-},${mp:-}" >> "$CSV"
    cleanup
}

# Step 1: reproduce the original collapse (protect on), traced
run_cfg orig_protect_on_traced 0 "protect_current=on" trace

# Step 2 + 3 + 4: n=3
for rep in 1 2 3; do
    run_cfg default_protect_off      $rep ""                                        # new default
    run_cfg protect_on               $rep "protect_current=on"                      # WP2 config
    run_cfg off_retention_none       $rep "retention=none"
    run_cfg off_depth1               $rep "prefetch_depth=1"
    run_cfg off_none_depth1          $rep "retention=none
prefetch_depth=1"
done

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity (after) ==="; uptime | tee -a "$LOG"
log "=== Phase 3 complete: $CSV ==="
