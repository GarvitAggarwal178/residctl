#!/usr/bin/env python3
# LIVELOCK FIX Phase 2 analysis. Revised expectation: the synthetic
# --consumption-signal all-threads path is UNCHANGED vs the final session.
# Compare to results/final/phase2_{determinism,sweep,opt}.csv. Any mismatch
# on a cell that existed in the baseline is a REGRESSION.
import csv, pathlib, sys

LL   = pathlib.Path("/root/residctl/results/livelock")
FIN  = pathlib.Path("/root/residctl/results/final")

def rows(p):
    with open(p) as f: return list(csv.DictReader(f))

def med(xs):
    xs = sorted(float(x) for x in xs if x not in ("", None))
    return xs[len(xs)//2] if xs else None

# ---- determinism: our all-threads/protect-off vs baseline allthreads_off ----
print("=== A. determinism grid (all-threads, protect off) vs phase2_determinism.csv[allthreads_off] ===")
base = {}
for r in rows(FIN/"phase2_determinism.csv"):
    if r["combo"] != "allthreads_off": continue
    base.setdefault(r["cell"], {})[r["rep"]] = (r["absent_handled"], r["evictions"], r["pager_bytes_fetched"])
mine = {}
for r in rows(LL/"phase2_determinism.csv"):
    mine.setdefault(r["cell"], {})[r["rep"]] = (r["absent_handled"], r["evictions"], r["pager_bytes_fetched"], r.get("signal_mode",""))
det_ok = True
for cell in sorted(mine, key=int):
    vals = {v[:3] for v in mine[cell].values()}
    det  = "DETERMINISTIC" if len(vals) == 1 else f"NON-DETERMINISTIC ({len(vals)})"
    smode = {v[3] for v in mine[cell].values()}
    b = set(base.get(cell, {}).values())
    m3 = {v[:3] for v in mine[cell].values()}
    match = "MATCH baseline" if m3 == b else f"*** DIFFERS from baseline {b}"
    if m3 != b: det_ok = False
    print(f"  cell {cell}: {det}; signal_mode={smode}; mine={sorted(m3)}; {match}")
print(f"  => determinism grid {'UNCHANGED' if det_ok else 'REGRESSED'}")

# ---- sweep: layer_order_declared all-threads vs baseline ----
print("\n=== B. sweep layer_order_declared (all-threads) vs phase2_sweep.csv ===")
# baseline: combo allthreads_off / allthreads_on ; keyed (cs,ratio,compute)
bkey = {}
for r in rows(FIN/"phase2_sweep.csv"):
    combo = r["combo"]
    if combo not in ("allthreads_off","allthreads_on"): continue
    prot = "off" if combo.endswith("_off") else "on"
    bkey.setdefault((prot, r["chunk_size"], r["ratio"], r["compute"]), []).append(
        (r["absent_handled"], r["evictions"], r["pager_bytes_fetched"]))
opt = {}
for r in rows(FIN/"phase2_opt.csv"):
    opt[(r["chunk_size"], r["ratio"])] = float(r["opt_bytes"])

sweep_ok = True
mine_sweep = {}
for r in rows(LL/"phase2_sweep.csv"):
    mine_sweep.setdefault((r["policy"], r["protect"], r["chunk_size"], r["ratio"], r["compute"]), []).append(r)

for (pol, prot, cs, ratio, compute), rs in sorted(mine_sweep.items()):
    ah = {x["absent_handled"] for x in rs}
    ev = {x["evictions"] for x in rs}
    pbf = {x["pager_bytes_fetched"] for x in rs}
    sm = {x.get("signal_mode","") for x in rs}
    tag = f"{pol[11:]:9} protect={prot} cs={int(cs)//1048576}MiB r={ratio} c={compute}"
    if pol == "layer_order_declared":
        b = set(bkey.get((prot, cs, ratio, compute), []))
        m = {(x["absent_handled"], x["evictions"], x["pager_bytes_fetched"]) for x in rs}
        status = "MATCH" if (m == b and b) else (f"*** DIFFERS baseline={b} mine={m}" if b else "(no baseline)")
        if b and m != b: sweep_ok = False
        dopt = ""
        if (cs, ratio) in opt and med(pbf):
            dopt = f"  D/OPT={med(pbf)/opt[(cs,ratio)]:.3f}"
        print(f"  {tag}: pbf={sorted(int(x)//1048576 for x in pbf)}MiB sm={sm} {status}{dopt}")
    else:
        print(f"  {tag}: pbf={sorted(int(x)//1048576 for x in pbf)}MiB ah={sorted(ah)} sm={sm}  (learned -- regression check only)")

print(f"\n  => sweep (layer_order_declared, all-threads) {'UNCHANGED vs baseline' if sweep_ok else 'REGRESSED -- investigate'}")

# ---- 128 MiB declared D/OPT headline ----
print("\n=== C. headline: layer_order_declared 128 MiB D/OPT (baseline was 1.08 / 1.04 / 1.00) ===")
for ratio in ("0.25","0.5","0.75"):
    for prot in ("off","on"):
        rs = mine_sweep.get(("layer_order_declared", prot, "134217728", ratio, "0"), [])
        if not rs: continue
        pbf = med(x["pager_bytes_fetched"] for x in rs)
        o = opt.get(("134217728", ratio))
        print(f"  r={ratio} protect={prot}: D/OPT = {pbf/o:.3f}" if o else f"  r={ratio}: no opt")

ok = det_ok and sweep_ok
print("\n=== VERDICT ===")
print("Synthetic path UNCHANGED -- post-mode Defect 1 is a no-op refactor as designed." if ok
      else "Synthetic path CHANGED -- REGRESSION. Investigate the d0 refactor / build.")
sys.exit(0 if ok else 1)
