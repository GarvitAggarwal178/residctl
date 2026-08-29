#!/usr/bin/env python3
"""Phase 3: execute the rename map with `git mv` (history follows each file).
No content edits. Run per group: experiments | data | figures | scripts | logs | syntheses
"""
import csv, os, subprocess, sys

os.chdir("/root/residctl")
group = sys.argv[1]
rows = list(csv.DictReader(open("experiments/logs/restructure/rename-map.csv")))

def want(r):
    k, new = r["kind"], r["new_path"]
    if k not in ("move",):
        return False
    if group == "experiments":
        return new.startswith("experiments/") and not new.startswith("experiments/logs/")
    if group == "data":
        return new.startswith("results/data/")
    if group == "figures":
        return new.startswith("results/figures/")
    if group == "scripts":
        return new.startswith("scripts/") or new.startswith("tools/")
    if group == "logs":
        return new.startswith("experiments/logs/")
    if group == "syntheses":
        return new in ("results/claims.md", "results/writeup-package.md",
                       "docs/04-related-work.md", "docs/bare-metal-plan.md")
    return False

sel = [r for r in rows if want(r)]
print(f"group {group}: {len(sel)} moves")
for r in sel:
    old, new = r["old_path"], r["new_path"]
    os.makedirs(os.path.dirname(new), exist_ok=True)
    if os.path.exists(new):
        print(f"  SKIP exists: {new}")
        continue
    subprocess.run(["git", "mv", "--", old, new], check=True)
print("done. staged:", len(subprocess.check_output(["git","diff","--cached","--name-only"],text=True).split()))
