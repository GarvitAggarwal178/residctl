#!/bin/bash
set -u
O=/root/residctl/results/overnight
python3 - <<'PY'
import re
inv = open('/root/residctl/results/overnight/wp2_tensor_inventory.txt').read()
cb = re.search(r'## chunk_bytes.*?\n(.*)', inv, re.S).group(1)
open('/tmp/cb.txt','w').write('\n'.join(l for l in cb.splitlines() if l.strip().isdigit())+'\n')
ds = re.search(r'## declared sequence[^\n]*\n\s*([\d ]+)', inv).group(1)
open('/tmp/ds.txt','w').write('\n'.join(ds.split())+'\n')
print("chunk_bytes:", sum(1 for l in open('/tmp/cb.txt') if l.strip()),
      " declared seq:", sum(1 for l in open('/tmp/ds.txt') if l.strip()))
PY
REGION=2104934400
PASSES=${1:-65}
echo "ratio,budget_bytes,opt_misses,opt_missed_bytes" > "$O/wp2_opt.csv"
for ratio in 0.25 0.5 0.75; do
    B=$(awk "BEGIN{printf \"%d\", $REGION*$ratio}")
    out=$(/root/residctl/src/wp2_opt /tmp/cb.txt "$B" "$PASSES" /tmp/ds.txt 2>&1)
    echo "r=$ratio: $out"
    m=$(echo "$out" | grep -oP 'opt_misses=\K[0-9]+')
    mb=$(echo "$out" | grep -oP 'opt_missed_bytes=\K[0-9]+')
    echo "$ratio,$B,${m:-},${mb:-}" >> "$O/wp2_opt.csv"
done
