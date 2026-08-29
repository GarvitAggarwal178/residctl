#!/usr/bin/env python3
# LIVELOCK FIX Phase 3 analysis -- real model, all 4 fixes, signal_mode=pre,
# --protect-current off. Checks the 5 pre-registered expectations.
import csv, pathlib, statistics, sys

LL  = pathlib.Path("/root/residctl/results/livelock")
FIN = pathlib.Path("/root/residctl/results/final")
GB  = 1e9   # decimal GB, matching phase3_real_model.md / Table 1 / Figure 6

def rows(p):
    with open(p) as f: return list(csv.DictReader(f))

def med(xs):
    xs = [float(x) for x in xs if x not in ("", None)]
    return statistics.median(xs) if xs else None

mine = rows(LL / "phase3_real_model.csv")
by = {}
for r in mine:
    by.setdefault((r["ratio"], r["arm"]), []).append(r)

# baselines
d_base = {}   # ratio -> pager_bytes (protect-on, phase1)
for r in rows(FIN / "phase1_equal_budget.csv"):
    if r["arm"] == "D":
        d_base[r["ratio"]] = float(r["pager_bytes_fetched"])
opt = {}      # ratio -> opt_missed_bytes, 65 passes (prompt scan + 64 decode
              # scans) -- the canonical count used by Table 1 and Figure 6.
for r in rows(FIN / "phase1_opt.csv"):
    if r["passes"] == "65":
        opt[r["ratio"]] = float(r["opt_missed_bytes"])

ratios = ["0.25", "0.375", "0.5", "0.625", "0.75"]
print("=== Phase 3: real model, signal_mode=pre, protect_current=off ===\n")
print(f"{'ratio':>6} {'arm':>3} {'reps rc':>10} {'pager_GB(med)':>14} {'faults':>8} "
      f"{'declined':>9} {'infeas':>7} {'pinbrk':>7} {'ftimeout':>9} {'tok/s':>7} {'D/OPT':>7}")
for ra in ratios:
    for arm in ("C", "D", "E"):
        rs = by.get((ra, arm), [])
        if not rs: continue
        rcs = [r["rc"] for r in rs]
        done = all(r == "0" for r in rcs)
        pbf = med(r["pager_bytes_fetched"] for r in rs if r["rc"] == "0")
        fa  = med(r["absent_handled"] for r in rs if r["rc"] == "0")
        dec = med(r["prefetch_declined"] for r in rs if r["rc"] == "0")
        inf = med(r["infeasible"] for r in rs if r["rc"] == "0")
        pb  = med(r["pin_broken"] for r in rs if r["rc"] == "0")
        ft  = med(r["fetching_timeout"] for r in rs if r["rc"] == "0")
        ts  = med(r["tokens_s"] for r in rs if r["rc"] == "0")
        dopt = (pbf / opt[ra]) if (pbf and ra in opt) else None
        print(f"{ra:>6} {arm:>3} {','.join(rcs):>10} "
              f"{(pbf/GB if pbf else 0):>14.2f} {(fa or 0):>8.0f} "
              f"{(dec if dec is not None else -1):>9.0f} {(inf or 0):>7.0f} {(pb or 0):>7.0f} "
              f"{(ft or 0):>9.0f} {(ts or 0):>7.2f} {(f'{dopt:.3f}' if dopt else '-'):>7}")
    print()

# ---- expectations ----
print("=== pre-registered expectations ===\n")

# 1: arm E completes at r=0.25
e025 = by.get(("0.25", "E"), [])
e_done = e025 and all(r["rc"] == "0" for r in e025)
print(f"1. Arm E completes at r=0.25 (prefetch on, retention pinned, protect off): "
      f"{'YES -- ' + str(len(e025)) + '/' + str(len(e025)) + ' reps rc=0' if e_done else 'NO (rc=' + ','.join(r['rc'] for r in e025) + ')'}")

# 2: arm D reads fewer bytes than baseline 126.14 / 98.39 / 79.29 / 60.62 / 43.36
print("\n2. Arm D reads fewer bytes than the protect-on baseline:")
for ra in ratios:
    rs = by.get((ra, "D"), [])
    pbf = med(r["pager_bytes_fetched"] for r in rs if r["rc"] == "0")
    b = d_base.get(ra)
    if pbf and b:
        d = (pbf - b) / b * 100
        print(f"   r={ra}: {pbf/GB:6.2f} GB vs {b/GB:6.2f} GB  ({d:+.1f}%)  {'FEWER' if pbf < b else 'MORE -- expectation missed'}")

# 3: stat_prefetch_declined drops orders of magnitude from 2104
print("\n3. stat_prefetch_declined vs the pre-fix 2104:")
for ra in ratios:
    for arm in ("D", "E"):
        rs = by.get((ra, arm), [])
        dec = med(r["prefetch_declined"] for r in rs if r["rc"] == "0")
        if dec is not None:
            print(f"   r={ra} arm {arm}: {dec:.0f}")

# 4: arm E competitive at r <= 0.375
print("\n4. Arm E vs arm D at r <= 0.375 (report whichever way):")
for ra in ("0.25", "0.375"):
    d = med(r["pager_bytes_fetched"] for r in by.get((ra, "D"), []) if r["rc"] == "0")
    e = med(r["pager_bytes_fetched"] for r in by.get((ra, "E"), []) if r["rc"] == "0")
    if d and e:
        print(f"   r={ra}: E {e/GB:.2f} GB vs D {d/GB:.2f} GB  ({(e-d)/d*100:+.1f}%)  "
              f"{'E competitive/better' if e <= d*1.02 else 'E worse -- prefetch non-advantageous'}")

# 5: D/OPT improves on the baseline ~1.09-1.16
print("\n5. D/OPT vs the protect-on baseline:")
for ra in ratios:
    pbf = med(r["pager_bytes_fetched"] for r in by.get((ra, "D"), []) if r["rc"] == "0")
    b = d_base.get(ra)
    if pbf and b and ra in opt:
        print(f"   r={ra}: D/OPT {pbf/opt[ra]:.3f}  (baseline {b/opt[ra]:.3f})  "
              f"{'IMPROVED' if pbf < b else 'not improved'}")

print("\n(fetching_timeout must be 0 everywhere -- part (a) of the Phase-0-era fix "
      "closes the orphan paths; nonzero = a missed path.)")
