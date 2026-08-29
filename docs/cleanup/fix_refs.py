#!/usr/bin/env python3
"""Phase 6: rewrite path references after the restructure. Conservative.

1. Full-path references for files that actually moved (old != new). Unambiguous.
2. A curated basename map — only data/figure/report basenames that are unique
   post-move AND do not collide with a kept src/ file. No prose-word risk.

Dry-run by default; --apply to write.
"""
import csv, os, re, subprocess, sys

os.chdir("/root/residctl")
APPLY = "--apply" in sys.argv
rows = list(csv.DictReader(open("docs/cleanup/rename-map.csv")))

kept_basenames = {os.path.basename(r["old_path"]) for r in rows if r["kind"] == "keep"}

fullmap = {}
for r in rows:
    old, new, kind = r["old_path"], r["new_path"], r["kind"]
    if new == "SPLIT" or old == new:
        continue
    if kind == "archive":
        new = "/root/residctl-archive/" + new.split("ARCHIVE/", 1)[1]
    fullmap[old] = new

# MECHANISM_SPEC.md is cited almost always for amendment provenance (A-N §M),
# which now lives in design-history.md — not the current-state 02-design.md.
fullmap["docs/MECHANISM_SPEC.md"] = "docs/design-history.md"

# curated basename -> new full path. Only files referenced bare in prose/tables
# whose basename is safe (a data/report filename, not a code file or a word).
CURATED = {}
for r in rows:
    old, new, kind = r["old_path"], r["new_path"], r["kind"]
    if new == "SPLIT" or old == new or kind == "archive":
        continue
    b = os.path.basename(old)
    if b in kept_basenames:            # collides with a kept src/ file — skip
        continue
    if not b.endswith((".csv", ".md", ".png")):
        continue
    if b in ("README.md",):
        continue
    # unique target?
    tgts = {n for o, n in fullmap.items() if os.path.basename(o) == b}
    if len(tgts) == 1:
        CURATED[b] = new

# report docs referenced by their OLD basename in prose ("the original X"):
# leave the name, but the auto-fixer would wreck them — exclude explicitly.
HISTORICAL_NAMES = {"MECHANISM_SPEC.md", "PROJECT_STATE.md",
                    "OVERNIGHT_SUMMARY.md", "FINAL_SUMMARY.md",
                    "CLEANUP_SUMMARY.md", "LIVELOCK_SUMMARY.md", "BLOCKERS.md"}
for h in HISTORICAL_NAMES:
    CURATED.pop(h, None)

targets = subprocess.check_output(
    ["git", "ls-files", "*.md", "*.sh", "*.py"], text=True).splitlines()
SKIP = {f"docs/cleanup/{x}" for x in
        ("gen_rename_map.py", "do_archive.py", "do_moves.py", "fix_refs.py",
         "rename-map.csv", "README.md")}

changed = {}
for t in targets:
    if t in SKIP:
        continue
    src = open(t, encoding="utf-8", errors="replace").read()
    out = src
    for old in sorted(fullmap, key=len, reverse=True):
        if old in out and fullmap[old] not in ("SPLIT",):
            out = out.replace(old, fullmap[old])
    if t.endswith(".md"):
        for b, new in CURATED.items():
            # bare basename in a backtick / paren / path context, not mid-word
            out = re.sub(r'(?<![\w./-])' + re.escape(b) + r'(?![\w])', new, out)
    if out != src:
        changed[t] = (src, out)

print(f"# Phase 6 — {'APPLY' if APPLY else 'DRY RUN'} — {len(changed)} files")
for t in sorted(changed):
    s, o = changed[t]
    lines = [(a, b) for a, b in zip(s.splitlines(), o.splitlines()) if a != b]
    print(f"\n## {t}  ({len(lines)} lines)")
    for a, b in lines[:6]:
        print(f"  - {a.strip()[:130]}")
        print(f"  + {b.strip()[:130]}")
    if APPLY:
        open(t, "w", encoding="utf-8").write(o)

print("\n# curated basename map:")
for b, n in sorted(CURATED.items()):
    print(f"  {b:44} -> {n}")
