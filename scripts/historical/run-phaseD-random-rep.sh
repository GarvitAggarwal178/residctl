#!/bin/bash
# Campaign 12 Phase D: single-purpose helper for arm A's "random" madvise
# mode, run synchronously in the foreground. Same rationale as Phase A's
# run_one_random_rep.sh: this mode is known-slow (160-220s/rep) and
# repeatedly landed at this session's background-task window boundary,
# orphaning the child process and losing its result. Usage:
#   run_phaseD_random_rep.sh <chunk_size> <ratio> <rep>
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
RESULTS=$RESIDCTL/results
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_phaseD_rand
CSV=$RESULTS/campaign12_phaseD_paper_table.csv
LOG=$RESULTS/campaign12_phaseD_paper_table_log.txt

REGION_LEN=2147483648
N_PASSES=5
MARGIN=67108864

cs=$1
ratio=$2
rep=$3
budget_bytes=$(awk "BEGIN{printf \"%d\", $REGION_LEN*$ratio}")
memmax=$((budget_bytes + MARGIN))

log() { echo "$@" | tee -a "$LOG"; }

already=$(awk -F, -v cs="$cs" -v r="$ratio" -v rp="$rep" \
    'NR>1 && $1==cs && $2==r && $3=="A" && $4=="random" && $6==rp {found=1} END{print found+0}' "$CSV")
if [ "$already" = "1" ]; then
    log "SKIP (done): cs=$cs ratio=$ratio arm=A mode=random rep=$rep"
    exit 0
fi

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
      "$SRC/baseline_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$cs" "$N_PASSES" random off 2>&1)
rc=$?
log "--- cs=$cs ratio=$ratio arm=A mode=random rep=$rep rc=$rc ---"
log "$out"

touches=$(echo "$out" | grep -oP 'ARM_CSV,.*touches=\K[0-9]+' | tail -1)
bytes_touched=$(echo "$out" | grep -oP 'ARM_CSV,.*bytes_touched=\K[0-9]+' | tail -1)
wall_ns=$(echo "$out" | grep -oP 'ARM_CSV,.*wall_ns=\K[0-9]+' | tail -1)
iord=$(echo "$out" | grep -oP 'ARM_CSV,.*io_read_bytes_delta=\K[0-9]+' | tail -1)

echo "$cs,$ratio,A,random,n/a,$rep,${touches:-},${bytes_touched:-},${wall_ns:-},n/a,n/a,n/a,n/a,n/a,${iord:-},n/a,n/a,n/a,$rc" >> "$CSV"

procs=$(cat "$CGROUP/cgroup.procs" 2>/dev/null)
if [ -n "$procs" ]; then for p in $procs; do kill -9 "$p" 2>/dev/null; done; fi
rmdir "$CGROUP" 2>/dev/null
