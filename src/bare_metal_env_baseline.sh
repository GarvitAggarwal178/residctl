#!/bin/bash
# bare_metal_env_baseline.sh -- Campaign 13 Phase D. Run once, before
# anything else, on the bare-metal machine. Usage:
#   bash bare_metal_env_baseline.sh <path-to-model-file>
set -u
MODEL_PATH="$1"
{
echo "=== uname -r ==="
uname -r
echo
echo "=== nproc ==="
nproc
echo
echo "=== free -m ==="
free -m
echo
echo "=== mount (root fs) ==="
mount | grep " / "
echo
echo "=== filesystem of model file ==="
df -T "$MODEL_PATH"
echo
echo "=== cgroup version ==="
stat -fc %T /sys/fs/cgroup/    # "cgroup2fs" = v2 (required); "tmpfs" = v1
echo
echo "=== transparent_hugepage/enabled ==="
cat /sys/kernel/mm/transparent_hugepage/enabled
echo
echo "=== transparent_hugepage/shmem_enabled ==="
cat /sys/kernel/mm/transparent_hugepage/shmem_enabled
echo
echo "=== unprivileged_userfaultfd ==="
cat /proc/sys/vm/unprivileged_userfaultfd 2>&1
echo
echo "=== swaps ==="
cat /proc/swaps
echo

DEV=$(df "$MODEL_PATH" | tail -1 | awk '{print $1}')
DEVNAME=$(basename "$DEV" | sed 's/[0-9]*$//')

echo "=== device model ($DEVNAME) ==="
cat "/sys/block/$DEVNAME/device/model" 2>&1
echo
echo "=== scheduler ($DEVNAME) ==="
cat "/sys/block/$DEVNAME/queue/scheduler" 2>&1
echo
echo "=== read_ahead_kb ($DEVNAME) ==="
cat "/sys/block/$DEVNAME/queue/read_ahead_kb" 2>&1
echo
echo "=== logical_block_size ($DEVNAME) ==="
cat "/sys/block/$DEVNAME/queue/logical_block_size" 2>&1
echo
echo "=== rotational (0=SSD/NVMe, 1=spinning) ($DEVNAME) ==="
cat "/sys/block/$DEVNAME/queue/rotational" 2>&1
} | tee bare_metal_env_baseline.txt
