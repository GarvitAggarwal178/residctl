#!/bin/bash
# Build-order item 8 verification: prefetch (§6.3). Wrapped in `timeout`
# deliberately -- the scenario this exercises is exactly the one that
# surfaced a self-eviction deadlock risk during development (see
# CLAUDE.md); if a future change reintroduces it, this must hang and time
# out, not silently pass.
set -u
RESIDCTL=/root/residctl
BIN=$RESIDCTL/src/test_prefetch
GEN=$RESIDCTL/src/gen_pattern
MODEL=$RESIDCTL/scratch/pattern_16m.bin
RESULTS=$RESIDCTL/results
CGROUP=/sys/fs/cgroup/residctl_item8
OUT=$RESULTS/item8_test_log.txt
> "$OUT"

mkdir -p "$RESIDCTL/scratch" "$RESULTS"
[ -f "$MODEL" ] || "$GEN" "$MODEL" 16777216

if [ -d "$CGROUP" ]; then
    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
    rmdir "$CGROUP" 2>/dev/null
fi
mkdir "$CGROUP"
echo 536870912 > "$CGROUP/memory.max"
echo 0 > "$CGROUP/memory.swap.max"

out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 20 "$@"' -- \
      "$BIN" "$CGROUP" "$MODEL" 2>&1)
rc=$?
echo "$out" | tee -a "$OUT"
echo "exit code: $rc" | tee -a "$OUT"
if [ $rc -eq 124 ]; then
    echo "TIMED OUT -- possible deadlock" | tee -a "$OUT"
fi

procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
rmdir "$CGROUP" 2>/dev/null

if [ $rc -eq 0 ] && echo "$out" | grep -q "^PASS$"; then
    echo "RESULT: PASS" | tee -a "$OUT"
else
    echo "RESULT: FAIL (rc=$rc)" | tee -a "$OUT"
fi
