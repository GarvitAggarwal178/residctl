#!/bin/bash
# FINAL SESSION Phase 1 -- equal-budget baseline.
#
# The quantity equalised across arms is the WEIGHT-RESIDENCY CEILING:
#   - arm A (pure kernel mmap): memory.max == B. The kernel caps weight page
#     cache at B minus arm A's own non-weight resident set (KV cache, compute
#     buffers, ~50 MiB anon), so arm A's effective weight ceiling ~= B - 50 MiB.
#   - arms C/D/E (residctl pager): budget_bytes == B (the pager keeps exactly
#     B bytes of weights resident). They additionally get memory.max = B + MARGIN
#     so the identical ~50-90 MiB llama non-weight footprint does not OOM-kill
#     them. MARGIN is applied identically to every residctl arm and every ratio.
#
# Net asymmetry: the residctl arms get ~50 MiB MORE weight-cache room than
# arm A (<=10% of B at r=0.25, <=3% at r=0.75). This is the OPPOSITE direction
# and ~4x smaller than WP2's original confound (memory.max = B + 256 MiB for
# every arm, which favoured arm A). Stated in the report and every table.
#
# Probed 2026-08-28: arm A runs at memory.max == B at every ratio; residctl
# arms OOM at memory.max == B (need the MARGIN); residctl arms with
# budget_bytes = B - 96 MiB (RESERVE approach) COLLAPSE at r=0.25 (pager
# starved), so budget_bytes is kept at full B.
#
# Arms A (mmap), C (lru), D (layer_order_declared, prefetch off),
#      E (layer_order_declared, prefetch on, depth 2, retention pinned).
# Ratios {0.25, 0.375, 0.5, 0.625, 0.75}. n=3. 64 tokens, fixed prompt, greedy.
# memory.swap.max=0. drop_caches before every arm-A run.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
OUT=$RESIDCTL/results/final
MODEL=$RESIDCTL/models/model.gguf
CGROUP=/sys/fs/cgroup/residctl_final_p1
CSV=$OUT/phase1_equal_budget.csv
LOG=$OUT/phase1_equal_budget_log.txt
PROMPT="In a distant future, humanity spread across the stars, and the old questions returned"
N_TOK=64
NREPS=3
MARGIN=134217728    # 128 MiB, residctl arms only, for llama's non-weight memory. Uniform across residctl arms/ratios.
TIMEOUT=360
O_DIRECT_CEIL_MIBS=3396

log() { echo "$@" | tee -a "$LOG"; }
[ -f "$MODEL" ] || { log "no model"; exit 1; }
FSIZE=$(stat -c %s "$MODEL")
REGION=$(( (FSIZE + 4095) / 4096 * 4096 ))

if [ ! -f "$CSV" ]; then
    > "$LOG"; > "$CSV"
    echo "ratio,arm,policy,prefetch,rep,B_bytes,budget_bytes,memory_max,n_prompt,n_decoded,layer_transitions,load_s,ttft_ms,p99_inter_ms,wall_s,tokens_s,io_gen_bytes,pager_bytes_fetched,absent_handled,evictions,prefetches,infeasible,pin_broken,dedup_fetching,resident_end,memory_peak,handler_p99_ns,vmhwm_kb,achieved_mibs,hostcache_flag,rc" > "$CSV"
    log "=== fresh start === REGION=$REGION FSIZE=$FSIZE MARGIN=$((MARGIN/1048576))MiB (residctl arms only)"
else
    log "=== resume: $(wc -l < "$CSV") lines ==="
fi

log "=== machine exclusivity (before) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -5 | tee -a "$LOG"
pgrep -af "cn-spike|iperf3|gate5" && log "WARNING foreign workload" || log "(clean)"

fresh() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"; echo "$1" > "$CGROUP/memory.max"; echo 0 > "$CGROUP/memory.swap.max"
    echo 0 > "$CGROUP/memory.peak" 2>/dev/null || true
}
cleanup() { for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done; }
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches; }
rep_done() { awk -F, -v r="$1" -v a="$2" -v rp="$3" 'NR>1 && $1==r && $2==a && $5==rp {f=1} END{exit !f}' "$CSV"; }
field() { echo "$1" | grep -oP "$2=\K[-0-9.]+" | tail -1; }

