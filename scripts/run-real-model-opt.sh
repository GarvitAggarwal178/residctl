#!/bin/bash
# Phase 1 OPT: offline Belady/MIN over the declared consumption sequence with
# unequal chunk sizes, at the 5 equal-budget ratios. Same chunk table + declared
# sequence as WP2 (identical model + build). n_passes = 65 (1 prefill + 64
# decode layer-scans), matching WP2's wp2_opt.csv; also emits a 64-pass column.
set -u
R=/root/residctl/results
OUT=$R/final
INV=$R/overnight/wp2_tensor_inventory.txt
REGION=2104934400

python3 - <<'PY'
import re
inv = open('/root/residctl/results/overnight/wp2_tensor_inventory.txt').read()
cb = re.search(r'## chunk_bytes.*?\n(.*)', inv, re.S).group(1)
open('/tmp/p1_cb.txt','w').write('\n'.join(l for l in cb.splitlines() if l.strip().isdigit())+'\n')
ds = re.search(r'## declared sequence[^\n]*\n\s*([\d ]+)', inv).group(1)
open('/tmp/p1_ds.txt','w').write('\n'.join(ds.split())+'\n')
print("chunk_bytes:", sum(1 for l in open('/tmp/p1_cb.txt') if l.strip()),
      "declared seq:", sum(1 for l in open('/tmp/p1_ds.txt') if l.strip()))
PY

echo "ratio,budget_bytes,passes,opt_misses,opt_missed_bytes,sum_chunk_bytes,equal_size" > "$OUT/phase1_opt.csv"
for passes in 65 64; do
  for ratio in 0.25 0.375 0.5 0.625 0.75; do
    B=$(awk "BEGIN{printf \"%d\", $REGION*$ratio}")
    out=$(/root/residctl/src/wp2_opt /tmp/p1_cb.txt "$B" "$passes" /tmp/p1_ds.txt 2>&1)
    echo "passes=$passes r=$ratio: $out"
    m=$(echo "$out"  | grep -oP 'opt_misses=\K[0-9]+')
    mb=$(echo "$out" | grep -oP 'opt_missed_bytes=\K[0-9]+')
    sb=$(echo "$out" | grep -oP 'sum_chunk_bytes=\K[0-9]+')
    eq=$(echo "$out" | grep -oP 'equal_size=\K[a-z]+')
    echo "$ratio,$B,$passes,${m:-},${mb:-},${sb:-},${eq:-}" >> "$OUT/phase1_opt.csv"
  done
done
echo "--- phase1_opt.csv ---"; cat "$OUT/phase1_opt.csv"
