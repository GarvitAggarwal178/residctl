#!/bin/bash
# WP2: build wp2_gen (llama.cpp + residctl pager) and wp2_opt (offline
# Belady over unequal chunk sizes).
set -eu
SRC=/root/residctl/src
LC=/root/residctl/third_party/llama.cpp
BIN=$LC/build/bin
CFLAGS="-Wall -Wextra -O2 -g -pthread -D_GNU_SOURCE"
INC="-I$LC/include -I$LC/ggml/include"

cd "$SRC"
mkdir -p wp2_obj

# residctl pager core (C) -- everything wp2 needs, minus the replay driver
PAGER_C="region.c pager.c fetch.c budget.c trace.c metrics.c cgroup_stat.c policy.c prefetch.c prefetch_pool.c fetch_trace.c policy_trace.c"
for f in $PAGER_C; do
    gcc $CFLAGS -c "$f" -o "wp2_obj/${f%.c}.o"
done
gcc $CFLAGS $INC -c residctl_llama.c -o wp2_obj/residctl_llama.o

# wp2_gen (C++), links libllama + ggml, exports residctl_llama_mmap for dlsym
g++ -Wall -Wextra -O2 -g -std=c++17 -pthread $INC -c wp2_gen.cpp -o wp2_obj/wp2_gen.o
g++ -O2 -g -pthread -o wp2_gen \
    wp2_obj/wp2_gen.o wp2_obj/residctl_llama.o \
    $(for f in $PAGER_C; do echo wp2_obj/${f%.c}.o; done) \
    -Wl,--export-dynamic \
    -L"$BIN" -Wl,-rpath,"$BIN" -lllama -lggml -lggml-base -lggml-cpu \
    -ldl -lpthread -lm

# wp2_opt: standalone offline optimum for unequal chunk sizes (does NOT
# touch belady.c). Greedy Belady/MIN: on a miss, evict the resident chunk
# whose declared next use is furthest ahead until the new one fits.
gcc $CFLAGS -o wp2_opt wp2_opt.c

echo "built: $SRC/wp2_gen  $SRC/wp2_opt"
