#!/bin/bash
# Item 1 verification: happy path + two negative tests confirming the
# assertions actually fire (not just present in source), per the "never
# conclude success from absence of a crash" rule carried from the spike.
set -u
RESIDCTL=/root/residctl
BIN=$RESIDCTL/src/test_startup
MODEL=$RESIDCTL/scratch/test_model.bin
RESULTS=$RESIDCTL/results
CGROUP=/sys/fs/cgroup/residctl_item1
OUT=$RESULTS/item1_test_log.txt
> "$OUT"

log() { echo "$@" | tee -a "$OUT"; }

mkdir -p "$RESIDCTL/scratch" "$RESULTS"
if [ ! -f "$MODEL" ]; then
    dd if=/dev/urandom of="$MODEL" bs=1M count=16 status=none
fi

fresh_cgroup() {
    if [ -d "$CGROUP" ]; then
        procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
        if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
        rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo 536870912 > "$CGROUP/memory.max"   # 512 MiB, generous
}

run_case() {
    local name="$1" mode="$2" swap_setting="$3" expect="$4"  # expect: pass|abort
    fresh_cgroup
    if [ "$swap_setting" = "zero" ]; then
        echo 0 > "$CGROUP/memory.swap.max"
    fi
    # else: leave swap.max at its default (nonzero-capable) to trigger I-3

    log "=== case: $name (mode=$mode swap=$swap_setting expect=$expect) ==="
    swapmax=$(cat "$CGROUP/memory.swap.max")
    log "cgroup readback: memory.swap.max=$swapmax"

    manifest_out="$RESULTS/item1_manifest_${name}.txt"
    rm -f "$manifest_out"

    # Join the exec'd process to the cgroup BEFORE exec, using BASHPID (not $,
    # which does not update inside a subshell -- this exact bug was found and
    # documented in SPIKE_ADDENDUM2.md's Step 1).
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec "$@"' -- \
          "$BIN" "$mode" "$CGROUP" "$MODEL" "$manifest_out" 2>&1)
    rc=$?
    echo "$out" >> "$OUT"
    log "exit code: $rc"

    if [ "$expect" = "pass" ]; then
        if [ $rc -eq 0 ] && echo "$out" | grep -q "^PASS$"; then
            log "RESULT: PASS (expected success, got success)"
        else
            log "RESULT: FAIL (expected success, rc=$rc)"
        fi
    else
        # abort() raises SIGABRT -> shell exit code 134 (128+6)
        if [ $rc -eq 134 ] && echo "$out" | grep -q "STARTUP FAILED"; then
            log "RESULT: PASS (expected abort, got SIGABRT with STARTUP FAILED message)"
        else
            log "RESULT: FAIL (expected SIGABRT/134 with STARTUP FAILED message, got rc=$rc)"
        fi
    fi
    log ""

    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
}

run_case "happy"      "happy"      "zero"    "pass"
run_case "bad_swap"   "happy"      "nonzero" "abort"
run_case "bad_budget" "bad_budget" "zero"    "abort"

rmdir "$CGROUP" 2>/dev/null

log "=== item 1 test run complete ==="
pass_count=$(grep -c "^RESULT: PASS" "$OUT")
log "PASS count: $pass_count / 3"
