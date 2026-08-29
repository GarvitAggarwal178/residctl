#!/bin/bash
set -eu
SRC=/root/residctl/src
LC=/root/residctl/third_party/llama.cpp
BIN=$LC/build/bin
INC="-I$LC/include -I$LC/ggml/include -I$SRC"
OUT=/root/residctl/results/livelock
PAGER_C="region.c pager.c fetch.c budget.c trace.c metrics.c cgroup_stat.c policy.c prefetch.c prefetch_pool.c fetch_trace.c policy_trace.c"
cd "$SRC"
g++ -Wall -O2 -g -std=c++17 -pthread $INC -c "$OUT/wp2_gen_dbg.cpp" -o "$OUT/wp2_gen_dbg.o"
g++ -O2 -g -pthread -o "$OUT/wp2_gen_dbg" \
    "$OUT/wp2_gen_dbg.o" wp2_obj/residctl_llama.o \
    $(for f in $PAGER_C; do echo wp2_obj/${f%.c}.o; done) \
    -Wl,--export-dynamic \
    -L"$BIN" -Wl,-rpath,"$BIN" -lllama -lggml -lggml-base -lggml-cpu \
    -ldl -lpthread -lm
echo "built $OUT/wp2_gen_dbg"