run_one() {
    local ratio=$1 arm=$2 policy=$3 prefetch=$4 rep=$5
    rep_done "$ratio" "$arm" "$rep" && { log "SKIP $ratio $arm rep$rep"; return; }
    local B budget memmax
    B=$(awk "BEGIN{printf \"%d\", $REGION*$ratio}")
    if [ "$arm" = A ]; then
        budget=$B; memmax=$B
    else
        budget=$B; memmax=$(( B + MARGIN ))
    fi
    fresh "$memmax"
    local cfg=""
    if [ "$arm" != A ]; then
        cfg=$OUT/phase1_${arm}.cfg
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
EOF
        [ "$arm" = D ] && [ "$rep" = 1 ] && echo "reftrace=$OUT/phase1_reftrace_r${ratio}.bin" >> "$cfg"
    fi
    [ "$arm" = A ] && drop_caches

    local out rc
    if [ "$arm" = A ]; then
        out=$(bash -c "echo \$BASHPID > $CGROUP/cgroup.procs; exec timeout $TIMEOUT $SRC/wp2_gen -m $MODEL -n $N_TOK -p \"\$1\" -t 8" _ "$PROMPT" 2>&1); rc=$?
    else
        out=$(bash -c "echo \$BASHPID > $CGROUP/cgroup.procs; RESIDCTL_CONFIG=$cfg exec timeout $TIMEOUT $SRC/wp2_gen -m $MODEL -n $N_TOK -p \"\$1\" -t 8" _ "$PROMPT" 2>&1); rc=$?
    fi
    log "--- ratio=$ratio arm=$arm rep=$rep rc=$rc B=$((B/1048576))MiB budget=$((budget/1048576))MiB memmax=$((memmax/1048576))MiB ---"
    log "$out"

    local np nd lt ld tt p99 ws ts iog pbf ah ev pf inf pb df re mp hp vk
    np=$(field "$out" n_prompt); nd=$(field "$out" n_decoded); lt=$(field "$out" layer_transitions)
    ld=$(field "$out" load_s); tt=$(field "$out" ttft_ms); p99=$(field "$out" p99_inter_ms)
    ws=$(field "$out" wall_s); ts=$(field "$out" tokens_s); iog=$(field "$out" io_read_bytes_gen)
    pbf=$(field "$out" pager_bytes_fetched); ah=$(field "$out" absent_handled); ev=$(field "$out" evictions)
    pf=$(field "$out" prefetches); inf=$(field "$out" infeasible); pb=$(field "$out" pin_broken)
    df=$(field "$out" dedup_fetching); re=$(field "$out" resident_bytes_end)
    mp=$(field "$out" memory_peak); hp=$(field "$out" handler_p99_ns); vk=$(field "$out" vmhwm_kb)
    [ -z "${mp:-}" ] && mp=$(cat "$CGROUP/memory.peak" 2>/dev/null)

    local ach="" flag=""
    if [ "$arm" = A ] && [ -n "${iog:-}" ] && [ -n "${ws:-}" ]; then
        ach=$(awk "BEGIN{ if ($ws>0) printf \"%.0f\", ($iog/1048576)/$ws; else print 0 }")
        awk "BEGIN{exit !($ach > $O_DIRECT_CEIL_MIBS)}" && flag="HOSTCACHE"
    fi

    echo "$ratio,$arm,$policy,$prefetch,$rep,$B,$budget,$memmax,${np:-},${nd:-},${lt:-},${ld:-},${tt:-},${p99:-},${ws:-},${ts:-},${iog:-},${pbf:-},${ah:-},${ev:-},${pf:-},${inf:-},${pb:-},${df:-},${re:-},${mp:-},${hp:-},${vk:-},${ach:-},${flag:-},$rc" >> "$CSV"
    cleanup
}

for ratio in 0.25 0.375 0.5 0.625 0.75; do
    for rep in $(seq 1 $NREPS); do
        run_one "$ratio" A "mmap"                 off "$rep"
        run_one "$ratio" C "lru"                  off "$rep"
        run_one "$ratio" D "layer_order_declared" off "$rep"
        run_one "$ratio" E "layer_order_declared" on  "$rep"
    done
done

rmdir "$CGROUP" 2>/dev/null
log ""
log "=== machine exclusivity (after) ==="
uptime | tee -a "$LOG"
log "=== Phase 1 sweep complete: $CSV ==="
