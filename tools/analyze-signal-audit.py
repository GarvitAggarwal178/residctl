#!/usr/bin/env python3
# LIVELOCK FIX Phase 0 -- analyse the cursor diagnostic.
# GATE (spec): FAIL if the notified chunk order does not match the declared
# sequence, or the policy cursor jumps. Item 3 (layer_transitions==declared_len)
# is NOT a gate -- reported only.
import struct, sys, re, pathlib

OUT = pathlib.Path("/root/residctl/results/livelock")
INV = OUT / "phase0_inventory.txt"
RT  = OUT / "phase0_reftrace.bin"
PT  = OUT / "phase0_policytrace.bin"
CON = OUT / "phase0_console.txt"

def load_declared():
    txt = INV.read_text().splitlines()
    seq = None
    groups = {}
    for i, ln in enumerate(txt):
        if ln.startswith("## declared sequence"):
            seq = [int(x) for x in txt[i+1].split()]
        m = re.match(r"\s*(\d+)\s+off=\s*\d+\s+len=\s*\d+\s+(\S+)", ln)
        if m:
            groups[int(m.group(1))] = m.group(2)
    return seq, groups

def load_reftrace():
    d = RT.read_bytes()
    assert d[:4] == b"RTRC", "bad reftrace magic"
    body = d[8:]; REC = 24
    return [struct.unpack("<QQIBBH", body[i*REC:(i+1)*REC])[2] for i in range(len(body)//REC)]

def load_policytrace():
    if not PT.exists() or PT.stat().st_size == 0: return []
    d = PT.read_bytes(); REC = struct.calcsize("<QIIqIII")
    out = []
    for i in range(len(d)//REC):
        s,v,c,dist,nr,lo,hi = struct.unpack("<QIIqIII", d[i*REC:(i+1)*REC])
        out.append(dict(seq=s,victim=v,cursor=c,dist=dist,n_resident=nr))
    return out

def con(pat):
    m = re.search(pat, CON.read_text()); return m.group(1) if m else None

def main():
    seq, groups = load_declared()
    L = len(seq)
    ids = load_reftrace()
    pts = load_policytrace()
    lab = lambda c: f"{c}({groups.get(c,'?')})"

    print("=== Phase 0 cursor diagnostic (UNMODIFIED code) ===\n")
    print(f"declared sequence ({L}): " + " ".join(lab(c) for c in seq))
    print(f"total notified refs in reftrace: {len(ids)}")
    print(f"n_decoded={con(r'n_decoded=(\d+)')}  layer_transitions={con(r'layer_transitions=(\d+)')}  "
          f"notify_layers={con(r'notify_layers=(\d+)')}\n")

    # distinct notified chunk ids, and which declared chunks are NEVER notified
    seen = sorted(set(ids))
    never = [c for c in seq if c not in seen]
    print("--- which declared chunks get a consumption signal? ---")
    print(f"  notified chunk ids: {seen}")
    if never:
        print(f"  *** declared chunks with NO consumption signal: {[lab(c) for c in never]}")
    else:
        print("  every declared chunk is notified at least once")
    print()

    # per-token period: detect by finding the repeat unit
    print("--- item 2 (GATE): notified order vs declared sequence ---")
    seq_no_never = [c for c in seq if c not in never]
    P = len(seq_no_never)
    print(f"  declared sequence minus never-signalled chunks = {P} entries: {[lab(c) for c in seq_no_never]}")
    ngroups = len(ids)//P
    order_ok = True
    for g in range(ngroups):
        segpos = ids[g*P:(g+1)*P]
        # align: the stream may start mid-sequence; rotate declared-minus-never to match seg[0]
        if segpos[0] in seq_no_never:
            r = seq_no_never.index(segpos[0])
            rot = seq_no_never[r:] + seq_no_never[:r]
        else:
            rot = seq_no_never
        match = (segpos == rot)
        if g < 3 or not match:
            print(f"  token {g}: {'MATCH (declared order, minus never-signalled)' if match else '*** MISMATCH ***'}")
            if not match:
                print(f"     got: {segpos}")
                print(f"     exp: {rot}")
                order_ok = False
    rem = len(ids) - ngroups*P
    if rem: print(f"  trailing partial token: {ids[ngroups*P:]} ({rem} refs)")
    # The GATE question: is the *order* of signalled chunks the declared order?
    print(f"  => signalled-chunk ORDER matches declared order: {'YES' if order_ok else 'NO'}")
    print(f"  => but the mapping is INCOMPLETE: {len(never)} declared chunk(s) never signalled" if never else "  => mapping complete")
    print()

    print("--- item 4 (GATE): policy cursor behaviour ---")
    cursor_ok = True
    if not pts:
        print("  no eviction records.")
    else:
        pos_of = {c:i for i,c in enumerate(seq)}
        cur_idx = [pos_of.get(r['cursor'], -1) for r in pts]
        bad = sorted({pts[i]['cursor'] for i,x in enumerate(cur_idx) if x < 0})
        if bad:
            print(f"  *** cursor at chunk(s) not in declared seq: {bad}"); cursor_ok = False
        wraps = jumps = 0
        for a,b in zip(cur_idx, cur_idx[1:]):
            if a<0 or b<0: continue
            if b < a:
                if b <= a - (L//2): wraps += 1
                else: jumps += 1
        print(f"  {len(pts)} evictions; cursor declared-idx head: {cur_idx[:30]}")
        print(f"  wrap-around steps: {wraps}; non-wrap backward JUMPS: {jumps}")
        if jumps: print("  *** cursor makes non-wrap backward jumps"); cursor_ok = False
        # never-signalled chunk 2: cursor should never sit at its declared position (0)
        if never:
            npos = [pos_of[c] for c in never]
            hit = [i for i in cur_idx if i in npos]
            print(f"  cursor sat at a never-signalled chunk's declared position: {len(hit)} times (expect 0)")
    print(f"  => cursor tracks consumption order: {'YES' if cursor_ok else 'NO (jumps)'}")
    print()

    gate = order_ok and cursor_ok and not never
    print("=== GATE VERDICT ===")
    if gate:
        print("PASS")
        sys.exit(0)
    print("FAIL -- STOP. The notified mapping is incomplete: at least one declared chunk")
    print("never receives a consumption signal. This is a node-name mismatch in eval_cb,")
    print("NOT the node_layer() trailing-digit bug the spec hypothesised, and NOT fixable")
    print("by the spec's proposed 'require blk.' guard. Report and stop; do not apply the")
    print("three fixes on top of a broken mapping.")
    sys.exit(1)

main()
