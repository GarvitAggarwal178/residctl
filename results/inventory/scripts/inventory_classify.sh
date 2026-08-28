#!/bin/bash
# INVENTORY Step 2 + 3 + 4. Analysis only.
set -u
cd /root/residctl
I=results/inventory
mkdir -p "$I"
git ls-files > /tmp/inv_tracked.txt

bucket() {
    local f="$1"
    case "$f" in
        third_party/*)                    echo THIRD_PARTY;;
        models/*.gguf|scratch/pattern_*.bin|scratch/*model*.bin) echo MODEL;;
        scratch/*)                        echo SCRATCH;;
        results/inventory/*)              echo REPORT;;   # this pass's own output
        *.o|*/build/*|src/wp2_obj/*)       echo BUILD;;
        src/Makefile|src/*.c|src/*.h|src/*.cpp|src/*.patch) echo SOURCE;;
        src/run_correctness_harness.sh|src/run_t6_t7.sh|src/run_item*_tests.sh) echo TEST;;
        src/test_*)                       echo TEST;;
        src/*.sh)                         echo SWEEP_SCRIPT;;
        src/gen_pattern|src/belady_main|src/replay_main|src/baseline_main|src/bench_concurrent_read|src/wp2_gen|src/wp2_opt) echo BUILD;;
        src/*)                            echo BUILD;;    # remaining compiled test binaries
        docs/*|CLAUDE.md)                 echo SPEC;;
        results/PROJECT_STATE.md|results/overnight/CLAIMS.md|results/final/WRITEUP_PACKAGE.md|results/overnight/figures/table*_final*|results/overnight/figures/table1_main_results.*|results/overnight/figures/table2_environment.*) echo DELIVERABLE;;
        results/overnight/figures/*)      echo FIGURE;;
        results/**/*.md|results/*.md)      echo REPORT;;
        results/**/make_wp3.py|scratch/make_wp3.py) echo TOOL;;
        results/**/*.csv|results/*.csv|results/**/*.txt|results/*.txt|results/**/*.bin|results/**/*.reftrace|results/**/*.trace|results/**/*.cfg|results/**/*.log|results/**/*.json) echo DATA;;   # refined below by citation
        *.md)                            echo REPORT;;
        *)                               echo UNKNOWN;;
    esac
}

echo "path,bucket,size,git_tracked,reason" > "$I/classification.csv"
# tracked files: classify individually
while read -r f; do
    [ -f "$f" ] || continue
    b=$(bucket "$f"); sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
    printf '%s,%s,%s,yes,\n' "$f" "$b" "$sz" >> "$I/classification.csv"
done < /tmp/inv_tracked.txt
# untracked, non-third_party (third_party is one bucket, summarised separately)
find . -path ./.git -prune -o -path ./third_party -prune -o -type f -print | sed 's|^\./||' \
  | grep -vxFf /tmp/inv_tracked.txt | while read -r f; do
    b=$(bucket "$f"); sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
    printf '%s,%s,%s,no,\n' "$f" "$b" "$sz" >> "$I/classification.csv"
done
# third_party as a single rolled-up line
tp_n=$(find third_party -type f | wc -l); tp_sz=$(du -sb third_party | cut -f1)
printf 'third_party/ (%s files rolled up),THIRD_PARTY,%s,no,llama.cpp clone incl build/ + node_modules\n' "$tp_n" "$tp_sz" >> "$I/classification.csv"

echo "=== per-bucket (count, MB, tracked) ==="
awk -F, 'NR>1{c[$2]++; b[$2]+=$3; if($4=="yes")t[$2]++}
  END{for(k in c) printf "%-14s %6d files  %10.1f MB  %d tracked\n", k, c[k], b[k]/1048576, t[k]+0}' "$I/classification.csv" | sort

echo
echo "=== Step 4 bloat/hygiene ==="
echo "Q1 GGUF in HEAD?  $(git ls-files | grep -c '\.gguf$') tracked; in history? $(grep -c 'gguf' results/inventory/raw/git_history_size.txt) blobs"
echo "Q2 llama.cpp: tracked=$(git ls-files | grep -c '^third_party/') files; size=$(du -sh third_party 2>/dev/null | cut -f1); nested .git? $([ -e third_party/llama.cpp/.git ] && echo YES || echo no)"
echo "Q3 tracked binaries/.o: $(git ls-files | grep -Ec '\.o$|^src/(test_|wp2_gen$|wp2_opt$|replay_main$|belady_main$|baseline_main$|gen_pattern$|bench_)')"
echo "Q4 scratch/ tracked: $(git ls-files | grep -c '^scratch/')"
echo "Q5 .gitignore:"; sed 's/^/     /' .gitignore 2>/dev/null || echo "     (no .gitignore)"
echo "census + classify done -> $I/"
