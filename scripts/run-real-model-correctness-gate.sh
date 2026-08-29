#!/bin/bash
# WP2 Phase 2.2 correctness gate: 32 tokens, fixed prompt, greedy (deterministic)
# under --load-mode mmap vs residctl with a budget large enough to hold the
# whole model. Token sequences MUST be identical.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
OUT=$RESIDCTL/results/overnight
MODEL=$RESIDCTL/models/model.gguf
CGROUP=/sys/fs/cgroup/residctl_wp2gate
LOG=$OUT/wp2_gate_log.txt
PROMPT="The quick brown fox jumps over the lazy dog and then keeps running"
> "$LOG"
log() { echo "$@" | tee -a "$LOG"; }

[ -f "$MODEL" ] || { log "GATE FAIL: $MODEL missing"; exit 1; }
FSIZE=$(stat -c %s "$MODEL")
BIG=$(( FSIZE + FSIZE / 2 + 268435456 ))   # ~1.5x file + 256 MiB -- everything fits

fresh() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo "$1" > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
}

CFG=$OUT/wp2_gate.cfg
cat > "$CFG" <<EOF
model=$MODEL
cgroup=$CGROUP
budget_bytes=$BIG
policy=layer_order_declared
prefetch=off
retention=pinned
fetch_workers=4
eager_reconcile=1
EOF

log "=== machine exclusivity ==="; uptime | tee -a "$LOG"

log "=== arm A: --load-mode mmap ==="
fresh $((BIG + 536870912))
out_a=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 600 "$@"' -- \
        "$SRC/wp2_gen" -m "$MODEL" -n 32 -p "$PROMPT" -t 8 --dump-tokens "$OUT/wp2_gate_tokens_mmap.txt" 2>&1)
rc_a=$?
log "$out_a"; log "arm A rc=$rc_a"
for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done

log ""
log "=== arm residctl: --load-mode residctl (budget holds whole model) ==="
fresh $((BIG + 536870912))
out_r=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; RESIDCTL_CONFIG="'"$CFG"'" exec timeout 600 "$@"' -- \
        "$SRC/wp2_gen" -m "$MODEL" -n 32 -p "$PROMPT" -t 8 --dump-tokens "$OUT/wp2_gate_tokens_residctl.txt" --inventory "$OUT/wp2_tensor_inventory.txt" 2>&1)
rc_r=$?
log "$out_r"; log "arm residctl rc=$rc_r"
for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
rmdir "$CGROUP" 2>/dev/null

log ""
if [ $rc_a -ne 0 ] || [ $rc_r -ne 0 ]; then
    log "GATE FAIL: a run exited non-zero (A=$rc_a residctl=$rc_r)"
    exit 1
fi
if diff -q "$OUT/wp2_gate_tokens_mmap.txt" "$OUT/wp2_gate_tokens_residctl.txt" >/dev/null; then
    log "GATE PASS: token sequences identical"
    log "  mmap:     $(cat "$OUT/wp2_gate_tokens_mmap.txt")"
    log "  residctl: $(cat "$OUT/wp2_gate_tokens_residctl.txt")"
    exit 0
else
    log "GATE FAIL: token sequences DIFFER"
    log "  mmap:     $(cat "$OUT/wp2_gate_tokens_mmap.txt")"
    log "  residctl: $(cat "$OUT/wp2_gate_tokens_residctl.txt")"
    diff "$OUT/wp2_gate_tokens_mmap.txt" "$OUT/wp2_gate_tokens_residctl.txt" | tee -a "$LOG"
    exit 1
fi
