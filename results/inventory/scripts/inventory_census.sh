#!/bin/bash
# INVENTORY Step 1 -- raw census. Analysis only, writes results/inventory/raw/.
set -u
cd /root/residctl
R=results/inventory/raw
mkdir -p "$R"

# gitignore membership (batch)
git ls-files > /tmp/inv_tracked.txt
git check-ignore --stdin < <(find . -path ./.git -prune -o -type f -print | sed 's|^\./||') 2>/dev/null > /tmp/inv_ignored.txt || true

# 1. all_files.csv
echo "path,size_bytes,extension,mtime,git_tracked,in_gitignore" > "$R/all_files.csv"
find . -path ./.git -prune -o -type f -print | sed 's|^\./||' | while read -r f; do
    sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
    mt=$(stat -c %y "$f" 2>/dev/null | cut -d. -f1)
    ext="${f##*.}"; [ "$ext" = "$f" ] && ext="(none)"
    grep -qxF "$f" /tmp/inv_tracked.txt && tr=yes || tr=no
    grep -qxF "$f" /tmp/inv_ignored.txt && ig=yes || ig=no
    printf '%s,%s,%s,%s,%s,%s\n' "$f" "$sz" "$ext" "$mt" "$tr" "$ig" >> "$R/all_files.csv"
done

# 2. repo_size.txt
{
    echo "=== working tree size (excl .git) ==="
    du -sh --exclude=.git . 2>/dev/null
    echo "=== .git size ==="
    du -sh .git 2>/dev/null
    echo "=== tracked file count ==="; wc -l < /tmp/inv_tracked.txt
    echo "=== untracked file count ==="
    find . -path ./.git -prune -o -type f -print | sed 's|^\./||' | grep -vxFf /tmp/inv_tracked.txt | wc -l
    echo
    echo "=== 30 largest TRACKED files ==="
    git ls-files | while read -r f; do printf '%s\t%s\n' "$(stat -c %s "$f" 2>/dev/null || echo 0)" "$f"; done | sort -rn | head -30 | awk -F'\t' '{printf "%12d  %s\n", $1, $2}'
    echo
    echo "=== 30 largest UNTRACKED files ==="
    find . -path ./.git -prune -o -type f -print | sed 's|^\./||' | grep -vxFf /tmp/inv_tracked.txt | while read -r f; do printf '%s\t%s\n' "$(stat -c %s "$f" 2>/dev/null || echo 0)" "$f"; done | sort -rn | head -30 | awk -F'\t' '{printf "%12d  %s\n", $1, $2}'
} > "$R/repo_size.txt"

# 3. git_history_size.txt -- largest objects EVER committed
{
    echo "=== 25 largest blobs in git history (any commit) ==="
    git rev-list --objects --all | git cat-file --batch-check='%(objecttype) %(objectname) %(objectsize) %(rest)' 2>/dev/null \
      | awk '/^blob/ {print $3, $4}' | sort -rn | head -25 | awk '{printf "%12d  %s\n", $1, $2}'
    echo
    echo "=== total pack size ==="
    du -sh .git/objects 2>/dev/null
} > "$R/git_history_size.txt"

# 4. file_type_summary.csv
echo "extension,count,total_bytes" > "$R/file_type_summary.csv"
tail -n +2 "$R/all_files.csv" | awk -F, '{c[$3]++; b[$3]+=$2} END{for(e in c) printf "%s,%d,%d\n", e, c[e], b[e]}' | sort -t, -k3 -rn >> "$R/file_type_summary.csv"

echo "=== headline ==="
grep -A0 "working tree size" -A1 "$R/repo_size.txt" | tail -1
grep -A1 ".git size" "$R/repo_size.txt" | tail -1
echo "tracked: $(($(wc -l < /tmp/inv_tracked.txt)))"
echo "census done -> $R/"
