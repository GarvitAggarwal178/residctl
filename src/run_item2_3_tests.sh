#!/bin/bash
# Build-order items 2+3 verification: handler loop + state machine + fetch path.
set -u
RESIDCTL=/root/residctl
BIN=$RESIDCTL/src/test_pager
GEN=$RESIDCTL/src/gen_pattern
MODEL=$RESIDCTL/scratch/pattern_16m.bin
RESULTS=$RESIDCTL/results
CGROUP=/sys/fs/cgroup/residctl_item23
OUT=$RESULTS/item2_3_test_log.txt
> "$OUT"

mkdir -p "$RESIDCTL/scratch" "$RESULTS"
"$GEN" "$MODEL" 16777216

if [ -d "$CGROUP" ]; then
    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
    rmdir "$CGROUP" 2>/dev/null
fi
mkdir "$CGROUP"
echo 536870912 > "$CGROUP/memory.max"
echo 0 > "$CGROUP/memory.swap.max"

out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec "$@"' -- \
      "$BIN" "$CGROUP" "$MODEL" 2>&1)
rc=$?
echo "$out" | tee -a "$OUT"
echo "exit code: $rc" | tee -a "$OUT"

procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
rmdir "$CGROUP" 2>/dev/null

if [ $rc -eq 0 ] && echo "$out" | grep -q "^PASS$"; then
    echo "RESULT: PASS" | tee -a "$OUT"
else
    echo "RESULT: FAIL (rc=$rc)" | tee -a "$OUT"
fi
