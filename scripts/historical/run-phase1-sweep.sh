#!/bin/bash
# Campaign 11 Phase 1: platform I/O concurrency, pager removed. Full literal
# grid from the spec: --threads {1,2,4,8} x --block-size {4MiB,128MiB} x
# {shared-fd,fd-per-thread} x {direct,buffered} = 32 configs x n=5 = 160
# runs (the spec's own text says "64 configurations x 5 = 320" -- 4x2x2x2=32,
# not 64; run the literal grid as specified and disclose the arithmetic
# mismatch in the report rather than padding it to match an unexplained
# number).
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CSV=$RESULTS/phase1_platform_io.csv
LOG=$RESULTS/phase1_platform_io_log.txt
> "$LOG"
> "$CSV"
echo "threads,block_size,fdmode,iomode,rep,wall_s,agg_MiBps,total_bytes,rc" > "$CSV"

TOTAL_BYTES=1073741824   # 1 GiB aggregate per trial
THREADS="1 2 4 8"
BLOCK_SIZES="4194304 134217728"   # 4 MiB, 128 MiB
FD_MODES="shared-fd fd-per-thread"
IO_MODES="direct buffered"

log() { echo "$@" | tee -a "$LOG"; }

log "=== machine exclusivity check (before) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
if pgrep -f "cn-spike|gate5r_driver|iperf3" > /dev/null 2>&1; then
    log "FATAL: a foreign workload appears to be running. Per instructions: do not kill it, stop and report."
    exit 1
fi

drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 >> "$LOG"; }

run_one() {
    local threads=$1 bs=$2 fdmode=$3 iomode=$4 rep=$5
    local fdflag ioflag
    [ "$fdmode" = "shared-fd" ] && fdflag="--shared-fd" || fdflag="--fd-per-thread"
    [ "$iomode" = "direct" ] && ioflag="--direct" || ioflag="--buffered"

    drop_caches
    local out rc
    out=$("$SRC/bench_concurrent_read" --threads "$threads" --block-size "$bs" "$fdflag" "$ioflag" \
          --total-bytes "$TOTAL_BYTES" "$MODEL" 2>&1)
    rc=$?
    log "--- threads=$threads bs=$bs fdmode=$fdmode iomode=$iomode rep=$rep rc=$rc ---"
    log "$out"

    local wall_s agg total
    wall_s=$(echo "$out" | grep -oP 'wall_s=\K[0-9.]+' | head -1)
    agg=$(echo "$out" | grep -oP 'agg_MiBps=\K[0-9.]+' | head -1)
    total=$(echo "$out" | grep -oP 'total_bytes=\K[0-9]+' | head -1)

    echo "$threads,$bs,$fdmode,$iomode,$rep,${wall_s:-},${agg:-},${total:-},$rc" >> "$CSV"
}

for threads in $THREADS; do
    for bs in $BLOCK_SIZES; do
        for fdmode in $FD_MODES; do
            for iomode in $IO_MODES; do
                for rep in 1 2 3 4 5; do
                    run_one "$threads" "$bs" "$fdmode" "$iomode" "$rep"
                done
            done
        done
    done
done

log ""
log "=== machine exclusivity check (after) ==="
uptime | tee -a "$LOG"
ps aux --sort=-%cpu 2>/dev/null | head -8 | tee -a "$LOG"
log ""
log "=== Phase 1 complete, results in $CSV ==="
