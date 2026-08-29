#!/bin/bash
# WP2 Phase 2.3 measurement. Arms A (mmap), C (lru), D (declared, prefetch
# off), E (declared, prefetch on depth 2 pinned). Budget ratios {0.25, 0.5,
# 0.75} of the model's weight-region size. 64 tokens, fixed prompt, greedy,
# n=3. memory.swap.max=0. drop_caches before every arm A run.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
OUT=$RESIDCTL/results/overnight
MODEL=$RESIDCTL/models/model.gguf
CGROUP=/sys/fs/cgroup/residctl_wp2sweep
CSV=$OUT/wp2_sweep.csv
LOG=$OUT/wp2_sweep_log.txt
PROMPT="In a distant future, humanity spread across the stars, and the old questions returned"
N_TOK=64
NREPS=2
MARGIN=268435456   # 256 MiB headroom for llama's non-weight footprint (kv cache, activations, ctx)

log() { echo "$@" | tee -a "$LOG"; }

[ -f "$MODEL" ] || { log "no model"; exit 1; }
FSIZE=$(stat -c %s "$MODEL")
# weight-region size = file rounded up to 4096 (residctl_llama's region_len).
REGION=$(( (FSIZE + 4095) / 4096 * 4096 ))

if [ ! -f "$CSV" ]; then
    > "$LOG"; > "$CSV"
    echo "ratio,arm,policy,prefetch,rep,budget_bytes,n_prompt,n_decoded,layer_transitions,load_s,ttft_ms,p99_inter_ms,wall_s,tokens_s,io_gen_bytes,pager_bytes_fetched,absent_handled,evictions,prefetches,infeasible,pin_broken,dedup_resident,dedup_fetching,resident_end,memory_peak,handler_p99_ns,vmhwm_kb,rc" > "$CSV"
    log "=== fresh start === REGION=$REGION FSIZE=$FSIZE"
else
    log "=== resume: $(wc -l < "$CSV") lines ==="
fi

log "=== machine exclusivity (before) ==="
uptime | tee -a "$LOG"; ps aux --sort=-%cpu 2>/dev/null | head -5 | tee -a "$LOG"

fresh() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"; echo "$1" > "$CGROUP/memory.max"; echo 0 > "$CGROUP/memory.swap.max"
    echo 0 > "$CGROUP/memory.peak" 2>/dev/null || true
}
cleanup() { for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done; }
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a "$LOG"; }

rep_done() { awk -F, -v r="$1" -v a="$2" -v p="$3" -v rp="$4" 'NR>1 && $1==r && $2==a && $3==p && $5==rp {f=1} END{exit !f}' "$CSV"; }

field() { echo "$1" | grep -oP "$2=\K[0-9.]+" | tail -1; }

run_one() {
    local ratio=$1 arm=$2 policy=$3 prefetch=$4 rep=$5
    rep_done "$ratio" "$arm" "$policy" "$rep" && { log "SKIP $ratio $arm $policy rep$rep"; return; }
    local budget=$(awk "BEGIN{printf \"%d\", $REGION*$ratio}")
    local memmax=$(( budget + MARGIN ))
    fresh "$memmax"
    local cfg=""
    if [ "$arm" != "A" ]; then
        cfg=$OUT/wp2_sweep_${arm}.cfg
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
    fi
    [ "$arm" = "A" ] && drop_caches
    local rt=""
    [ "$arm" = "D" ] && [ "$rep" = 1 ] && rt="reftrace=$OUT/wp2_reftrace_r${ratio}.bin"
    [ -n "$rt" ] && echo "$rt" >> "$cfg"

    local out rc
    if [ "$arm" = "A" ]; then
        out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 360 "$@"' -- \
              "$SRC/wp2_gen" -m "$MODEL" -n "$N_TOK" -p "$PROMPT" -t 8 2>&1); rc=$?
    else
        out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; RESIDCTL_CONFIG="'"$cfg"'" exec timeout 360 "$@"' -- \
              "$SRC/wp2_gen" -m "$MODEL" -n "$N_TOK" -p "$PROMPT" -t 8 2>&1); rc=$?
    fi
    log "--- ratio=$ratio arm=$arm policy=$policy prefetch=$prefetch rep=$rep rc=$rc ---"
    log "$out"

    local np nd lt ld tt p99 ws ts iog
    np=$(field "$out" 'n_prompt'); nd=$(field "$out" 'n_decoded'); lt=$(field "$out" 'layer_transitions')
    ld=$(field "$out" 'load_s'); tt=$(field "$out" 'ttft_ms'); p99=$(field "$out" 'p99_inter_ms')
    ws=$(field "$out" 'wall_s'); ts=$(field "$out" 'tokens_s'); iog=$(field "$out" 'io_read_bytes_gen')
    local pbf ah ev pf inf pb dr df re mp hp vk
    pbf=$(field "$out" 'pager_bytes_fetched'); ah=$(field "$out" 'absent_handled'); ev=$(field "$out" 'evictions')
    pf=$(field "$out" 'prefetches'); inf=$(field "$out" 'infeasible'); pb=$(field "$out" 'pin_broken')
    dr=$(field "$out" 'dedup_resident'); df=$(field "$out" 'dedup_fetching'); re=$(field "$out" 'resident_bytes_end')
    mp=$(field "$out" 'memory_peak'); hp=$(field "$out" 'handler_p99_ns'); vk=$(field "$out" 'vmhwm_kb')

    echo "$ratio,$arm,$policy,$prefetch,$rep,$budget,${np:-},${nd:-},${lt:-},${ld:-},${tt:-},${p99:-},${ws:-},${ts:-},${iog:-},${pbf:-},${ah:-},${ev:-},${pf:-},${inf:-},${pb:-},${dr:-},${df:-},${re:-},${mp:-},${hp:-},${vk:-},$rc" >> "$CSV"
    cleanup
}

for ratio in 0.25 0.5 0.75; do
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
log "=== WP2 sweep complete: $CSV ==="
