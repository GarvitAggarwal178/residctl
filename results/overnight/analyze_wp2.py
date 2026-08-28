#!/usr/bin/env python3
# WP2 analysis: real-model sweep + OPT + synthetic-vs-real comparison.
import csv, statistics, subprocess, os, re

R = '/root/residctl/results/overnight'
INV = f'{R}/wp2_tensor_inventory.txt'
SWEEP = f'{R}/wp2_sweep.csv'
OPT_BIN = '/root/residctl/src/wp2_opt'
MiB = 1048576.0

# ---- parse inventory: chunk bytes + declared sequence + region/file size
chunk_bytes, declared_seq = [], []
region_len = file_size = n_layers = 0
sec = None
for line in open(INV):
    line = line.rstrip('\n')
    m = re.match(r'file_size=(\d+) region_len=(\d+).*n_layers=(\d+)', line)
    if m: file_size, region_len, n_layers = int(m[1]), int(m[2]), int(m[3])
    if line.startswith('## chunk_bytes'): sec = 'cb'; continue
    if line.startswith('## declared sequence'): sec = 'ds'; continue
    if line.startswith('## '): sec = None; continue
    if sec == 'cb' and line.strip().isdigit(): chunk_bytes.append(int(line))
    if sec == 'ds' and line.strip(): declared_seq = [int(x) for x in line.split()]

n_chunks = len(chunk_bytes)
sum_declared = sum(chunk_bytes[c] for c in set(declared_seq))
print(f"region_len={region_len:,} file_size={file_size:,} n_chunks={n_chunks} n_layers={n_layers}")
print(f"chunk sizes: min={min(chunk_bytes)/MiB:.2f} MiB  median={statistics.median(chunk_bytes)/MiB:.2f} MiB  "
      f"max={max(chunk_bytes)/MiB:.2f} MiB  sum={sum(chunk_bytes)/MiB:.1f} MiB  ({sum(chunk_bytes)/file_size:.4f} x file)")
print(f"declared sequence: {len(declared_seq)} entries, {len(set(declared_seq))} distinct, sum_distinct={sum_declared/MiB:.1f} MiB")
print()

# write chunk_bytes + declared seq to files for wp2_opt
cbf, dsf = '/tmp/wp2_chunk_bytes.txt', '/tmp/wp2_declared_seq.txt'
open(cbf, 'w').write('\n'.join(str(x) for x in chunk_bytes) + '\n')
open(dsf, 'w').write('\n'.join(str(x) for x in declared_seq) + '\n')

rows = [r for r in csv.DictReader(open(SWEEP)) if r['rc'] == '0']
by = {}
for r in rows:
    by.setdefault((r['ratio'], r['arm']), []).append(r)

def med(cell, f, cast=float):
    v = [cast(x[f]) for x in by.get(cell, []) if x[f] not in ('', 'n/a', None)]
    return statistics.median(v) if v else None

RATIOS = sorted({r['ratio'] for r in rows}, key=float)
# passes for OPT: median layer_transitions / n_layers + 1 (embd/out per token too)
lt = med((RATIOS[0], 'D'), 'layer_transitions') or med((RATIOS[0], 'C'), 'layer_transitions')
passes = int(round((lt or (64*n_layers)) / n_layers)) + 1
print(f"OPT passes (layer scans incl. prompt) = {passes}")
print()

opt = {}
for ratio in RATIOS:
    budget = int(float(ratio) * region_len)
    out = subprocess.run([OPT_BIN, cbf, str(budget), str(passes), dsf],
                         capture_output=True, text=True)
    line = [l for l in (out.stdout + out.stderr).splitlines() if l.startswith('WP2_OPT')]
    print(f"r={ratio} budget={budget:,}  {line[0] if line else out.stderr.strip()}")
    if line:
        d = dict(kv.split('=') for kv in line[0].split(',')[1:])
        opt[ratio] = int(d['opt_missed_bytes'])
print()

print("## Per-cell (median of n=3). read_bytes: arm A = io_gen_bytes; C/D/E = pager_bytes_fetched.")
print("| ratio | arm | read_bytes (GB) | /OPT | demand faults | evictions | tokens/s | ttft (ms) | p99 inter (ms) | wall (s) | mem_peak (MiB) | infeasible | pin_broken |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
tbl = {}
for ratio in RATIOS:
    ob = opt.get(ratio)
    for arm in ('A', 'C', 'D', 'E'):
        cell = (ratio, arm)
        if cell not in by: continue
        rb = med(cell, 'io_gen_bytes') if arm == 'A' else med(cell, 'pager_bytes_fetched')
        df = med(cell, 'absent_handled')
        ev = med(cell, 'evictions')
        ts = med(cell, 'tokens_s'); tt = med(cell, 'ttft_ms'); p99 = med(cell, 'p99_inter_ms')
        ws = med(cell, 'wall_s'); mp = med(cell, 'memory_peak'); inf = med(cell, 'infeasible'); pb = med(cell, 'pin_broken')
        dopt = rb / ob if (rb and ob) else None
        tbl[cell] = dict(rb=rb, dopt=dopt, ts=ts, ws=ws)
        print(f"| {ratio} | {arm} | {rb/1e9:.1f} | {dopt:.2f} | {df:.0f} | {ev:.0f} | {ts:.2f} | {tt:.0f} | {p99:.0f} | {ws:.1f} | "
              f"{(mp or 0)/MiB:.0f} | {inf if inf is not None else '-'} | {pb if pb is not None else '-'} |"
              if dopt else f"| {ratio} | {arm} | {rb/1e9 if rb else 0:.1f} | - | {df or 0:.0f} | {ev or 0:.0f} | {ts:.2f} | {tt:.0f} | {p99:.0f} | {ws:.1f} | {(mp or 0)/MiB:.0f} | - | - |")

print()
print("## Pre-registered expectations")
c_miss = []
for ratio in RATIOS:
    dfC = med((ratio,'C'),'absent_handled'); refs = (med((ratio,'C'),'layer_transitions') or 0) + 2*passes
    print(f"1. arm C miss rate ~100%: r={ratio} demand_faults={dfC:.0f} over ~{int(refs)} references -> {dfC/refs if refs else 0:.3f}")
for ratio in RATIOS:
    A, D = tbl.get((ratio,'A')), tbl.get((ratio,'D'))
    if A and D and A['rb'] and D['rb']:
        print(f"2. D<A bytes @ r={ratio}: D={D['rb']/1e9:.1f}GB vs A={A['rb']/1e9:.1f}GB -> {'HELD' if D['rb']<A['rb'] else 'DID NOT HOLD'}")
for ratio in RATIOS:
    D, E = tbl.get((ratio,'D')), tbl.get((ratio,'E'))
    if D and E and D['rb'] and E['rb']:
        print(f"3. E<D bytes @ r={ratio}: E={E['rb']/1e9:.1f}GB vs D={D['rb']/1e9:.1f}GB -> {'HELD' if E['rb']<D['rb'] else 'DID NOT HOLD'}")
for ratio in RATIOS:
    D = tbl.get((ratio,'D'))
    if D and D['dopt']: print(f"4. OPT<=D @ r={ratio}: D/OPT={D['dopt']:.2f} -> {'HELD' if D['dopt']>=1.0 else 'DID NOT HOLD (D below OPT!)'}")
print("5. tokens/s vs budget (graceful?):")
for arm in ('A','C','D','E'):
    ts = [f"r={r}:{tbl[(r,arm)]['ts']:.2f}" for r in RATIOS if (r,arm) in tbl]
    print(f"   {arm}: {'  '.join(ts)}  (baseline unconstrained ~13.2 t/s)")
