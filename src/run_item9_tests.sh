#!/bin/bash
# Build-order item 9 verification: Belady offline optimal solver (§10).
set -u
RESIDCTL=/root/residctl
BELADY=$RESIDCTL/src/belady_main
REPLAY=$RESIDCTL/src/replay_main
GEN=$RESIDCTL/src/gen_pattern
MODEL=$RESIDCTL/scratch/pattern_16m.bin
TRACE=$RESIDCTL/scratch/item9_trace.bin
RESULTS=$RESIDCTL/results
CGROUP=/sys/fs/cgroup/residctl_item9
OUT=$RESULTS/item9_test_log.txt
> "$OUT"

mkdir -p "$RESIDCTL/scratch" "$RESULTS"
[ -f "$MODEL" ] || "$GEN" "$MODEL" 16777216

echo "--- selftest ---" | tee -a "$OUT"
"$BELADY" --selftest 2>&1 | tee -a "$OUT"
selftest_rc=${PIPESTATUS[0]}
echo "selftest exit: $selftest_rc" | tee -a "$OUT"

echo "--- generate a real trace via replay_main (layer_order, same scenario as items 6-8) ---" | tee -a "$OUT"
if [ -d "$CGROUP" ]; then
    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
    rmdir "$CGROUP" 2>/dev/null
fi
mkdir "$CGROUP"
echo 536870912 > "$CGROUP/memory.max"
echo 0 > "$CGROUP/memory.swap.max"
rm -f "$TRACE"

bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec "$@"' -- \
  "$REPLAY" "$CGROUP" "$MODEL" 16777216 2097152 6291456 5 layer_order "$TRACE" 2>&1 | tee -a "$OUT"
replay_rc=${PIPESTATUS[0]}

procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
rmdir "$CGROUP" 2>/dev/null

echo "--- run belady_main against that trace (chunk_size=2097152, budget=6291456) ---" | tee -a "$OUT"
belady_out=$("$BELADY" "$TRACE" 2097152 6291456 2>&1)
belady_rc=$?
echo "$belady_out" | tee -a "$OUT"

# Consistency check: offline OPT can never need MORE fetches than what the
# live online policy actually achieved on the same demand sequence -- if it
# did, that alone would prove a bug (in the live policy, the solver, or
# both), regardless of any other numbers.
opt_misses=$(echo "$belady_out" | grep -oP 'minimum_misses=\K[0-9]+')
online_faults=$(grep -oP 'absent_handled=\K[0-9]+' "$RESULTS/item9_test_log.txt" | head -1)
consistency_ok=1
if [ -n "$opt_misses" ] && [ -n "$online_faults" ]; then
    if [ "$opt_misses" -gt "$online_faults" ]; then
        echo "CONSISTENCY FAIL: OPT misses ($opt_misses) > online policy faults ($online_faults) -- impossible, this is a bug" | tee -a "$OUT"
        consistency_ok=0
    else
        echo "consistency check: OPT misses ($opt_misses) <= online policy faults ($online_faults) -- OK" | tee -a "$OUT"
    fi
else
    echo "consistency check: could not parse both values, skipped" | tee -a "$OUT"
    consistency_ok=0
fi

echo "" | tee -a "$OUT"
if [ "$selftest_rc" -eq 0 ] && [ "$replay_rc" -eq 0 ] && [ "$belady_rc" -eq 0 ] && [ "$consistency_ok" -eq 1 ]; then
    echo "RESULT: PASS" | tee -a "$OUT"
else
    echo "RESULT: FAIL (selftest_rc=$selftest_rc replay_rc=$replay_rc belady_rc=$belady_rc consistency_ok=$consistency_ok)" | tee -a "$OUT"
fi
