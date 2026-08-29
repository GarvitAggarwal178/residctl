#!/bin/bash
CSV=/root/residctl/results/data/prefetch-collapse-fallback-configs.csv
CG=/sys/fs/cgroup/residctl_final_p3
TARGET=17   # 1 header + 1 traced + 5 configs * 3 reps
while :; do
    n=$(wc -l < "$CSV" 2>/dev/null || echo 0)
    [ "$n" -ge "$TARGET" ] && { echo "$(date +%H:%M:%S) DONE rows=$n"; break; }
    if ! pgrep -f run_final_phase3.sh >/dev/null; then
        echo "$(date +%H:%M:%S) not running (rows=$n) -- relaunching"
        pkill -9 -f "wp2_gen -m /root/residctl/models/model.gguf" 2>/dev/null; sleep 2
        [ -d "$CG" ] && { for p in $(cat "$CG/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done; sleep 1; rmdir "$CG" 2>/dev/null; }
        cd /root/residctl && setsid bash scripts/run-prefetch-collapse-probe.sh >> /root/residctl-archive/process-logs/results__final__phase3_console.txt 2>&1 < /dev/null & disown
        sleep 5
    else
        echo "$(date +%H:%M:%S) rows=$n alive"
    fi
    sleep 90
done
