#!/bin/bash
CSV=/root/residctl/results/data/real-model-bytes-by-budget.csv
awk -F, 'NR>1 {
    gb_io=$17/1e9; gb_pbf=$18/1e9;
    printf "%-6s %-2s rep%s  rc=%-3s  io=%6.1fGB pbf=%6.1fGB  tok/s=%-5s wall=%-6s peak=%dMiB %s\n", \
        $1,$2,$5,$31, gb_io, gb_pbf, $16, $15, $26/1048576, $30
}' "$CSV"
echo "--- rows: $(( $(wc -l < "$CSV") - 1 ))/60 ---"
pgrep -a wp2_gen >/dev/null && echo "running: $(ps -o etimes= -C wp2_gen | head -1)s" || echo "(no wp2_gen running)"
tail -1 /root/residctl/experiments/logs/final__phase1_equal_budget_log.txt
uptime
