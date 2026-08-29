#!/bin/bash
set -u
cd /root/residctl
CG=/sys/fs/cgroup/residctl_smoke
[ -d $CG ] && { for p in $(cat $CG/cgroup.procs 2>/dev/null); do kill -9 $p; done; sleep 1; rmdir $CG 2>/dev/null; }
mkdir $CG
echo $((1073741824+67108864)) > $CG/memory.max
echo 0 > $CG/memory.swap.max
run() {
  local flags="$1"
  bash -c "echo \$BASHPID > $CG/cgroup.procs; exec timeout 120 src/replay_main $CG scratch/pattern_2g.bin 2147483648 134217728 1073741824 3 layer_order_declared off '' '' --fetch-workers 4 --driver-threads 8 --lookahead-window 1 --compute-ns-per-mib 400000 $flags" 2>&1 | grep -E "ARM_CSV|consumption_signal=|abort|FAIL|replay_cyclic_mt:"
  for p in $(cat $CG/cgroup.procs 2>/dev/null); do kill -9 $p 2>/dev/null; done
}
echo "=== tid0 + on (default, unchanged path) ==="
run "--consumption-signal tid0 --protect-current on"
echo "=== all-threads + off (intended fix) ==="
run "--consumption-signal all-threads --protect-current off"
echo "=== tid0 + off (session-1 broken) ==="
run "--consumption-signal tid0 --protect-current off"
rmdir $CG 2>/dev/null
