#!/usr/bin/env python3
"""WP3 -- figures + tables from data already on disk. No new sweeps."""
import csv, statistics, os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

R = '/root/residctl/results'
OUT = f'{R}/overnight/figures'
os.makedirs(OUT, exist_ok=True)
MiB = 1048576.0
OD_CEIL = 3396.0  # spike O_DIRECT max MiB/s -- host-cache contamination signature

def rows(path):
    with open(path) as f:
        return list(csv.DictReader(f))

def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None

def i(x):
    return None if x in (None, '', 'n/a') else int(x)

# ---------------------------------------------------------------- sources
phaseD = rows(f'{R}/campaign12_phaseD_paper_table.csv')
wp1    = rows(f'{R}/overnight/wp1_sweep.csv')
wp1opt = rows(f'{R}/overnight/wp1_sweep_opt.csv')
phase3 = rows(f'{R}/phase3_chunk_size.csv')
phaseB = rows(f'{R}/campaign12_phaseB_chunk_floor.csv')

OPT_BYTES = {(o['chunk_size'], o['ratio']): int(o['opt_bytes']) for o in wp1opt}
# ratios not in the WP1 grid: from Campaign 12 Phase D's OPT table
OPT_BYTES.setdefault(('8388608','0.375'), 7516192768)
OPT_BYTES.setdefault(('8388608','0.625'), 5368709120)
OPT_BYTES.setdefault(('134217728','0.375'), 7516192768)
OPT_BYTES.setdefault(('134217728','0.625'), 5368709120)
OPT_MISS = {k: v // (8388608 if k[0]=='8388608' else 134217728) for k,v in OPT_BYTES.items()}

CS8, CS128 = '8388608', '134217728'
RA5 = ['0.25','0.375','0.5','0.625','0.75']
RA3 = ['0.25','0.5','0.75']

NONDET = {('8388608','0.25','400000'), ('8388608','0.375','400000'),
          ('134217728','0.25','400000'), ('134217728','0.375','400000'),
          ('134217728','0.5','400000')}

# ---- Phase D helpers ----
def pd_cell(cs, ratio, arm, compute):
    return [r for r in phaseD if r['chunk_size']==cs and r['ratio']==ratio
            and r['arm']==arm and (arm in ('A','B') or r['compute']==compute) and r['rc']=='0']

def pd_armA_best(cs, ratio):
    """best madvise mode by median touches/sec, return (median read_bytes, median wall_ns, contaminated?)"""
    best = None
    for mode in ('normal','sequential','random'):
        rs = [r for r in phaseD if r['chunk_size']==cs and r['ratio']==ratio and r['arm']=='A'
              and r['detail']==mode and r['rc']=='0']
        if not rs: continue
        walls = [int(r['wall_ns']) for r in rs]
        tps = int(rs[0]['touches']) / (med(walls)/1e9)
        if best is None or tps > best[0]:
            rb = med([int(r['pager_bytes_fetched']) if r['pager_bytes_fetched'] not in ('','n/a')
                      else int(r['io_read_bytes_delta']) for r in rs])
            bt = int(rs[0]['bytes_touched'])
            mibs = bt / (med(walls)/1e9) / MiB
            best = (tps, rb, med(walls), mibs, mode)
    return best  # (tps, read_bytes, wall_ns, mib_s, mode)

def pd_median(cs, ratio, arm, compute, field='pager_bytes_fetched'):
    rs = pd_cell(cs, ratio, arm, compute)
    vals = [i(r[field]) for r in rs]
    return med(vals)

# ---- WP1 helpers ----
def wp1_cell(cs, ratio, arm, policy, compute):
    return [r for r in wp1 if r['chunk_size']==cs and r['ratio']==ratio and r['arm']==arm
            and r['policy']==policy and r['compute']==compute and r['rc']=='0']

def wp1_median(cs, ratio, arm, policy, compute, field='pager_bytes_fetched'):
    return med([i(r[field]) for r in wp1_cell(cs, ratio, arm, policy, compute)])

TOUCHES = {CS8: 1280, CS128: 80}

# ================================================================ FIGURE 1
def figure1():
    comp = '0'
    fig, axes = plt.subplots(1, 2, figsize=(12, 5), sharey=False)
    csv_rows = [('chunk_size','ratio','arm','policy','read_bytes','bytes_per_touch_MiB','MiB_s','contaminated','source')]
    for ax, cs, label in [(axes[0], CS8, '8 MiB chunks'), (axes[1], CS128, '128 MiB chunks')]:
        tp = TOUCHES[cs]
        # arm A
        xs, ys, cont = [], [], []
        for ra in RA5:
            b = pd_armA_best(cs, ra)
            if not b: continue
            _, rb, wall, mibs, mode = b
            xs.append(float(ra)); ys.append(rb/tp/MiB); cont.append(mibs > OD_CEIL)
            csv_rows.append((cs,ra,'A',f'madvise:{mode}',rb,f'{rb/tp/MiB:.2f}',f'{mibs:.1f}',
                             'YES' if mibs>OD_CEIL else 'no','campaign12_phaseD'))
        ax.plot(xs, ys, '-o', color='#7a7a7a', label='A (mmap baseline)')
        for x,y,c in zip(xs,ys,cont):
            if c: ax.plot([x],[y], marker='X', ms=13, color='#d62728', mec='k', mew=0.5, zorder=5)
        # arm C
        xs, ys = [], []
        for ra in RA5:
            rb = pd_median(cs, ra, 'C', comp)
            if rb is None: continue
            xs.append(float(ra)); ys.append(rb/tp/MiB)
            csv_rows.append((cs,ra,'C','lru',rb,f'{rb/tp/MiB:.2f}','','no','campaign12_phaseD'))
        ax.plot(xs, ys, '-s', color='#1f77b4', label='C (kernel LRU)')
        # arm D declared (WP1)
        xs, ys = [], []
        for ra in RA3:
            rb = wp1_median(cs, ra, 'D', 'layer_order_declared', comp)
            if rb is None: continue
            xs.append(float(ra)); ys.append(rb/tp/MiB)
            csv_rows.append((cs,ra,'D','layer_order_declared',rb,f'{rb/tp/MiB:.2f}','','no','wp1_sweep'))
        ax.plot(xs, ys, '-^', color='#2ca02c', label='D (declared order)')
        # arm E declared (WP1)
        xs, ys = [], []
        for ra in RA3:
            rb = wp1_median(cs, ra, 'E', 'layer_order_declared', comp)
            if rb is None: continue
            xs.append(float(ra)); ys.append(rb/tp/MiB)
            csv_rows.append((cs,ra,'E','layer_order_declared',rb,f'{rb/tp/MiB:.2f}','','no','wp1_sweep'))
        ax.plot(xs, ys, '-D', color='#ff7f0e', label='E (declared + prefetch)')
        # OPT
        xs, ys = [], []
        for ra in RA5:
            ob = OPT_BYTES.get((cs, ra))
            if ob is None: continue
            xs.append(float(ra)); ys.append(ob/tp/MiB)
            csv_rows.append((cs,ra,'OPT','belady',ob,f'{ob/tp/MiB:.2f}','','no','belady_main'))
        ax.plot(xs, ys, '--', color='k', label='OPT (Belady)')
        ax.set_title(label); ax.set_xlabel('budget ratio (fraction of weight region)')
        ax.set_ylabel('bytes read per touch (MiB)'); ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
    fig.suptitle('Figure 1 — Bytes read per unit of work, by budget ratio (compute=0)', fontsize=12)
    fig.text(0.5, 0.055, 'D/E: WP1 declared-order policy WITH the WP0 fix (r=0.25/0.5/0.75); at 8 MiB D reads '
             'exactly OPT, at 128 MiB D/OPT=1.09-1.15. A/C: Campaign 12 Phase D (Phase A repaired baseline). '
             'Arm A line is hidden behind C.', ha='center', fontsize=7)
    fig.text(0.5, 0.02, f'X = arm A point whose achieved bandwidth exceeds the spike {OD_CEIL:.0f} MiB/s '
             'O_DIRECT ceiling -- Windows VHDX host-cache contamination.', ha='center', fontsize=7)
    fig.tight_layout(rect=[0,0.10,1,0.95])
    fig.savefig(f'{OUT}/figure1_bytes_per_work.png', dpi=200)
    plt.close(fig)
    with open(f'{OUT}/figure1_bytes_per_work.csv','w',newline='') as f:
        csv.writer(f).writerows(csv_rows)
    print("figure1 done")

# ================================================================ FIGURE 2
def figure2():
    comp = '0'
    fig, axes = plt.subplots(1, 2, figsize=(12, 5))
    csv_rows = [('chunk_size','ratio','arm','policy','demand_faults','total_refs','miss_rate','source')]
    for ax, cs, label in [(axes[0], CS8, '8 MiB chunks'), (axes[1], CS128, '128 MiB chunks')]:
        tp = TOUCHES[cs] * 5  # 5 passes; touches column is passes*n_chunks already = references
        tp = TOUCHES[cs]      # 'touches' column IS total references (n_passes*n_chunks)
        for arm, pol, style, col, name in [
            ('C','lru','-s','#1f77b4','C (kernel LRU)'),
            ('D','layer_order_declared','-^','#2ca02c','D (declared)'),
            ('E','layer_order_declared','-D','#ff7f0e','E (declared + prefetch)')]:
            xs, ys = [], []
            for ra in (RA5 if arm=='C' else RA3):
                if arm=='C':
                    df = pd_median(cs, ra, 'C', comp, 'absent_handled')
                    refs = int(pd_cell(cs,ra,'C',comp)[0]['touches'])
                else:
                    df = wp1_median(cs, ra, arm, pol, comp, 'absent_handled')
                    cc = wp1_cell(cs,ra,arm,pol,comp)
                    refs = int(cc[0]['touches']) if cc else tp
                if df is None: continue
                xs.append(float(ra)); ys.append(df/refs)
                csv_rows.append((cs,ra,arm,pol,df,refs,f'{df/refs:.4f}','wp1_sweep' if arm!='C' else 'campaign12_phaseD'))
            ax.plot(xs, ys, style, color=col, label=name)
        # OPT
        xs, ys = [], []
        for ra in RA5:
            om = OPT_MISS.get((cs,ra));
            if om is None: continue
            xs.append(float(ra)); ys.append(om/TOUCHES[cs])
            csv_rows.append((cs,ra,'OPT','belady',om,TOUCHES[cs],f'{om/TOUCHES[cs]:.4f}','belady_main'))
        ax.plot(xs, ys, '--', color='k', label='OPT (Belady)')
        ax.set_title(label); ax.set_xlabel('budget ratio'); ax.set_ylabel('demand faults / total references')
        ax.set_ylim(0, 1.05); ax.grid(alpha=0.3); ax.legend(fontsize=8)
    fig.suptitle('Figure 2 — Miss rate against the optimal bound (compute=0)', fontsize=12)
    fig.text(0.5, 0.02, 'C sits flat at 1.000 (every reference misses -- full-pass thrashing). D (with the WP0 fix) '
             'tracks OPT exactly at 8 MiB and sits ~10-15% above it at 128 MiB (protecting the 2 live chunks '
             'costs more when only ~8 fit). E is below the OPT line because its DEMAND-fault rate excludes '
             'prefetches; E total fetches (demand+prefetch) still >= OPT.', ha='center', fontsize=7)
    fig.tight_layout(rect=[0,0.07,1,0.95])
    fig.savefig(f'{OUT}/figure2_miss_rate.png', dpi=200)
    plt.close(fig)
    with open(f'{OUT}/figure2_miss_rate.csv','w',newline='') as f:
        csv.writer(f).writerows(csv_rows)
    print("figure2 done")

# ================================================================ FIGURE 3
def figure3():
    # chunk-size sweep, arm D, one line per ratio. Phase B {4,8,16,32} + Phase 3 {32,64,128,256}
    def dmed(src, cs, ratio, field):
        rs = [r for r in src if r['chunk_size']==str(cs) and r['ratio']==ratio and r['arm']=='D'
              and r.get('rc','0') in ('0',) and r.get('exit_code','0') in ('0','')]
        # phase3 uses exit_code, phaseB uses rc
        rs = [r for r in src if r['chunk_size']==str(cs) and r['ratio']==ratio and r['arm']=='D']
        rs = [r for r in rs if r.get('rc', r.get('exit_code','0')) in ('0',)]
        return med([i(r[field]) for r in rs])
    sizes_b = [4194304, 8388608, 16777216, 33554432]
    sizes_3 = [33554432, 67108864, 134217728, 268435456]
    fig, ax1 = plt.subplots(figsize=(10, 6))
    ax2 = ax1.twinx()
    csv_rows = [('chunk_size_MiB','ratio','read_bytes','wall_ns','source')]
    overlap = {}
    for ratio, col in zip(RA3, ['#1f77b4','#2ca02c','#d62728']):
        xs, yb, yw = [], [], []
        for cs in sizes_b:
            rb = dmed(phaseB, cs, ratio, 'pager_bytes_fetched'); wl = dmed(phaseB, cs, ratio, 'wall_ns')
            if rb is None: continue
            xs.append(cs/MiB); yb.append(rb/1e9); yw.append(wl/1e9)
            csv_rows.append((cs//1048576, ratio, rb, wl, 'campaign12_phaseB'))
            if cs == 33554432: overlap.setdefault(ratio, {})['B'] = (rb, wl)
        for cs in sizes_3:
            if cs == 33554432:
                rb = dmed(phase3, cs, ratio, 'pager_bytes_fetched'); wl = dmed(phase3, cs, ratio, 'wall_ns')
                if rb is not None: overlap.setdefault(ratio, {})['P3'] = (rb, wl)
                continue
            rb = dmed(phase3, cs, ratio, 'pager_bytes_fetched'); wl = dmed(phase3, cs, ratio, 'wall_ns')
            if rb is None: continue
            xs.append(cs/MiB); yb.append(rb/1e9); yw.append(wl/1e9)
            csv_rows.append((cs//1048576, ratio, rb, wl, 'campaign11_phase3'))
        order = sorted(range(len(xs)), key=lambda k: xs[k])
        xs=[xs[k] for k in order]; yb=[yb[k] for k in order]; yw=[yw[k] for k in order]
        ax1.plot(xs, yb, '-o', color=col, label=f'read_bytes  r={ratio}')
        ax2.plot(xs, yw, '--s', color=col, alpha=0.55, label=f'wall-clock  r={ratio}')
    ax1.set_xscale('log', base=2)
    ax1.set_xticks([4,8,16,32,64,128,256]); ax1.set_xticklabels([4,8,16,32,64,128,256])
    ax1.set_xlabel('chunk size (MiB, log scale)')
    ax1.set_ylabel('read_bytes (GB)  [solid]'); ax2.set_ylabel('wall-clock (s)  [dashed]')
    ax1.grid(alpha=0.3)
    ax1.legend(loc='upper left', fontsize=8); ax2.legend(loc='upper right', fontsize=8)
    note = "; ".join(f"r={ra}: 32MiB B={o['B'][0]/1e9:.2f}GB/{o['B'][1]/1e9:.2f}s vs "
                     f"P3={o['P3'][0]/1e9:.2f}GB/{o['P3'][1]/1e9:.2f}s"
                     for ra,o in overlap.items() if 'B' in o and 'P3' in o)
    fig.suptitle('Figure 3 — Chunk size trade-off (arm D, layer_order_learned, compute=0)', fontsize=12)
    fig.text(0.5, 0.055, 'Solid = read_bytes (left axis); dashed = wall-clock (right axis); colour = ratio. '
             'Bytes minimise near 8-16 MiB then climb; wall-clock has a valley near 16-32 MiB and is worst at 4 MiB.',
             ha='center', fontsize=7)
    fig.text(0.5, 0.02, 'Campaign 12 Phase B {4,8,16,32} MiB + Campaign 11 Phase 3 {32,64,128,256} MiB, different '
             'sweeps/scripts; 32 MiB overlap did NOT agree exactly (Phase B 3-7% more bytes, ~10% slower). '
             'Numbers: figure3_chunk_size_tradeoff.csv.', ha='center', fontsize=6.8)
    fig.tight_layout(rect=[0,0.10,1,0.95])
    fig.savefig(f'{OUT}/figure3_chunk_size_tradeoff.png', dpi=200)
    plt.close(fig)
    with open(f'{OUT}/figure3_chunk_size_tradeoff.csv','w',newline='') as f:
        csv.writer(f).writerows(csv_rows)
    print("figure3 done; overlap:", overlap)

# ================================================================ FIGURE 4
def figure4():
    conds = ['memory.swap.max = 0\n(S3d)', 'swap available\n(S3e, mean of 3)']
    pgscan = [0, round(statistics.mean([120668,120850,120543]))]
    pgsteal = [0, round(statistics.mean([60109,60205,60108]))]
    shmem_recl = [0.0, round(statistics.mean([248049664,248401920,247537664])/MiB, 1)]
    high_ev = ['37', '512-515']
    fig, axes = plt.subplots(1, 3, figsize=(13, 4.5))
    for ax, vals, title in [(axes[0], pgscan, 'pgscan (pages)'),
                            (axes[1], pgsteal, 'pgsteal (pages)'),
                            (axes[2], shmem_recl, 'shmem reclaimed (MiB)')]:
        bars = ax.bar(conds, vals, color=['#1f77b4','#d62728'])
        ax.set_title(title); ax.grid(alpha=0.3, axis='y')
        for b, v in zip(bars, vals):
            ax.text(b.get_x()+b.get_width()/2, b.get_height(), f'{v:,}', ha='center', va='bottom', fontsize=10)
    for ax in axes:
        ax.text(0, ax.get_ylim()[1]*0.5, 'memory.events[high]=37\n(kernel tried, found\nnothing eligible)',
                ha='center', va='center', fontsize=8, color='#1f77b4')
        ax.text(1, ax.get_ylim()[1]*0.5, 'high=512-515', ha='center', va='center', fontsize=8, color='white')
    fig.suptitle('Figure 4 — The reclaim-authority result (spike S3d vs S3e)', fontsize=12)
    fig.text(0.5, 0.01, f'Under memory.swap.max=0 the kernel enters reclaim (memory.events[high]={high_ev[0]}) '
             f'and finds nothing eligible: pgscan=pgsteal=0, shmem unchanged. With swap available it reclaims '
             f'~{shmem_recl[1]:.0f} MiB of shmem to swap (high={high_ev[1]}).', ha='center', fontsize=7)
    fig.tight_layout(rect=[0,0.06,1,0.94])
    fig.savefig(f'{OUT}/figure4_reclaim_authority.png', dpi=200)
    plt.close(fig)
    with open(f'{OUT}/figure4_reclaim_authority.csv','w',newline='') as f:
        w = csv.writer(f)
        w.writerow(['condition','pgscan','pgsteal','shmem_reclaimed_MiB','memory_events_high'])
        w.writerow(['memory.swap.max=0 (S3d)', 0, 0, 0, 37])
        for run,ps,pst,sw,hi in [(1,120668,60109,248049664,512),(2,120850,60205,248401920,515),(3,120543,60108,247537664,513)]:
            w.writerow([f'swap available (S3e run {run})', ps, pst, round(sw/MiB,1), hi])
    print("figure4 done")

# ================================================================ FIGURE 5
def figure5():
    # total fetches (demand + prefetch), D vs E, one panel per compute, 5 ratio groups. 128 MiB.
    cs = CS128
    import numpy as np
    fig, axes = plt.subplots(1, 2, figsize=(13, 5.5), sharey=True)
    ratios = RA5
    csv_rows = [('chunk_size','ratio','compute','arm','policy','demand','prefetch','total_fetches','source')]
    x = np.arange(len(ratios)); w = 0.38
    for ax, comp in zip(axes, ['0','400000']):
        dvals, evals = [], []
        for ra in ratios:
            dd = pd_median(cs, ra, 'D', comp, 'absent_handled')
            dp = pd_median(cs, ra, 'D', comp, 'prefetches') or 0
            ed = pd_median(cs, ra, 'E', comp, 'absent_handled')
            ep = pd_median(cs, ra, 'E', comp, 'prefetches') or 0
            dvals.append((dd or 0) + dp); evals.append((ed or 0) + ep)
            csv_rows.append((cs, ra, comp, 'D', 'layer_order_learned', dd, dp, (dd or 0)+dp, 'campaign12_phaseD'))
            csv_rows.append((cs, ra, comp, 'E', 'layer_order_learned', ed, ep, (ed or 0)+ep, 'campaign12_phaseD'))
        b1 = ax.bar(x - w/2, dvals, w, color='#2ca02c', label='D (prefetch off)')
        b2 = ax.bar(x + w/2, evals, w, color='#ff7f0e', label='E (prefetch on)')
        for b in list(b1)+list(b2):
            ax.text(b.get_x()+b.get_width()/2, b.get_height()+0.5, f'{int(b.get_height())}',
                    ha='center', va='bottom', fontsize=8)
        ax.set_xticks(x); ax.set_xticklabels([f'r={r}' for r in ratios])
        ax.set_title(f'compute = {comp}'); ax.grid(alpha=0.3, axis='y'); ax.legend(fontsize=9)
        ax.set_ylabel('total fetches (demand + prefetch)')
    fig.suptitle('Figure 5 — Prefetch: total fetches, not hit rate (128 MiB, arm D vs E)', fontsize=12)
    fig.text(0.5, 0.02, 'Hit rate (used through Campaign 12) was found in Campaign 13 Phase B to be an artifact '
             'of its own denominator -- issuing more prefetches mechanically dilutes it. Total fetches is the '
             'volume metric that replaced it: E beats D only where a compute phase gives prefetch time to land. '
             'Source: Campaign 12 Phase D (layer_order_learned).', ha='center', fontsize=7)
    fig.tight_layout(rect=[0,0.12,1,0.93])
    fig.savefig(f'{OUT}/figure5_prefetch_total_fetches.png', dpi=200)
    plt.close(fig)
    with open(f'{OUT}/figure5_prefetch_total_fetches.csv','w',newline='') as f:
        csv.writer(f).writerows(csv_rows)
    print("figure5 done")

# ================================================================ FIGURE 6
def figure6():
    """llama.cpp real workload -- same idea as Figure 1, real model. Only if WP2 produced data."""
    import os
    swp = f'{R}/overnight/wp2_sweep.csv'
    optp = f'{R}/overnight/wp2_opt.csv'
    if not (os.path.exists(swp) and os.path.exists(optp)):
        print("figure6 SKIPPED -- no WP2 sweep data"); return
    wr = [r for r in rows_(swp) if r['rc'] == '0']
    opt = {o['ratio']: int(o['opt_missed_bytes']) for o in rows_(optp) if o['opt_missed_bytes']}
    by6 = {}
    for r in wr: by6.setdefault((r['ratio'], r['arm']), []).append(r)
    RA = sorted({r['ratio'] for r in wr}, key=float)
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))
    csv_rows = [('ratio','arm','read_bytes_GB','read_bytes_per_OPT','tokens_s','demand_faults','source')]
    style = {'A':('-o','#7a7a7a','A (kernel mmap)'), 'C':('-s','#1f77b4','C (kernel LRU)'),
             'D':('-^','#2ca02c','D (declared order)'), 'E':('-D','#ff7f0e','E (declared + prefetch)')}
    for arm,(st,col,lab) in style.items():
        xr, yb, yt = [], [], []
        for ra in RA:
            rs = by6.get((ra, arm), [])
            if not rs: continue
            fld = 'io_gen_bytes' if arm == 'A' else 'pager_bytes_fetched'
            vals = [int(x[fld]) for x in rs if x[fld] not in ('','n/a')]
            if not vals: continue
            rb = statistics.median(vals)
            ts = statistics.median([float(x['tokens_s']) for x in rs if x['tokens_s'] not in ('','n/a')])
            df = statistics.median([int(x['absent_handled']) for x in rs if x['absent_handled'] not in ('','n/a')]) if arm != 'A' else 0
            xr.append(float(ra)); yb.append(rb/1e9); yt.append(ts)
            ob = opt.get(ra)
            csv_rows.append((ra, arm, f'{rb/1e9:.1f}', f'{rb/ob:.3f}' if ob else '', f'{ts:.2f}', df, 'wp2_sweep'))
        ax1.plot(xr, yb, st, color=col, label=lab)
        ax2.plot(xr, yt, st, color=col, label=lab)
    # OPT
    xr, yb = [], []
    for ra in RA:
        if ra in opt: xr.append(float(ra)); yb.append(opt[ra]/1e9)
    ax1.plot(xr, yb, '--', color='k', label='OPT (Belady, declared seq)')
    for ra in RA:
        if ra in opt: csv_rows.append((ra, 'OPT', f'{opt[ra]/1e9:.1f}', '1.000', '', '', 'wp2_opt'))
    ax1.set_xlabel('budget ratio (of weight region)'); ax1.set_ylabel('bytes read during generation (GB)')
    ax1.set_title('bytes read, 64 tokens'); ax1.grid(alpha=0.3); ax1.legend(fontsize=8)
    ax2.set_xlabel('budget ratio (of weight region)'); ax2.set_ylabel('tokens / s')
    ax2.axhline(13.2, ls=':', color='gray', label='unconstrained baseline (13.2 t/s)')
    ax2.set_title('generation throughput'); ax2.grid(alpha=0.3); ax2.legend(fontsize=8)
    fig.suptitle('Figure 6 — llama.cpp (Qwen2.5-3B Q4_K_M): real model through the pager', fontsize=12)
    fig.text(0.5, 0.02, 'Arm E collapsed at r=0.25 (360s timeout) -- absent from that x-point. D reads 24-51% '
             'fewer bytes than the kernel at r>=0.5 and D/OPT=1.09-1.14. A/C cannot turn extra budget into '
             'throughput; D/E can. layer_order_declared with the WP0 fix.', ha='center', fontsize=7)
    fig.tight_layout(rect=[0,0.07,1,0.94])
    fig.savefig(f'{OUT}/figure6_llamacpp.png', dpi=200)
    plt.close(fig)
    with open(f'{OUT}/figure6_llamacpp.csv','w',newline='') as f:
        csv.writer(f).writerows(csv_rows)
    print("figure6 done")

def rows_(p):
    with open(p) as f: return list(csv.DictReader(f))

# ================================================================ TABLE 1
def table1():
    """arm x ratio x chunk x compute: read_bytes/touch, total fetches, demand faults, wall, D/OPT.
    Footnote key: [x] excluded non-deterministic (C13-A); [h] host-cache contaminated (>3396 MiB/s);
    [s] superseded by WP1 declared order; [d] declared-order (WP1, layer_order_declared)."""
    hdr = ['chunk','ratio','arm','policy','compute','read_bytes_per_touch_MiB','total_fetches',
           'demand_faults','wall_s','arm/OPT','flags']
    out = [hdr]
    for cs in (CS8, CS128):
        tp = TOUCHES[cs]
        cslabel = f'{int(cs)//1048576}MiB'
        for ra in RA5:
            optb = OPT_BYTES.get((cs, ra))
            # arms A, B (compute n/a)
            for arm in ('A','B'):
                if arm == 'A':
                    b = pd_armA_best(cs, ra)
                    if not b: continue
                    _, rb, wall, mibs, mode = b
                    flags = 'h' if mibs > OD_CEIL else ''
                    pol = f'madvise:{mode}'
                else:
                    rs = [r for r in phaseD if r['chunk_size']==cs and r['ratio']==ra and r['arm']=='B' and r['rc']=='0']
                    if not rs: continue
                    rb = med([int(r['pager_bytes_fetched']) if r['pager_bytes_fetched'] not in ('','n/a')
                              else int(r['io_read_bytes_delta']) for r in rs])
                    wall = med([int(r['wall_ns']) for r in rs])
                    bt = int(rs[0]['bytes_touched']); mibs = bt/(wall/1e9)/MiB
                    flags = 'h' if mibs > OD_CEIL else ''
                    pol = 'madvise+hints'
                out.append([cslabel, ra, arm, pol, 'n/a', f'{rb/tp/MiB:.2f}', 'n/a', 'n/a',
                            f'{wall/1e9:.3f}', f'{rb/optb:.3f}' if optb else 'n/a', flags])
            # arms C, D, E (learned) x compute
            for comp in ('0','400000'):
                for arm in ('C','D','E'):
                    rs = pd_cell(cs, ra, arm, comp)
                    if not rs: continue
                    rb = med([i(r['pager_bytes_fetched']) for r in rs])
                    dem = med([i(r['absent_handled']) for r in rs])
                    pf = med([i(r['prefetches']) for r in rs]) or 0
                    wall = med([i(r['wall_ns']) for r in rs])
                    flags = ''
                    if arm == 'D' and (cs, ra, comp) in NONDET: flags = 'x'
                    if arm in ('D','E') and comp == '0' and ra in RA3: flags += 's'
                    pol = 'lru' if arm=='C' else 'layer_order_learned'
                    out.append([cslabel, ra, arm, pol, comp, f'{rb/tp/MiB:.2f}', int(dem+pf),
                                int(dem), f'{wall/1e9:.3f}', f'{rb/optb:.3f}' if optb else 'n/a', flags])
            # arms D, E declared (WP1), 3 ratios
            if ra in RA3:
                for comp in ('0','400000'):
                    for arm in ('D','E'):
                        rs = wp1_cell(cs, ra, arm, 'layer_order_declared', comp)
                        if not rs: continue
                        rb = med([i(r['pager_bytes_fetched']) for r in rs])
                        dem = med([i(r['absent_handled']) for r in rs])
                        pf = med([i(r['prefetches']) for r in rs]) or 0
                        wall = med([i(r['wall_ns']) for r in rs])
                        flags = 'd'
                        # WP0 fix made the compute=400000 declared cells deterministic (WP1 §1.3
                        # post-fix: cell 5 = this grid's config is deterministic). No 'X' flag.
                        out.append([cslabel, ra, arm, 'layer_order_declared', comp, f'{rb/tp/MiB:.2f}',
                                    int(dem+pf), int(dem), f'{wall/1e9:.3f}',
                                    f'{rb/optb:.3f}' if optb else 'n/a', flags])
            # OPT row
            if optb:
                out.append([cslabel, ra, 'OPT', 'belady', 'n/a', f'{optb/tp/MiB:.2f}',
                            OPT_MISS[(cs,ra)], OPT_MISS[(cs,ra)], 'n/a', '1.000', ''])
    with open(f'{OUT}/table1_main_results.csv','w',newline='') as f:
        csv.writer(f).writerows(out)
    # markdown
    with open(f'{OUT}/table1_main_results.md','w') as f:
        f.write('# Table 1 — Main results\n\n')
        f.write('read_bytes per touch (MiB) = pager_bytes_fetched / total references. '
                'total fetches = demand faults + prefetches. arm/OPT = read_bytes / OPT bytes.\n\n')
        f.write('Flag key: **x** = excluded, non-deterministic arm D LEARNED cell (Campaign 13 Phase A — '
                'one sample from a distribution). The WP0 consumption-signal fix made the DECLARED '
                'compute=400000 cells deterministic, so no X flags remain on declared rows. '
                '**h** = arm A/B host-cache contaminated (achieved bandwidth > 3396 MiB/s O_DIRECT ceiling — '
                'Windows VHDX host cache, out of scope to defeat). '
                '**s** = superseded on this metric by the declared-order row below it (WP1). '
                '**d** = WP1 declared-order policy (`layer_order_declared`).\n\n')
        f.write('| ' + ' | '.join(hdr) + ' |\n')
        f.write('|' + '|'.join(['---']*len(hdr)) + '|\n')
        for row in out[1:]:
            f.write('| ' + ' | '.join(str(c) for c in row) + ' |\n')
    print("table1 done")

# ================================================================ TABLE 2
def table2():
    env = [
        ('kernel release', '6.18.33.2-microsoft-standard-WSL2 (WSL2, guest-only; bare metal out of scope)'),
        ('CPU / RAM', '16 logical cores / 7.6 GiB (WSL2 VM)'),
        ('filesystem (model + region backing)', 'ext4 on /dev/sdd (rw,relatime,discard,data=ordered); Windows VHDX host cache present and unreachable by guest drop_caches'),
        ('cgroup version', 'v2 (cgroup2fs); controllers: cpuset cpu io memory hugetlb pids rdma'),
        ('THP transparent_hugepage/enabled', 'madvise'),
        ('THP shmem_enabled', 'never (shmem pages are not transparently collapsed to hugepages)'),
        ('memory.swap.max', '0 in every sweep (I-3) — eviction authority test'),
        ('memory.high', 'budget_bytes (per cell); memory.max = budget_bytes + 64 MiB margin'),
        ('region size', '2 GiB (2,147,483,648 B), fixed across every sweep except chunk-size sweeps'),
        ('chunk sizes swept', 'Phase D / WP1: {8 MiB (256 chunks), 128 MiB (16 chunks)}; chunk-size sweeps: {4,8,16,32,64,128,256} MiB'),
        ('n_passes', '5 (references = 5 x n_chunks)'),
        ('handler mode', 'async dispatch-only (A-5); --sync-handler available but unused in these sweeps'),
        ('fetch workers', '4 (--fetch-workers 4)'),
        ('driver threads', '8 (--driver-threads 8)'),
        ('lookahead window', '1 (--lookahead-window 1; W+1=2 chunks in flight max)'),
        ('prefetch depth', '2 for arm E (--prefetch-depth 2); n/a for arm D (prefetch off)'),
        ('prefetch retention', 'pinned (A-9, --prefetch-retention pinned)'),
        ('prefetch admission', 'guarded (A-6 default)'),
        ('reconcile interval', 'amortized 16 in sweeps; 1 (eager) hard-compiled in T-1..T-7'),
        ('policy', 'WP1: layer_order_declared (default, A-12) and layer_order_learned; prior campaigns: layer_order (== learned)'),
        ('compute phase', '--compute-ns-per-mib in {0, 400000}; achieved rate ~1.5-1.8 Mns/MiB at the 400000 setting (calibration overshoot, disclosed Campaign 11 Phase 2)'),
        ('O_DIRECT bandwidth ceiling', '3396 MiB/s (spike max; points above = host-cache contamination)'),
        ('model file hash (WP2)', 'n/a — WP2 not run, models/model.gguf absent (BLOCKER 1)'),
    ]
    with open(f'{OUT}/table2_environment.csv','w',newline='') as f:
        w = csv.writer(f); w.writerow(['parameter','value'])
        for k,v in env: w.writerow([k,v])
    with open(f'{OUT}/table2_environment.md','w') as f:
        f.write('# Table 2 — Environment and configuration\n\n')
        f.write('Every value needed to reproduce the sweeps behind Figures 1–5 and Table 1.\n\n')
        f.write('| parameter | value |\n|---|---|\n')
        for k,v in env: f.write(f'| {k} | {v} |\n')
    print("table2 done")

# ================================================================ FINAL SESSION
# Extends this script (does not rewrite it). Figures 3/4/5 are unchanged
# (historical data). Figure 6 is regenerated from Phase 1's equal-budget real
# model sweep; Figure 7 is new. Tables 1/2 get final-session companions.
F = f'{R}/final'

def _p1rows():
    return rows_(f'{F}/phase1_equal_budget.csv')

def _p1opt():
    d = {}
    for o in rows_(f'{F}/phase1_opt.csv'):
        if o['passes'] == '65':
            d[o['ratio']] = int(o['opt_missed_bytes'])
    return d

def _med(xs):
    xs = sorted(x for x in xs if x is not None)
    return xs[len(xs)//2] if xs else None

def figure6_final():
    """llama.cpp real model, EQUAL BUDGET (Phase 1). bytes read + tokens/s vs
    budget ratio; arms A/C/D/E + OPT. 5 ratios. Arm E at r=0.25 is the Phase 3
    fallback config (protect-off + retention-none), marked distinctly."""
    pr = _p1rows(); opt = _p1opt()
    RA = ['0.25','0.375','0.5','0.625','0.75']
    def cell(ra, arm):
        rs = [r for r in pr if r['ratio']==ra and r['arm']==arm and r['rc']=='0']
        if not rs: return None
        fld = 'io_gen_bytes' if arm=='A' else 'pager_bytes_fetched'
        rb = _med([int(r[fld]) for r in rs if r[fld] not in ('','n/a')])
        ts = _med([float(r['tokens_s']) for r in rs if r['tokens_s'] not in ('','n/a')])
        df = _med([int(r['absent_handled']) for r in rs if r['absent_handled'] not in ('','n/a')]) if arm!='A' else None
        return rb, ts, df
    # Phase 3 fallback point for arm E at r=0.25 (off_retention_none, median)
    p3 = [r for r in rows_(f'{F}/phase3_arm_e.csv') if r['config']=='off_retention_none' and r['rc']=='0']
    e025 = (_med([int(r['pager_bytes_fetched']) for r in p3]),
            _med([float(r['tokens_s']) for r in p3]),
            _med([int(r['absent_handled']) for r in p3])) if p3 else None

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))
    csv_rows = [('ratio','arm','read_GB','read_per_OPT','tokens_s','demand_faults','note','source')]
    style = {'A':('-o','#7a7a7a','A (kernel mmap)'),'C':('-s','#1f77b4','C (kernel LRU)'),
             'D':('-^','#2ca02c','D (declared order)'),'E':('-D','#ff7f0e','E (declared + prefetch)')}
    for arm,(st,col,lab) in style.items():
        xr,yb,yt = [],[],[]
        for ra in RA:
            c = cell(ra, arm)
            if arm=='E' and ra=='0.25':
                if e025 is None: continue
                rb,ts,df = e025
                ax1.plot([0.25],[rb/1e9], marker='*', ms=16, color=col, mec='k', mew=0.6, zorder=6)
                ax2.plot([0.25],[ts], marker='*', ms=16, color=col, mec='k', mew=0.6, zorder=6)
                ob = opt.get(ra)
                csv_rows.append((ra,arm,f'{rb/1e9:.1f}',f'{rb/ob:.3f}' if ob else '',f'{ts:.2f}',df,
                                 'Phase3 fallback: protect-off + retention-none (collapses in default config)','phase3_arm_e'))
                continue
            if c is None: continue
            rb,ts,df = c
            xr.append(float(ra)); yb.append(rb/1e9); yt.append(ts)
            ob = opt.get(ra)
            csv_rows.append((ra,arm,f'{rb/1e9:.1f}',f'{rb/ob:.3f}' if ob else '',f'{ts:.2f}',df,'','phase1_equal_budget'))
        ax1.plot(xr,yb,st,color=col,label=lab); ax2.plot(xr,yt,st,color=col,label=lab)
    xr = [float(r) for r in RA if r in opt]; yb = [opt[r]/1e9 for r in RA if r in opt]
    ax1.plot(xr,yb,'--',color='k',label='OPT (Belady, declared seq)')
    for r in RA:
        if r in opt: csv_rows.append((r,'OPT',f'{opt[r]/1e9:.1f}','1.000','','','','phase1_opt'))
    ax1.set_xlabel('budget ratio (of weight region)'); ax1.set_ylabel('bytes read during generation (GB)')
    ax1.set_title('bytes read, 64 tokens (equal budget)'); ax1.grid(alpha=0.3); ax1.legend(fontsize=8)
    ax2.set_xlabel('budget ratio (of weight region)'); ax2.set_ylabel('tokens / s')
    ax2.axhline(13.2, ls=':', color='gray'); ax2.set_title('generation throughput'); ax2.grid(alpha=0.3); ax2.legend(fontsize=8)
    fig.suptitle('Figure 6 — llama.cpp (Qwen2.5-3B Q4_K_M), equal budget (Phase 1)', fontsize=12)
    fig.text(0.5, 0.02, 'memory.max = B for arm A; budget_bytes = B (+128 MiB memory.max) for the pager arms. '
             'Arm D beats arm A on bytes at every ratio (D/A 0.98..0.52); D/OPT = 1.09-1.14. '
             'Arm E * at r=0.25 = Phase 3 fallback (protect-off + retention-none); the default arm-E config '
             'deadlocks there. A/C flat in throughput; D/E scale.', ha='center', fontsize=7)
    fig.tight_layout(rect=[0,0.07,1,0.94])
    fig.savefig(f'{OUT}/figure6_llamacpp.png', dpi=200); plt.close(fig)
    with open(f'{OUT}/figure6_llamacpp.csv','w',newline='') as f:
        csv.writer(f).writerows(csv_rows)
    print("figure6_final done")

def figure7():
    """Throughput scaling -- the project's strongest single result. x: budget
    ratio; y: tokens/s; lines A/C/D/E, real model, equal budget."""
    pr = _p1rows()
    RA = ['0.25','0.375','0.5','0.625','0.75']
    fig, ax = plt.subplots(figsize=(9, 5.5))
    csv_rows = [('ratio','arm','tokens_s_median','tokens_s_reps','source')]
    style = {'A':('-o','#7a7a7a','A (kernel mmap)'),'C':('-s','#1f77b4','C (kernel LRU)'),
             'D':('-^','#2ca02c','D (layer_order_declared)'),'E':('-D','#ff7f0e','E (declared + prefetch)')}
    for arm,(st,col,lab) in style.items():
        xr,yt = [],[]
        for ra in RA:
            rs = [r for r in pr if r['ratio']==ra and r['arm']==arm and r['rc']=='0']
            reps = [float(r['tokens_s']) for r in rs if r['tokens_s'] not in ('','n/a')]
            if not reps: continue
            m = _med(reps)
            xr.append(float(ra)); yt.append(m)
            csv_rows.append((ra,arm,f'{m:.3f}','|'.join(f'{x:.3f}' for x in reps),'phase1_equal_budget'))
        if xr: ax.plot(xr,yt,st,color=col,label=lab, lw=2, ms=8)
    ax.axhline(13.2, ls=':', color='gray', label='unconstrained baseline (13.2 t/s)')
    ax.set_xlabel('budget ratio (fraction of the 2.10 GB weight region)')
    ax.set_ylabel('generation throughput (tokens / s)')
    ax.set_title('Figure 7 — The kernel cannot turn memory into throughput; the app-authoritative pager can',
                 fontsize=11)
    ax.grid(alpha=0.3); ax.legend(fontsize=9)
    fig.text(0.5, 0.01, 'Qwen2.5-3B Q4_K_M, CPU, 64 tokens, n=3, equal budget (Phase 1). Arms A and C are '
             'horizontal (0.6-0.9 t/s at every budget); arm D rises 0.91 -> 1.95 t/s (2.1x). Arm A absolute '
             'values are reclaim-noisy; the flat trend is the robust result.', ha='center', fontsize=7)
    fig.tight_layout(rect=[0,0.06,1,0.96])
    fig.savefig(f'{OUT}/figure7_throughput_scaling.png', dpi=200); plt.close(fig)
    with open(f'{OUT}/figure7_throughput_scaling.csv','w',newline='') as f:
        csv.writer(f).writerows(csv_rows)
    print("figure7 done")

def table1_final():
    """Real-model main results at equal budget (Phase 1) + the Phase 2 synthetic
    D/OPT for the final policy config. Footnote key inline."""
    pr = _p1rows(); opt = _p1opt()
    RA = ['0.25','0.375','0.5','0.625','0.75']
    hdr = ['ratio','arm','config','read_GB','read/OPT','demand_faults','tokens_s_med','p99_inter_ms_med','note']
    out=[hdr]
    for ra in RA:
        ob = opt.get(ra)
        for arm in ('A','C','D','E'):
            rs = [r for r in pr if r['ratio']==ra and r['arm']==arm and r['rc']=='0']
            note=''
            if arm=='E' and not rs:
                p3=[r for r in rows_(f'{F}/phase3_arm_e.csv') if r['config']=='off_retention_none' and r['rc']=='0']
                if p3:
                    rb=_med([int(r['pager_bytes_fetched']) for r in p3]); ts=_med([float(r['tokens_s']) for r in p3])
                    dfx=_med([int(r['absent_handled']) for r in p3]); p99='-'
                    note='COLLAPSES in default config; Phase 3 fallback protect-off+retention-none'
                    out.append([ra,arm,'prefetch on (fallback)',f'{rb/1e9:.1f}',f'{rb/ob:.3f}' if ob else '-',dfx,f'{ts:.2f}',p99,note]);
                continue
            if not rs: continue
            fld='io_gen_bytes' if arm=='A' else 'pager_bytes_fetched'
            rb=_med([int(r[fld]) for r in rs if r[fld] not in ('','n/a')])
            ts=_med([float(r['tokens_s']) for r in rs if r['tokens_s'] not in ('','n/a')])
            p99=_med([float(r['p99_inter_ms']) for r in rs if r['p99_inter_ms'] not in ('','n/a')])
            dfx=_med([int(r['absent_handled']) for r in rs if r['absent_handled'] not in ('','n/a')]) if arm!='A' else '-'
            cfg={'A':'kernel mmap, memory.max=B','C':'lru','D':'layer_order_declared, prefetch off','E':'declared + prefetch d2 pinned'}[arm]
            if arm=='A':
                ach=_med([float(r['achieved_mibs']) for r in rs if r['achieved_mibs'] not in ('','n/a')])
                note=f'achieved {ach:.0f} MiB/s (< 3396 ceiling, no host-cache flag)'
            out.append([ra,arm,cfg,f'{rb/1e9:.1f}',f'{rb/ob:.3f}' if ob else '-',dfx,f'{ts:.2f}',
                        f'{p99:.0f}' if p99 else '-',note])
        if ob: out.append([ra,'OPT','belady, declared seq, 65 passes',f'{ob/1e9:.1f}','1.000','-','-','-',''])
    with open(f'{OUT}/table1_final_real_model.csv','w',newline='') as f:
        csv.writer(f).writerows(out)
    with open(f'{OUT}/table1_final_real_model.md','w') as f:
        f.write('# Table 1 (FINAL) — real model, equal budget\n\n')
        f.write('Qwen2.5-3B Q4_K_M, CPU, 64 tokens, n=3, `memory.max = B` (arm A) / `budget_bytes = B` '
                '(+128 MiB `memory.max`, pager arms). read/OPT uses `wp2_opt` over the declared sequence '
                '(65 layer-scans). Arm A `read` is the `/proc/self/io` delta; C/D/E is `pager_bytes_fetched`.\n\n')
        f.write('| ' + ' | '.join(hdr) + ' |\n|' + '|'.join(['---']*len(hdr)) + '|\n')
        for row in out[1:]:
            f.write('| ' + ' | '.join(str(c) for c in row) + ' |\n')
    print("table1_final done")

def table2_final():
    import subprocess
    def q(c):
        try: return subprocess.check_output(c, shell=True, text=True).strip()
        except Exception: return 'n/a'
    env = [
        ('kernel release', q('uname -r') + ' (WSL2 guest; bare metal out of scope)'),
        ('CPU / RAM', q("nproc") + ' logical cores / ' + q("free -g | awk '/Mem:/{print $2\" GiB\"}'")),
        ('model file (WP2 / Phase 1 / Phase 3)', 'Qwen2.5-3B-Instruct-GGUF q4_k_m, 2,104,932,768 B'),
        ('model sha256', q("sha256sum /root/residctl/models/model.gguf | cut -d' ' -f1")),
        ('model layout', '435 tensors / 36 layers; 41 chunks (min 0.01 / median 41.5 / max 243 MiB); '
                         'tensors in name-lexicographic not layer order; layer 21 split across 2 non-contiguous chunks'),
        ('weight region', '2,104,934,400 B = align_up(file, 4096)'),
        ('cgroup', 'v2; memory.swap.max = 0 (I-3)'),
        ('equal-budget setup (Phase 1)', 'arm A: memory.max = B (= ratio x region). pager arms: budget_bytes = B, '
                                         'memory.max = B + 128 MiB (uniform, llama non-weight footprint). '
                                         'residual ~50 MiB weight-cache asymmetry favours the pager arms.'),
        ('ratios', 'Phase 1 real model: {0.25, 0.375, 0.5, 0.625, 0.75}, n=3. '
                   'Phase 2 synthetic: {0.25, 0.5, 0.75} x compute {0, 400000}, n=3.'),
        ('tokens generated', '64, fixed prompt (16 tokens), greedy/deterministic'),
        ('llama threads', '8 (-t 8), n_gpu_layers = 0, load mode mmap/residctl'),
        ('policy (final default)', 'layer_order_declared, --consumption-signal all-threads, --protect-current off '
                                   '(Phase 2 outcome). layer_order_learned retained as comparison arm.'),
        ('fetch workers', '4; async dispatch-only handler (A-5)'),
        ('prefetch (arm E)', 'depth 2, retention pinned -- EXCEPT r <= 0.375 where the recommendation is prefetch off '
                             '(Phase 3: default config deadlocks; fallback = retention none, still no benefit)'),
        ('per-layer compute (measured)', '~51,000 ns/MiB (13.2 t/s baseline / 36 layers / 41.5 MiB per layer chunk)'),
        ('O_DIRECT bandwidth ceiling', '3396 MiB/s; Phase 1 arm A achieved 920-1310 MiB/s (fault-stall-bound, no contamination)'),
        ('T-1..T-7', 'PASS with --eager-reconcile, after every Phase 2 code change and at session end'),
        ('disk free', q("df -h /root | awk 'NR==2{print $4}'")),
    ]
    with open(f'{OUT}/table2_final_environment.csv','w',newline='') as f:
        w=csv.writer(f); w.writerow(['parameter','value'])
        for k,v in env: w.writerow([k,v])
    with open(f'{OUT}/table2_final_environment.md','w') as f:
        f.write('# Table 2 (FINAL) — environment and configuration\n\n| parameter | value |\n|---|---|\n')
        for k,v in env: f.write(f'| {k} | {v} |\n')
    print("table2_final done")

figure1(); figure2(); figure3(); figure4(); figure5(); figure6()
table1(); table2()
figure6_final(); figure7(); table1_final(); table2_final()
print("ALL DONE ->", OUT)
