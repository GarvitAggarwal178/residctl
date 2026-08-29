#!/bin/bash
# Negative test for the Phase 0b consumption-signal audit: temporarily break
# the embd node-name match, confirm wp2_gen aborts with the FATAL message
# naming chunk 2 (token_embd), then restore + rebuild.
set -u
SRC=/root/residctl/src
CG=/sys/fs/cgroup/residctl_livelock_p0   # must match cgroup= in phase0_armD.cfg
MODEL=/root/residctl/models/model.gguf
CFG=/root/residctl/results/livelock/phase0_armD.cfg

cd "$SRC"
cp wp2_gen.cpp /tmp/wp2_gen_keep.cpp
sed -i 's/!strcmp(nm, "inp_embd") || !strcmp(nm, "embd")/!strcmp(nm, "inp_embd")/' wp2_gen.cpp
echo "-- match line after break:"
grep -n 'inp_embd") ' wp2_gen.cpp
bash build_wp2.sh > /tmp/negtest_build.log 2>&1 && echo "-- built broken variant"

rmdir "$CG" 2>/dev/null || true
mkdir -p "$CG"
echo 1400000000 > "$CG/memory.max"
echo 0 > "$CG/memory.swap.max"
bash -c 'echo $BASHPID > '"$CG"'/cgroup.procs; export RESIDCTL_CONFIG='"$CFG"'; exec timeout 90 '"$SRC"'/wp2_gen -m '"$MODEL"' -n 8 -p abc -t 8' > /tmp/negtest_run.log 2>&1
echo "-- wp2_gen exit: $?"
rmdir "$CG" 2>/dev/null || true
echo "-- relevant output:"
grep -E 'FATAL|audit|[Aa]bort|WP2_TOKENS|SIGABRT' /tmp/negtest_run.log | head

cp /tmp/wp2_gen_keep.cpp wp2_gen.cpp
bash build_wp2.sh > /tmp/negtest_restore.log 2>&1 && echo "-- restored + rebuilt"
grep -n 'inp_embd") ||' wp2_gen.cpp
