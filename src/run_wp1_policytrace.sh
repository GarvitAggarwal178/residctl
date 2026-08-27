#!/bin/bash
# WP1 §1.3 follow-up: capture --policy-trace for layer_order_declared at the
# non-deterministic cell 5 (8 threads / window 1 / compute 400000 / 128 MiB),
# plus the deterministic cell 1 (fully serial) as a baseline, and find the
# first victim-sequence divergence -- exactly as Campaign 13 Phase A did.
set -u
RESIDCTL=/root/residctl
SRC=$RESIDCTL/src
SCRATCH=$RESIDCTL/scratch
MODEL=$SCRATCH/pattern_2g.bin
CGROUP=/sys/fs/cgroup/residctl_wp1pt
LOG=$RESIDCTL/results/overnight/wp1_policytrace_log.txt
REGION_LEN=2147483648
CHUNK=134217728
BUDGET=1073741824
MARGIN=67108864
> "$LOG"
log() { echo "$@" | tee -a "$LOG"; }

fresh() {
    if [ -d "$CGROUP" ]; then
        for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
        sleep 1; rmdir "$CGROUP" 2>/dev/null
    fi
    mkdir "$CGROUP"; echo "$((BUDGET+MARGIN))" > "$CGROUP/memory.max"; echo 0 > "$CGROUP/memory.swap.max"
}
runit() { # name threads window compute ptpath rtpath
    fresh
    bash -c 'echo $BASHPID > "'"$CGROUP"'/cgroup.procs"; exec timeout 180 "$@"' -- \
        "$SRC/replay_main" "$CGROUP" "$MODEL" "$REGION_LEN" "$CHUNK" "$BUDGET" 5 \
        layer_order_declared off "" "$6" --fetch-workers 4 --driver-threads "$2" \
        --lookahead-window "$3" --compute-ns-per-mib "$4" --policy-trace "$5" 2>&1 | \
        grep -E "absent_handled=|ARM_CSV" | tee -a "$LOG"
    for p in $(cat "$CGROUP/cgroup.procs" 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
}

log "=== cell 1 (serial, deterministic) ==="
runit cell1 1 0 0 "$SCRATCH/wp1_pt_cell1.bin" "$SCRATCH/wp1_rt_cell1.bin"
log "=== cell 5 (8t/w1/c400000) rep A ==="
runit cell5a 8 1 400000 "$SCRATCH/wp1_pt_cell5a.bin" "$SCRATCH/wp1_rt_cell5a.bin"
log "=== cell 5 rep B ==="
runit cell5b 8 1 400000 "$SCRATCH/wp1_pt_cell5b.bin" "$SCRATCH/wp1_rt_cell5b.bin"
log "=== cell 5 rep C ==="
runit cell5c 8 1 400000 "$SCRATCH/wp1_pt_cell5c.bin" ""

rmdir "$CGROUP" 2>/dev/null

python3 - <<'PY' | tee -a "$LOG"
import struct
PT_FMT = '<QIIqIII'; PT_SIZE = struct.calcsize(PT_FMT)
def load(p):
    d = open(p,'rb').read(); out=[]
    for i in range(len(d)//PT_SIZE):
        s,v,c,dist,nr,lo,hi = struct.unpack(PT_FMT, d[i*PT_SIZE:(i+1)*PT_SIZE])
        out.append(dict(seq=s,victim=v,cursor=c,dist=dist,n_resident=nr,lo=lo,hi=hi))
    return out
S='/root/residctl/scratch/'
c1=load(S+'wp1_pt_cell1.bin'); a=load(S+'wp1_pt_cell5a.bin'); b=load(S+'wp1_pt_cell5b.bin'); c=load(S+'wp1_pt_cell5c.bin')
print(f"cell1 (serial):   {len(c1)} evictions, victims={[r['victim'] for r in c1]}")
print(f"cell5 repA:       {len(a)} evictions, victims={[r['victim'] for r in a]}")
print(f"cell5 repB:       {len(b)} evictions, victims={[r['victim'] for r in b]}")
print(f"cell5 repC:       {len(c)} evictions, victims={[r['victim'] for r in c]}")
def firstdiv(x,y,nx,ny):
    for i in range(min(len(x),len(y))):
        if x[i]['victim']!=y[i]['victim']:
            print(f"\n{nx} vs {ny}: first victim divergence at eviction index {i} (seq={i+1})")
            print(f"  {nx}: victim={x[i]['victim']} cursor={x[i]['cursor']} dist={x[i]['dist']} n_resident={x[i]['n_resident']} bitmap_lo={x[i]['lo']}")
            print(f"  {ny}: victim={y[i]['victim']} cursor={y[i]['cursor']} dist={y[i]['dist']} n_resident={y[i]['n_resident']} bitmap_lo={y[i]['lo']}")
            print(f"  same resident set? {'YES' if x[i]['lo']==y[i]['lo'] and x[i]['hi']==y[i]['hi'] and x[i]['n_resident']==y[i]['n_resident'] else 'NO'}")
            return
    print(f"\n{nx} vs {ny}: no victim divergence in first {min(len(x),len(y))} evictions")
firstdiv(a,b,'repA','repB')
firstdiv(a,c,'repA','repC')
firstdiv(b,c,'repB','repC')
PY
log "=== done ==="
