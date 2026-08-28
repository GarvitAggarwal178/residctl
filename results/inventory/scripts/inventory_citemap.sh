#!/bin/bash
# INVENTORY Step 3 (citation map) + finish Step 4. Analysis only.
set -u
cd /root/residctl
I=results/inventory

echo "=== 16 UNKNOWN files ==="
awk -F, '$2=="UNKNOWN"{print $1}' "$I/classification.csv"
echo

# --- Step 4 Q6: duplicate files (content hash), tracked files > 1 KiB ---
echo "=== Q6 duplicate tracked files (>1KiB, same sha256) ==="
git ls-files | while read -r f; do
  [ -f "$f" ] || continue
  sz=$(stat -c %s "$f"); [ "$sz" -gt 1024 ] || continue
  printf '%s  %s\n' "$(sha256sum "$f" | cut -c1-16)" "$f"
done | sort | awk '{h[$1]=h[$1]" "$2; n[$1]++} END{for(k in n) if(n[k]>1) print k":"h[k]}'
echo

# --- Step 3: citation map ---
# data files: everything in DATA + FIGURE + REPORT buckets, tracked
mapfile -t DATAFILES < <(awk -F, '($2=="DATA"||$2=="FIGURE"||$2=="REPORT"||$2=="DELIVERABLE") && $4=="yes"{print $1}' "$I/classification.csv")
mapfile -t DOCS < <(git ls-files '*.md')

echo "data_file,citation_count,cited_by" > "$I/citation_map.csv"
> "$I/orphans.txt"
> "$I/load_bearing.txt"
LB_DOCS='WRITEUP_PACKAGE.md|CLAIMS.md|PROJECT_STATE.md|phase1_deadlock_fix.md|phase2_claims_reconciled.md|RELATED_WORK.md|CLEANUP_SUMMARY.md|FINAL_SUMMARY.md|phase1_equal_budget.md|phase2_consumption_signal.md|phase3_arm_e_collapse.md|phase4_figures.md'

for df in "${DATAFILES[@]}"; do
  base=$(basename "$df")
  # search every tracked .md for the basename or the full path
  hits=$(grep -rlF -e "$base" -e "$df" -- $(git ls-files '*.md') 2>/dev/null | sort -u)
  cnt=$(echo "$hits" | grep -c . )
  [ -z "$hits" ] && cnt=0
  csv_hits=$(echo "$hits" | tr '\n' ';' | sed 's/;$//')
  echo "$df,$cnt,\"$csv_hits\"" >> "$I/citation_map.csv"
  if [ "$cnt" -eq 0 ]; then
    mt=$(stat -c %y "$df" | cut -d' ' -f1)
    echo "$df    (mtime $mt)" >> "$I/orphans.txt"
  fi
  if echo "$hits" | grep -qE "$LB_DOCS"; then
    echo "$df" >> "$I/load_bearing.txt"
  fi
done

echo "=== citation map summary ==="
echo "data files scanned: ${#DATAFILES[@]}"
echo "orphans (cited by nothing): $(grep -c . "$I/orphans.txt")"
echo "load-bearing (cited by a deliverable/final report): $(grep -c . "$I/load_bearing.txt")"
echo

# --- Step 3 cross-check: every source cell in WRITEUP_PACKAGE.md §1 must resolve ---
echo "=== WRITEUP_PACKAGE.md source-cell resolution (dangling = BAD) ==="
grep -oE '`[a-zA-Z0-9_./-]+\.(csv|md|txt|log|png)`' results/final/WRITEUP_PACKAGE.md | tr -d '`' | sort -u | while read -r ref; do
  found=$(git ls-files | grep -E "(^|/)$(basename "$ref")$" | head -1)
  if [ -n "$found" ]; then echo "  OK   $ref -> $found"; else echo "  DANGLING  $ref"; fi
done
echo "citemap done"
