#!/bin/bash
# Runs exactly one arm-A random-mode rep, foreground, appends to the CSV.
# Usage: run_one_random_rep.sh <ratio> <rep>
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phase4_AB
CSV=$RESULTS/campaign12_phaseA_phase4_AB.csv
LOG=$RESULTS/campaign12_phaseA_phase4_AB_log.txt

REGION_LEN=2147483648
CHUNK_SIZE=134217728
N_PASSES=5
MARGIN=67108864
ratio=$1
rep=$2

budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
memmax=$((budget_bytes + MARGIN))

if [ -d "$CGROUP" ]; then
    procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
    if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; sleep 1; fi
    rmdir "$CGROUP" 2>/dev/null
fi
mkdir "$CGROUP"
echo "$memmax" > "$CGROUP/memory.max"
echo 0 > "$CGROUP/memory.swap.max"

sync; echo 3 > /proc/sys/vm/drop_caches 2>&1 | tee -a "$LOG"

out=$(bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 220 "$@"' -- \
    "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK_SIZE" "$N_PASSES" random off 2>&1)
rc=$?
echo "--- RANDOM-ONLY ratio=$ratio rep=$rep rc=$rc ---" | tee -a "$LOG"
echo "$out" | tee -a "$LOG"

touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
iord=$(echo "$out" | grep -oP 'ARM_CSV,.*io_read_bytes_delta=\K[0-9]+' | tail -1)

echo "$ratio,A,random,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},${iord:-},$rc" >> "$CSV"

procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
rmdir "$CGROUP" 2>/dev/null
echo "rc=$rc"
