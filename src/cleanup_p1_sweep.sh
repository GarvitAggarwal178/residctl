#!/bin/bash
# CLEANUP Phase 1 verification sweeps #3 and #4 (real model, equal budget).
#  #3: shipping default arm D (protect-current off), r {0.25,0.5,0.75}, n=3.
#      Compare read_bytes to results/final/phase1_equal_budget.csv (within variance).
#  #4: arm E at r <= 0.375 (r {0.25,0.375}), shipping default (protect off),
#      prefetch on d2 pinned, n=3. Report whether prefetch completes and its cost.
# All runs also report stat_fetching_timeout (fix part b).
set -u
cd /root/residctl
SRC=$PWD/src
OUT=$PWD/results/cleanup
MODEL=$PWD/models/model.gguf
CG=/sys/fs/cgroup/residctl_cleanup
CSV=$OUT/phase1_verify.csv
LOG=$OUT/phase1_verify_log.txt
PROMPT="In a distant future, humanity spread across the stars, and the old questions returned"
N_TOK=64; NREPS=3; MARGIN=134217728; TIMEOUT=360
FSIZE=$(stat -c %s "$MODEL"); REGION=$(( (FSIZE+4095)/4096*4096 ))
log(){ echo "$@" | tee -a "$LOG"; }
if [ ! -f "$CSV" ]; then
  > "$LOG"
  echo "check,ratio,arm,prefetch,protect,rep,rc,n_decoded,wall_s,tokens_s,io_gen_bytes,pager_bytes_fetched,absent_handled,evictions,prefetches,infeasible,pin_broken,stat_fetching_timeout,memory_peak" > "$CSV"
fi
log "=== $(date) exclusivity ==="; uptime | tee -a "$LOG"
pgrep -af "cn-spike|iperf3|gate5" && log "WARN foreign" || log "(clean)"
fresh(){ [ -d "$CG" ] && { for p in $(cat $CG/cgroup.procs 2>/dev/null); do kill -9 $p 2>/dev/null; done; sleep 1; rmdir $CG 2>/dev/null; }; mkdir $CG; echo "$1" > $CG/memory.max; echo 0 > $CG/memory.swap.max; echo 0 > $CG/memory.peak 2>/dev/null||true; }
g(){ echo "$1" | grep -oP "$2=\K[-0-9.]+" | tail -1; }
done_row(){ awk -F, -v c="$1" -v r="$2" -v a="$3" -v rp="$4" 'NR>1&&$1==c&&$2==r&&$3==a&&$6==rp{f=1}END{exit !f}' "$CSV"; }

run(){ # check ratio arm prefetch protect rep
  local ck=$1 ratio=$2 arm=$3 pf=$4 prot=$5 rep=$6
  done_row "$ck" "$ratio" "$arm" "$rep" && { log "SKIP $ck $ratio $arm rep$rep"; return; }
  local B budget mm; B=$(awk "BEGIN{printf \"%d\", $REGION*$ratio}")
  budget=$B; mm=$(( B + MARGIN ))
  fresh "$mm"; sync; echo 3 > /proc/sys/vm/drop_caches
  local cfg=$OUT/verify_${ck}_${arm}.cfg
  cat > "$cfg" <<EOF
model=$MODEL
cgroup=$CG
budget_bytes=$budget
policy=layer_order_declared
prefetch=$pf
prefetch_depth=2
retention=pinned
fetch_workers=4
eager_reconcile=0
protect_current=$prot
EOF
  local out rc
  out=$(bash -c "echo \$BASHPID > $CG/cgroup.procs; RESIDCTL_CONFIG=$cfg exec timeout $TIMEOUT $SRC/wp2_gen -m $MODEL -n $N_TOK -p \"\$1\" -t 8" _ "$PROMPT" 2>&1); rc=$?
  log "--- $ck ratio=$ratio arm=$arm pf=$pf protect=$prot rep=$rep rc=$rc ---"
  echo "$out" | grep -E "WP2_CSV|RESIDCTL_STATS|watchdog reclaimed|PAGER FAILED" | tee -a "$LOG" >/dev/null
  local nd ws ts iog pbf ah ev pfx inf pb sft mp
  nd=$(g "$out" n_decoded); ws=$(g "$out" wall_s); ts=$(g "$out" tokens_s)
  iog=$(g "$out" io_read_bytes_gen); pbf=$(g "$out" pager_bytes_fetched)
  ah=$(g "$out" absent_handled); ev=$(g "$out" evictions); pfx=$(g "$out" prefetches)
  inf=$(g "$out" infeasible); pb=$(g "$out" pin_broken); sft=$(g "$out" stat_fetching_timeout)
  mp=$(g "$out" memory_peak); [ -z "${mp:-}" ] && mp=$(cat $CG/memory.peak 2>/dev/null)
  echo "$ck,$ratio,$arm,$pf,$prot,$rep,$rc,${nd:-},${ws:-},${ts:-},${iog:-},${pbf:-},${ah:-},${ev:-},${pfx:-},${inf:-},${pb:-},${sft:-},${mp:-}" >> "$CSV"
  for p in $(cat $CG/cgroup.procs 2>/dev/null); do kill -9 $p 2>/dev/null; done
}

for rep in 1 2 3; do
  for ratio in 0.25 0.5 0.75; do run noreg   "$ratio" D off off "$rep"; done
  for ratio in 0.25 0.375;    do run armE_lo "$ratio" E on  off "$rep"; done
done
rmdir $CG 2>/dev/null
log "=== $(date) done ==="; uptime | tee -a "$LOG"
log "--- $CSV ---"; column -t -s, "$CSV" | tee -a "$LOG"
