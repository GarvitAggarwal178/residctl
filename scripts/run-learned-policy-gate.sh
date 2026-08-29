#!/bin/bash
# WP1 §1.2 verification gate: `layer_order_learned` must reproduce Campaign
# 12 Phase D's arm D numbers exactly at the deterministic cell
# (128 MiB, r=0.5, compute=0, threads=8, window=1). Phase D CSV rows
# 204-206: absent_handled=57, evictions=49, pager_bytes_fetched=7650410496.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
MODEL=$RESIDCTL/scratch/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_wp1gate
LOG=$RESIDCTL/experiments/logs/overnight__wp1_gate_log.txt
REGION_LEN=2147483648
CHUNK=134217728
BUDGET=1073741824
MARGIN=67108864
> "$LOG"
log() { echo "$@" | tee -a "$LOG"; }

fresh_cgroup() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"
    echo "$((BUDGET + MARGIN))" > "$CGROUP/memory.max"
    echo 0 > "$CGROUP/memory.swap.max"
}

EXPECT_ABSENT=57
EXPECT_EVICT=49
EXPECT_BYTES=7650410496

pass=1
for rep in 1 2 3; do
    fresh_cgroup
    out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 180 "$@"' -- \
          "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK" "$BUDGET" 5 \
          layer_order_learned off "" "" --fetch-workers 4 --driver-threads 8 \
          --lookahead-window 1 --compute-ns-per-mib 0 2>&1)
    rc=$?
    a=$(echo "$out" | grep -oP 'ARM_CSV,.*absent_handled=\K[0-9]+' | tail -1)
    e=$(echo "$out" | grep -oP 'ARM_CSV,.*evictions=\K[0-9]+' | tail -1)
    b=$(echo "$out" | grep -oP 'pager_bytes_fetched=\K[0-9]+' | tail -1)
    log "rep=$rep rc=$rc absent_handled=$a evictions=$e pager_bytes_fetched=$b"
    [ "$a" = "$EXPECT_ABSENT" ] && [ "$e" = "$EXPECT_EVICT" ] && [ "$b" = "$EXPECT_BYTES" ] || pass=0
    for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
done
rmdir "$CGROUP" 2>/dev/null
log ""
log "expected: absent_handled=$EXPECT_ABSENT evictions=$EXPECT_EVICT pager_bytes_fetched=$EXPECT_BYTES"
if [ "$pass" = 1 ]; then
    log "GATE: PASS -- layer_order_learned reproduces Campaign 12 Phase D arm D exactly"
    exit 0
else
    log "GATE: FAIL -- rename changed behaviour; nothing downstream is valid"
    exit 1
fi
