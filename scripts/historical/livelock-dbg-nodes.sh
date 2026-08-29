#!/bin/bash
# Rebuild an instrumented copy of the CURRENT wp2_gen.cpp and dump the distinct
# compute-graph node names eval_cb() sees. Evidence tool for the Phase 0 GATE
# finding (token_embd node is "embd", not "inp_embd"). Regenerates the
# instrumented .cpp from live source each run so it never goes stale.
set -u
SRC=/root/residctl/src
LC=/root/residctl/third_party/llama.cpp
BIN=$LC/build/bin
OUT=/root/residctl/results/livelock
MODEL=/root/residctl/models/model.gguf
CG=/sys/fs/cgroup/residctl_livelock_p0
INC="-I$LC/include -I$LC/ggml/include -I$SRC"
PAGER_C="region.c pager.c fetch.c budget.c trace.c metrics.c cgroup_stat.c policy.c prefetch.c prefetch_pool.c fetch_trace.c policy_trace.c"

python3 - "$SRC/wp2_gen.cpp" "$OUT/wp2_gen_dbg.cpp" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
needle = "static bool eval_cb(struct ggml_tensor * t, bool ask, void * ud) {\n"
inj = ('#include <set>\n'
       'static std::set<std::string> g_seen_names;\n'
       + needle +
       '    if (getenv("WP2_DBG_NAMES")) {\n'
       '        std::string k = std::string(ask?"ask  ":"eval ") + t->name;\n'
       '        if (g_seen_names.insert(k).second) fprintf(stderr, "NODE %s\\n", k.c_str());\n'
       '    }\n')
assert needle in s, "eval_cb signature not found -- update dbg_nodes.sh"
open(dst, "w").write(s.replace(needle, inj, 1))
print("regenerated", dst)
PY

cd "$SRC"
g++ -Wall -O2 -g -std=c++17 -pthread $INC -c "$OUT/wp2_gen_dbg.cpp" -o "$OUT/wp2_gen_dbg.o"
g++ -O2 -g -pthread -o "$OUT/wp2_gen_dbg" "$OUT/wp2_gen_dbg.o" wp2_obj/residctl_llama.o \
    $(for f in $PAGER_C; do echo wp2_obj/${f%.c}.o; done) \
    -Wl,--export-dynamic -L"$BIN" -Wl,-rpath,"$BIN" -lllama -lggml -lggml-base -lggml-cpu -ldl -lpthread -lm

rmdir "$CG" 2>/dev/null || true
mkdir -p "$CG"
echo 1400000000 > "$CG/memory.max"
echo 0 > "$CG/memory.swap.max"
cd "$OUT"
bash -c 'echo $BASHPID > '"$CG"'/cgroup.procs; export WP2_DBG_NAMES=1; export RESIDCTL_CONFIG='"$OUT"'/phase0_armD.cfg; exec timeout 120 '"$OUT"'/wp2_gen_dbg -m '"$MODEL"' -n 3 -p abc -t 8' > "$OUT/dbg_raw.txt" 2>&1
rmdir "$CG" 2>/dev/null || true

grep '^NODE' "$OUT/dbg_raw.txt" | awk '{print $2, $3}' | sed -E 's/-[0-9]+/-<il>/' | sort -u > "$OUT/phase0_node_names.txt"
{
  echo "# distinct compute-graph node names eval_cb() sees (per-layer index -> <il>)"
  echo
  echo "## non-per-layer nodes (eval_cb's role branch tries to match these):"
  grep -vE 'cache_[kv]|<il>|\(view\)|\(permuted\)|node_[0-9]' "$OUT/phase0_node_names.txt"
  echo
  echo "## 'inp_embd' occurrences:"; grep -c 'inp_embd' "$OUT/phase0_node_names.txt" | sed 's/^/  /'
  echo "## 'embd' occurrences:"; grep -E ' embd$' "$OUT/phase0_node_names.txt" | sed 's/^/  /'
  echo
  echo "## first 42 nodes of one decode, eval-callback order (ask pass):"
  grep '^NODE ask' "$OUT/dbg_raw.txt" | head -42 | nl
} > "$OUT/phase0_node_evidence.txt"
rm -f "$OUT/wp2_gen_dbg" "$OUT/wp2_gen_dbg.o" "$OUT/dbg_raw.txt"
cat "$OUT/phase0_node_evidence.txt"
