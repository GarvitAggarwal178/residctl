#!/usr/bin/env python3
"""Phase 2 of the restructure: archive the noise.

For every rename-map row with kind == 'archive':
  copy repo file -> /root/residctl-archive/<subdir>/<name>
  verify sha256
  (git rm is done by the caller, grouped into commits)

Prints the manifest and a sha256 verification table. Never deletes anything.
"""
import csv, hashlib, os, shutil, subprocess, sys

REPO = "/root/residctl"
ARCH = "/root/residctl-archive"
os.chdir(REPO)

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()

rows = [r for r in csv.DictReader(open("docs/cleanup/rename-map.csv"))
        if r["kind"] == "archive"]

os.makedirs(ARCH, exist_ok=True)
manifest = []
mismatches = 0
total = 0
for r in rows:
    old = r["old_path"]
    rel = r["new_path"].split("ARCHIVE/", 1)[1]          # e.g. run-configs/foo.cfg
    dst = os.path.join(ARCH, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    src_hash = sha(old)
    shutil.copy2(old, dst)
    dst_hash = sha(dst)
    ok = src_hash == dst_hash
    mismatches += (not ok)
    sz = os.path.getsize(old)
    total += sz
    manifest.append((rel, old, sz, src_hash[:12], "OK" if ok else "MISMATCH"))

grp = {}
for rel, old, sz, h, st in manifest:
    grp.setdefault(rel.split("/")[0], []).append((rel, old, sz, h, st))

print(f"# Archive manifest — {len(manifest)} files, {total/1024:.0f} KiB, {mismatches} hash mismatches\n")
for g in sorted(grp):
    gsz = sum(x[2] for x in grp[g])
    print(f"## {g}/  ({len(grp[g])} files, {gsz/1024:.0f} KiB)")
    for rel, old, sz, h, st in sorted(grp[g]):
        print(f"  {st:8} {h}  {sz:>9}  {old}")
    print()

with open(os.path.join(ARCH, "MANIFEST.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["archive_path", "repo_path", "bytes", "sha256", "verify"])
    for rel, old, sz, h, st in sorted(manifest):
        w.writerow([f"ARCHIVE/{rel}", old, sz, sha(os.path.join(ARCH, rel)), st])

sys.exit(1 if mismatches else 0)
