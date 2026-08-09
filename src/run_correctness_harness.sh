#!/bin/bash
# MECHANISM_SPEC.md §13 correctness harness: T-1, T-2, T-3, T-4 (T-5 is
# belady_main --selftest, item 9). Must pass before any performance number
# from item 10's harness is treated as trustworthy.
set -u
RESIDCTL=/root/residctl
GEN=$RESIDCTL/src/gen_pattern
MODEL=$RESIDCTL/scratch/pattern_16m.bin
RESULTS=$RESIDCTL/results
CGROUP=/sys/fs/cgroup/residctl_correctness
OUT=$RESULTS/correctness_harness_log.txt
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

echo "=== T-1, T-2, T-4 (test_correctness) ===" | tee -a "$OUT"
fresh_cgroup
out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 30 "$@"' -- \
      "$RESIDCTL/src/test_correctness" "$CGROUP" "$MODEL" 2>&1)
rc1=$?
echo "$out" | tee -a "$OUT"
echo "exit code: $rc1" | tee -a "$OUT"
cleanup

echo "" | tee -a "$OUT"
echo "=== T-3 (test_storm, 60s, timeout-guarded at 90s) ===" | tee -a "$OUT"
fresh_cgroup
out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 90 "$@"' -- \
      "$RESIDCTL/src/test_storm" "$CGROUP" "$MODEL" 60 2>&1)
rc3=$?
echo "$out" | tee -a "$OUT"
echo "exit code: $rc3" | tee -a "$OUT"
if [ $rc3 -eq 124 ]; then
    echo "TIMED OUT -- possible hang, T-3 FAILED" | tee -a "$OUT"
fi
cleanup
rmdir "$CGROUP" 2>/dev/null

echo "" | tee -a "$OUT"
echo "=== T-5 (belady_main --selftest, item 9) ===" | tee -a "$OUT"
"$RESIDCTL/src/belady_main" --selftest 2>&1 | tee -a "$OUT"
rc5=${PIPESTATUS[0]}
echo "exit code: $rc5" | tee -a "$OUT"

echo "" | tee -a "$OUT"
if [ $rc1 -eq 0 ] && [ $rc3 -eq 0 ] && [ $rc5 -eq 0 ]; then
    echo "RESULT: PASS (all of T-1, T-2, T-3, T-4, T-5)" | tee -a "$OUT"
else
    echo "RESULT: FAIL (T1/T2/T4 rc=$rc1, T3 rc=$rc3, T5 rc=$rc5)" | tee -a "$OUT"
fi
