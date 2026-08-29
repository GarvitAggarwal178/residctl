#!/bin/bash
set -u
RESIDCTL=/root/residctl
GEN=$RESIDCTL/src/gen_pattern
MODEL=$RESIDCTL/scratch/pattern_16m.bin
RESULTS=$RESIDCTL/results
CGROUP=/sys/fs/cgroup/residctl_t6t7
OUT=$RESULTS/t6_t7_log.txt
> "$OUT"

mkdir -p "$RESIDCTL/scratch" "$RESULTS"
[ -f "$MODEL" ] || "$GEN" "$MODEL" 16777216

fresh_cgroup() {
    if [ -d "$CGROUP" ]; then
        local procs
        procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
        if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
        rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo 536870912 > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
}
cleanup() {
    local procs
    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
}

echo "=== T-6 (dedup branches fire under async handler, 60s) ===" | tee -a "$OUT"
fresh_cgroup
out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 90 "$@"' -- \
      "$RESIDCTL/src/test_t6" "$CGROUP" "$MODEL" 60 2>&1)
rc6=$?
echo "$out" | tee -a "$OUT"
echo "exit code: $rc6" | tee -a "$OUT"
if [ $rc6 -eq 124 ]; then echo "TIMED OUT" | tee -a "$OUT"; fi
cleanup
rmdir "$CGROUP" 2>/dev/null

echo "" | tee -a "$OUT"
echo "=== T-7 (no fault ever lost, storm + 120s internal watchdog, 60s storm) ===" | tee -a "$OUT"
fresh_cgroup
out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 200 "$@"' -- \
      "$RESIDCTL/src/test_t7" "$CGROUP" "$MODEL" 60 2>&1)
rc7=$?
echo "$out" | tee -a "$OUT"
echo "exit code: $rc7" | tee -a "$OUT"
if [ $rc7 -eq 124 ]; then echo "OUTER TIMEOUT (200s) -- watchdog itself may be stuck" | tee -a "$OUT"; fi
cleanup
rmdir "$CGROUP" 2>/dev/null

echo "" | tee -a "$OUT"
if [ $rc6 -eq 0 ] && [ $rc7 -eq 0 ]; then
    echo "RESULT: PASS (T-6, T-7)" | tee -a "$OUT"
else
    echo "RESULT: FAIL (T-6 rc=$rc6, T-7 rc=$rc7)" | tee -a "$OUT"
fi
