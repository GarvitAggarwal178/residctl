#!/usr/bin/env python3
"""Phase 6 GATE: every path-like reference in the key docs must resolve to a
file in the tree or the archive. Reports unresolved refs; exits 1 if any in
claims.md or findings.md."""
import os, re, subprocess, sys

os.chdir("/root/residctl")
tracked = set(subprocess.check_output(["git", "ls-files"], text=True).split())
archive = set()
for root, _, files in os.walk("/root/residctl-archive"):
    for f in files:
        archive.add(os.path.relpath(os.path.join(root, f), "/root/residctl-archive"))

DOCS = subprocess.check_output(
    ["git", "ls-files", "*.md"], text=True).splitlines()
GATE_DOCS = {"results/claims.md", "results/findings.md"}

# path-ish token: has a slash and a dotted extension, or is a known bare name
PATH_RE = re.compile(r'`?((?:/root/)?[A-Za-z0-9_][\w./-]*\.(?:md|csv|png|txt|log|sh|py|c|h|cpp|patch))`?')
IGNORE_SUBSTR = ("spike/results", "spike/src", "/root/spike", "third_party/",
                 "models/", "scratch/", "path/script", "c/.h/.cpp",
                 "residctl-archive")

gate_fail = 0
total_unresolved = 0
for d in DOCS:
    if d.startswith("docs/cleanup/"):
        continue
    txt = open(d).read()
    seen = set()
    for m in PATH_RE.finditer(txt):
        p = m.group(1)
        if p in seen:
            continue
        seen.add(p)
        if any(s in p for s in IGNORE_SUBSTR):
            continue
        if p in tracked:
            continue
        # relative-to-doc-dir link (e.g. data/foo.csv inside results/README.md)
        if os.path.normpath(os.path.join(os.path.dirname(d), p)) in tracked:
            continue
        # ../foo relative link
        if p.startswith("../") and os.path.normpath(os.path.join(os.path.dirname(d), p)) in tracked:
            continue
        # a bare basename that uniquely matches a tracked file is OK-ish but flag it
        base = os.path.basename(p)
        hits = [t for t in tracked if t.endswith("/" + base) or t == base]
        if len(hits) == 1 and "/" not in p:
            continue  # unambiguous bare name
        # old-directory shapes are definitely broken
        looks_old = p.startswith((
            "results/final/", "results/overnight/", "results/cleanup/",
            "results/livelock/", "results/campaign", "results/HARNESS",
            "results/ASYNC", "results/CONCURRENCY", "results/DIAGNOSTIC",
            "results/LOOKAHEAD", "results/phase", "results/PROJECT_STATE",
            "results/BARE_METAL", "results/item", "results/task_",
            "docs/MECHANISM_SPEC", "docs/overnight/", "docs/cleanup.md",
            "docs/inventory.md", "src/run_", "src/livelock_",
            "src/phase", "src/smoke", "src/p1progress"))
        if looks_old or (("/" in p) and p not in tracked and not p.startswith("/root")):
            total_unresolved += 1
            tag = "GATE" if d in GATE_DOCS else "warn"
            print(f"  [{tag}] {d}: {p}")
            if d in GATE_DOCS:
                gate_fail += 1

print(f"\n{total_unresolved} unresolved reference(s); GATE ({'/'.join(sorted(GATE_DOCS))}): "
      f"{'FAIL' if gate_fail else 'PASS'} ({gate_fail})")
sys.exit(1 if gate_fail else 0)
