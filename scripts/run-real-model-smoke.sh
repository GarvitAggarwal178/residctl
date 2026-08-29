#!/bin/bash
# WP2 quick smoke: one residctl arm-D run under a tight budget (eviction path).
set -u
OUT=/root/residctl/results/overnight
MODEL=/root/residctl/models/model.gguf
CG=/sys/fs/cgroup/residctl_wp2smoke
FSIZE=$(stat -c %s "$MODEL")
REGION=$(( (FSIZE + 4095) / 4096 * 4096 ))
BUDGET=$(awk "BEGIN{printf \"%d\", $REGION*0.5}")

[ -d "$CG" ] && { for p in $(cat "$CG/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done; rmdir "$CG" 2>/dev/null; }
mkdir "$CG"; echo $((BUDGET + 268435456)) > "$CG/memory.max"; echo 0 > "$CG/memory.swap.max"

cat > /tmp/wp2smoke.cfg <<EOF
model=$MODEL
cgroup=$CG
budget_bytes=$BUDGET
policy=${1:-layer_order_declared}
prefetch=${2:-off}
prefetch_depth=2
retention=pinned
fetch_workers=4
EOF

out=$(bash -c 'echo $BASHPID > "'"$CG"'/cgroup.procs"; RESIDCTL_CONFIG=/tmp/wp2smoke.cfg exec timeout 600 "$@"' -- \
      "$OUT/../../src/wp2_gen" -m "$MODEL" -n 16 -p "Testing the pager under a tight budget right now" -t 8 2>&1)
echo "$out" | grep -E 'WP2_TOKENS|WP2_CSV|RESIDCTL_STATS|WP2_RSS|FETCH FAILED|STARTUP FAILED'
for p in $(cat "$CG/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
rmdir "$CG" 2>/dev/null
