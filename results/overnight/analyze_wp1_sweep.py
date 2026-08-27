#!/usr/bin/env python3
# WP1 §1.4 analysis: learned vs declared. Per cell: read_bytes, total fetches
# (demand + prefetch), demand faults, wall-clock, D/OPT, and the
# declared-minus-learned delta on each. Plus pre-registered expectations 1-5.
import csv, statistics
from collections import defaultdict

R = '/root/residctl/results/overnight'
rows = list(csv.DictReader(open(f'{R}/wp1_sweep.csv')))
opt = {}
for o in csv.DictReader(open(f'{R}/wp1_sweep_opt.csv')):
    opt[(o['chunk_size'], o['ratio'])] = int(o['opt_bytes'])

by = defaultdict(list)
for r in rows:
    if r['rc'] != '0':
        continue
    by[(r['chunk_size'], r['ratio'], r['arm'], r['policy'], r['compute'])].append(r)

def med(cell, f, cast=int):
    v = [cast(x[f]) for x in by.get(cell, []) if x[f] not in ('', 'n/a')]
    return statistics.median(v) if v else None

def cellname(cs, r, comp):
    return f"{int(cs)//1048576}MiB/r={r}/c={comp}"

CS = ['8388608', '134217728']
RA = ['0.25', '0.5', '0.75']
CO = ['0', '400000']

print("## Main table (median of n=3). read_bytes = pager_bytes_fetched.")
print()
hdr = ("cell", "arm", "policy", "read_bytes", "tot_fetch", "demand", "wall_s", "D/OPT")
print("| " + " | ".join(hdr) + " |")
print("|" + "|".join(["---"]*len(hdr)) + "|")
records = {}
for cs in CS:
    for ra in RA:
        for comp in CO:
            ob = opt.get((cs, ra))
            for arm, pol in [('C','lru'),('D','layer_order_learned'),('D','layer_order_declared'),
                             ('E','layer_order_learned'),('E','layer_order_declared')]:
                cell = (cs, ra, arm, pol, comp)
                rb = med(cell, 'pager_bytes_fetched')
                if rb is None:
                    continue
                dem = med(cell, 'absent_handled')
                pf = med(cell, 'prefetches') or 0
                tot = dem + pf
                wall = med(cell, 'wall_ns')
                dopt = rb/ob if ob else None
                records[cell] = dict(read_bytes=rb, tot=tot, demand=dem, wall=wall, dopt=dopt,
                                     infeasible=med(cell,'infeasible'), pin_broken=med(cell,'pin_broken'))
                print(f"| {cellname(cs,ra,comp)} | {arm} | {pol.replace('layer_order_','')} | "
                      f"{rb:,} | {tot} | {dem} | {wall/1e9:.3f} | {dopt:.3f} |" if dopt else
                      f"| {cellname(cs,ra,comp)} | {arm} | {pol.replace('layer_order_','')} | {rb:,} | {tot} | {dem} | {wall/1e9:.3f} | n/a |")

print()
print("## Declared minus learned (negative = declared reads/fetches fewer)")
print()
print("| cell | arm | Δ read_bytes | Δ tot_fetch | Δ demand | Δ wall_s | Δ (D/OPT) |")
print("|---|---|---|---|---|---|---|")
exp2_fail = []
exp4 = []
margin = {}
for cs in CS:
    for ra in RA:
        for comp in CO:
            for arm in ['D', 'E']:
                lc = (cs, ra, arm, 'layer_order_learned', comp)
                dc = (cs, ra, arm, 'layer_order_declared', comp)
                if lc not in records or dc not in records:
                    continue
                L, D = records[lc], records[dc]
                drb = D['read_bytes'] - L['read_bytes']
                print(f"| {cellname(cs,ra,comp)} | {arm} | {drb:+,} | {D['tot']-L['tot']:+} | "
                      f"{D['demand']-L['demand']:+} | {(D['wall']-L['wall'])/1e9:+.3f} | "
                      f"{(D['dopt']-L['dopt']):+.3f} |" if D['dopt'] and L['dopt'] else
                      f"| {cellname(cs,ra,comp)} | {arm} | {drb:+,} | ... |")
                if arm == 'D':
                    margin[(cs,ra,comp)] = drb
                    if drb > 0:
                        exp2_fail.append((cellname(cs,ra,comp), drb))
                    if D['dopt'] and L['dopt']:
                        exp4.append((cellname(cs,ra,comp), L['dopt'], D['dopt']))

print()
print("## Pre-registered expectations")
print()
print(f"1. declared deterministic at every 1.3 cell -> see wp1_determinism.csv")
print(f"2. declared reads FEWER bytes than learned at every arm-D cell:")
if not exp2_fail:
    print(f"   HELD -- declared <= learned on read_bytes in all {len(margin)} arm-D cells")
else:
    print(f"   DID NOT HOLD at {len(exp2_fail)} cell(s): " + "; ".join(f"{c} (+{d:,} bytes)" for c,d in exp2_fail))
print(f"3. margin larger at 128 MiB than 8 MiB:")
m8  = [v for (cs,ra,co),v in margin.items() if cs=='8388608']
m128= [v for (cs,ra,co),v in margin.items() if cs=='134217728']
print(f"   mean Δ read_bytes (declared-learned) arm D: 8MiB={statistics.mean(m8):,.0f}  128MiB={statistics.mean(m128):,.0f}")
print(f"   (more negative at 128 MiB => held)")
print(f"4. D/OPT improves under declared at every arm-D cell:")
bad4 = [(c,l,d) for c,l,d in exp4 if d > l + 1e-9]
if not bad4:
    print(f"   HELD in all {len(exp4)} cells (declared D/OPT <= learned D/OPT)")
else:
    print(f"   DID NOT HOLD at: " + "; ".join(f"{c} ({l:.3f}->{d:.3f})" for c,l,d in bad4))
print(f"5. the five C13-A non-det cells: only r=0.25/0.375 c=400000 at both chunk sizes + 128/r=0.5/c=400000")
print(f"   this sweep covers r in {{0.25,0.5,0.75}} -- check arm C approach for declared at c=400000 cells:")
for cs in CS:
    for ra in RA:
        cc = (cs, ra, 'C', 'lru', '400000')
        dd = (cs, ra, 'D', 'layer_order_declared', '400000')
        dl = (cs, ra, 'D', 'layer_order_learned', '400000')
        if cc in records and dd in records:
            crb = records[cc]['read_bytes']
            print(f"   {cellname(cs,ra,'400000')}: C={crb:,}  D_declared={records[dd]['read_bytes']:,} "
                  f"({records[dd]['read_bytes']/crb:.3f}xC)  D_learned={records[dl]['read_bytes']:,} "
                  f"({records[dl]['read_bytes']/crb:.3f}xC)" if dl in records else "")
