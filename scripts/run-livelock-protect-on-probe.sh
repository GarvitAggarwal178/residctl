#!/bin/bash
# LIVELOCK FIX Phase 3b -- the decisive test for Claim 10.
# The original arm-E livelock (cleanup Phase 1) was a PROTECT-CURRENT-ON
# phenomenon: rc=124, ~90x I/O amplification, would finish in ~6h. Phase 3
# above ran protect-OFF (per spec) and arm E completed everywhere. This runs
# arm E with protect-current ON + the four fixes, at the two ratios where it
# livelocked, with the SIGUSR1 + gdb-style watchdog, n=2. If it now completes,
# the livelock is FIXED (not merely avoided by protect-off).
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
OUT=$RESIDCTL/results/livelock
MODEL=$RESIDCTL/models/model.gguf
CGROUP=/sys/fs/cgroup/residctl_ll_p3b
CSV=$OUT/phase3b_arm_e_protect_on.csv
LOG=$OUT/phase3b_console.txt
PROMPT="In a distant future, humanity spread across the stars, and the old questions returned"
N_TOK=64
MARGIN=134217728
TIMEOUT=600      # generous -- a livelock would run for hours; 600s is well past a healthy ~70s run
WATCHDOG=180

mkdir -p "$OUT"
log() { echo "$@" | tee -a "$LOG"; }
FSIZE=$(stat -c %s "$MODEL"); REGION=$(( (FSIZE + 4095) / 4096 * 4096 ))

if [ ! -f "$CSV" ]; then
    > "$LOG"
    echo "ratio,rep,rc,n_decoded,wall_s,tokens_s,pager_bytes_fetched,absent_handled,evictions,prefetches,infeasible,pin_broken,prefetch_declined,fetching_timeout,protect_current,signal_mode" > "$CSV"
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
    echo 0 > "$CGROUP/memory.peak" 2>/dev/null || true
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
    local cfg=$OUT/phase3b_r${ratio}.cfg
    cat > "$cfg" <<EOF
model=$MODEL
cgroup=$CGROUP
budget_bytes=$budget
policy=layer_order_declared
prefetch=on
prefetch_depth=2
retention=pinned
fetch_workers=4
eager_reconcile=0
protect_current=on
EOF
    [ "$rep" = 1 ] && { echo "fetchtrace=$OUT/phase3b_r${ratio}_fetch.trace" >> "$cfg"; echo "policytrace=$OUT/phase3b_r${ratio}_policy.trace" >> "$cfg"; }

    ( sleep $WATCHDOG
      pid=$(cat "$CGROUP/cgroup.procs" 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+$' | tail -1)
      if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        log ">>> WATCHDOG t=${WATCHDOG}s r=$ratio rep$rep pid=$pid STILL RUNNING (healthy run is ~70s) -- SIGUSR1"
        kill -USR1 "$pid" 2>/dev/null; sleep 0.5
        for t in /proc/$pid/task/*/wchan; do [ -r "$t" ] && log "   tid $(basename $(dirname $t)) wchan=$(cat $t 2>/dev/null)"; done
        sleep 60
        if kill -0 "$pid" 2>/dev/null; then
          log ">>> WATCHDOG t=$((WATCHDOG+60))s pid=$pid STILL RUNNING -- SIGUSR1 again (counter progression check)"
          kill -USR1 "$pid" 2>/dev/null
        fi
      fi
    ) & local wd=$!

    local out rc
    out=$(bash -c "echo \$BASHPID > $CGROUP/cgroup.procs; RESIDCTL_CONFIG=$cfg exec timeout $TIMEOUT $SRC/wp2_gen -m $MODEL -n $N_TOK -p \"\$1\" -t 8" _ "$PROMPT" 2>&1); rc=$?
    kill $wd 2>/dev/null; wait $wd 2>/dev/null
    log "--- r=$ratio rep$rep rc=$rc (rc=124 => TIMEOUT/livelock) ---"
    echo "$out" | grep -E "WP2_CSV|RESIDCTL_STATS|RESIDCTL_SIGDUMP" | tee -a "$LOG" >/dev/null
    local nd ws ts pbf ah ev pf inf pb pd ft
    nd=$(field "$out" n_decoded); ws=$(field "$out" wall_s); ts=$(field "$out" tokens_s)
    pbf=$(field "$out" pager_bytes_fetched); ah=$(field "$out" absent_handled); ev=$(field "$out" evictions)
    pf=$(field "$out" prefetches); inf=$(field "$out" infeasible); pb=$(field "$out" pin_broken)
    pd=$(field "$out" stat_prefetch_declined); ft=$(field "$out" stat_fetching_timeout)
    echo "$ratio,$rep,$rc,${nd:-},${ws:-},${ts:-},${pbf:-},${ah:-},${ev:-},${pf:-},${inf:-},${pb:-},${pd:-},${ft:-},on,pre" >> "$CSV"
    cleanup
}

for ratio in 0.25 0.375; do
    for rep in 1 2; do run "$ratio" "$rep"; done
done
rmdir "$CGROUP" 2>/dev/null
log "=== machine exclusivity (after) ==="; uptime | tee -a "$LOG"
log "=== Phase 3b complete $(date -u +%FT%TZ): $CSV ==="
