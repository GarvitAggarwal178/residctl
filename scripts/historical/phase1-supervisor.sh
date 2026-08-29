#!/bin/bash
# Self-healing supervisor for the Phase 1 sweep: if the sweep script is not
# running and the CSV is incomplete, clean orphans and relaunch (the sweep
# resumes from its CSV row count). Exits when the CSV has all 60 data rows.
CSV=/root/residctl/results/data/real-model-bytes-by-budget.csv
CG=/sys/fs/cgroup/residctl_final_p1
TARGET=61   # header + 60 data rows

while :; do
    rows=$(wc -l < "$CSV" 2>/dev/null || echo 0)
    [ "$rows" -ge "$TARGET" ] && { echo "$(date +%H:%M:%S) DONE rows=$rows"; break; }
    if ! pgrep -f run_final_phase1.sh >/dev/null; then
        echo "$(date +%H:%M:%S) sweep not running (rows=$rows) -- cleaning + relaunching"
        pkill -9 -f "wp2_gen -m /root/residctl/models/model.gguf" 2>/dev/null
        sleep 2
        if [ -d "$CG" ]; then
            for p in $(cat "$CG/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
            sleep 1; rmdir "$CG" 2>/dev/null
        fi
        cd /root/residctl && setsid bash scripts/run-real-model-equal-budget.sh >> /root/residctl-archive/process-logs/results__final__phase1_console.txt 2>&1 < /dev/null &
        disown
        sleep 5
    else
        echo "$(date +%H:%M:%S) rows=$rows sweep alive"
    fi
    sleep 90
done
