#!/bin/bash
# Self-healing supervisor for the Phase 2 grid. Relaunches the resumable grid
# script if the shell is interrupted. Exits when both CSVs are complete.
DCSV=/root/residctl/results/data/consumption-signal-determinism-grid.csv
SCSV=/root/residctl/results/data/consumption-signal-comparison.csv
CG=/sys/fs/cgroup/residctl_final_p2
DET_TARGET=121   # 1 + 4 combos * 6 cells * 5 reps
SW_TARGET=145    # 1 + 4 combos * 2cs * 3r * 2c * 3reps

while :; do
    d=$(wc -l < "$DCSV" 2>/dev/null || echo 0)
    s=$(wc -l < "$SCSV" 2>/dev/null || echo 0)
    if [ "$d" -ge "$DET_TARGET" ] && [ "$s" -ge "$SW_TARGET" ]; then
        echo "$(date +%H:%M:%S) DONE det=$d sweep=$s"; break
    fi
    if ! pgrep -f run_final_phase2.sh >/dev/null; then
        echo "$(date +%H:%M:%S) grid not running (det=$d sweep=$s) -- relaunching"
        pkill -9 -f "replay_main .*residctl_final_p2" 2>/dev/null
        sleep 2
        if [ -d "$CG" ]; then
            for p in $(cat "$CG/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
            sleep 1; rmdir "$CG" 2>/dev/null
        fi
        cd /root/residctl && setsid bash scripts/run-consumption-signal-sweep.sh >> /root/residctl-archive/process-logs/results__final__phase2_console.txt 2>&1 < /dev/null &
        disown
        sleep 5
    else
        echo "$(date +%H:%M:%S) det=$d sweep=$s alive"
    fi
    sleep 90
done
