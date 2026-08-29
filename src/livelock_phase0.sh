#!/bin/bash
# LIVELOCK FIX Phase 0 -- cursor diagnostic GATE. Runs on UNMODIFIED code.
# Verifies pos tracks the workload's real per-token consumption order before
# any of the three fixes touches the distance function.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
OUT=$RESIDCTL/results/livelock
MODEL=$RESIDCTL/models/model.gguf
CGROUP=/sys/fs/cgroup/residctl_livelock_p0
PROMPT="In a distant future, humanity spread across the stars, and the old questions returned"
N_TOK=8
MARGIN=134217728
RATIO=0.5
mkdir -p "$OUT"
LOG=$OUT/phase0_console.txt
> "$LOG"
log() { echo "$@" | tee -a "$LOG"; }

FSIZE=$(stat -c %s "$MODEL"); REGION=$(( (FSIZE+4095)/4096*4096 ))
B=$(awk "BEGIN{printf \"%d\", $REGION*$RATIO}")
MM=$(( B + MARGIN ))
log "=== Phase 0 cursor diagnostic (UNMODIFIED code) ==="
log "model=$MODEL file_size=$FSIZE region=$REGION ratio=$RATIO B=$B ($((B/1048576)) MiB) memory.max=$((MM/1048576)) MiB"
log "=== machine exclusivity (before) ==="; uptime | tee -a "$LOG"
pgrep -af "cn-spike|iperf3|gate5" && log "WARNING foreign workload" || log "(clean)"

if [ -d "$CGROUP" ]; then
    for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
    sleep 1; rmdir "$CGROUP" 2>/dev/null
fi
mkdir "$CGROUP"; echo "$MM" > "$CGROUP/memory.max"; echo 0 > "$CGROUP/memory.swap.max"
echo 0 > "$CGROUP/memory.peak" 2>/dev/null || true
sync; echo 3 > /proc/sys/vm/drop_caches

CFG=$OUT/phase0_armD.cfg
cat > "$CFG" <<EOF
model=$MODEL
cgroup=$CGROUP
budget_bytes=$B
policy=layer_order_declared
prefetch=on
prefetch_depth=2
retention=pinned
fetch_workers=4
eager_reconcile=0
reftrace=$OUT/phase0_reftrace.bin
policytrace=$OUT/phase0_policytrace.bin
EOF
log "--- config ---"; sed 's/^/    /' "$CFG" | tee -a "$LOG" >/dev/null

# inventory (declared sequence + chunk table) -- written after mmap
INV=$OUT/phase0_inventory.txt

out=$(bash -c "echo \$BASHPID > $CGROUP/cgroup.procs; RESIDCTL_CONFIG=$CFG exec timeout 300 $SRC/wp2_gen -m $MODEL -n $N_TOK -p \"\$1\" -t 8 --inventory $INV" _ "$PROMPT" 2>&1)
rc=$?
log "--- rc=$rc ---"
echo "$out" | grep -E "residctl_llama:|WP2_CSV|RESIDCTL_STATS|WP2_TOKENS|WP2_RSS|FAILED" | tee -a "$LOG"
for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
rmdir "$CGROUP" 2>/dev/null

log ""
log "=== machine exclusivity (after) ==="; uptime | tee -a "$LOG"
log "rc=$rc  -- analysis: python3 $SRC/livelock_phase0_analyze.py"
