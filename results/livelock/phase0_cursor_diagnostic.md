# LIVELOCK FIX — Phase 0: cursor diagnostic

**Box:** 45 min. **GATE — run BEFORE any fix.** Machine exclusive (load 0.00
before; own run only; `pgrep cn-spike|iperf3|gate5` clean).

Run on **unmodified** code at commit `4ba98bf`. Real model
(`models/model.gguf`, qwen2.5-3b-instruct, 36 layers, 41 chunks, `declared_len
= 40`), arm D (`layer_order_declared`, prefetch on, depth 2, retention pinned),
r = 0.5 (B = 1003 MiB, `memory.max` = B + 128 MiB), `-n 8`, `--fetch-trace` /
`--policy-trace` via the config's `reftrace=` / `policytrace=` keys.

Scripts: `src/livelock_phase0.sh`, `src/livelock_phase0_analyze.py`.
Artifacts: `phase0_console.txt`, `phase0_inventory.txt`, `phase0_reftrace.bin`
(312 records), `phase0_policytrace.bin` (209 records), `phase0_node_names.txt`,
`phase0_node_evidence.txt` (+ `wp2_gen_dbg.cpp` / `build_dbg.sh` / `dbg_nodes.sh`
— the throwaway instrumented build used to dump graph-node names).

---

## VERDICT: **GATE FAILS — STOP.**

`pos` tracks the workload's real per-token consumption order **for 39 of the 40
declared chunks**. The 40th — `token_embd.weight` (chunk 2, ~175 MiB, declared
position 0) — **never receives a consumption signal at all.** The three fixes
must not be applied on top of an incomplete mapping.

This is **not** the failure mode the spec's GATE hypothesised (`node_layer()`
mis-parsing a trailing `-<digits>` on a non-layer node, making the cursor jump).
The layer mapping is exactly right. It is a **node-name mismatch** in
`wp2_gen.cpp:eval_cb()`, and **the spec's proposed remedy ("require the node
name to contain `blk.`") would break the mapping entirely** — see §4.

---

## What the diagnostic found

### Item 2 (GATE) — notified order vs declared sequence

The reference trace over 8 decode steps holds **312 references = 8 × 39**, not
8 × 40. Every decode step emits the identical 39-chunk sequence:

```
3(L0) 4(L1) 15(L2) 18(L3) 19(L4) 20(L5) 21(L6) 22(L7) 23(L8) 24(L9)
5(L10) 6(L11) 7(L12) 8(L13) 9(L14) 10(L15) 11(L16) 12(L17) 13(L18) 14(L19)
16(L20) 17(L21) 25(L21) 26(L22) 27(L23) 28(L24) 29(L25) 30(L26) 31(L27) 32(L28)
33(L29) 34(L30) 35(L31) 36(L32) 37(L33) 38(L34) 39(L35) 40(output_norm) 1(output)
```

This is **exactly `g_declared_seq` with chunk 2 (`token_embd`) removed**, in
declared order, once per token. So:

- **the ORDER of the signalled chunks is correct** — `node_layer()` parses the
  per-layer `-<il>` suffix correctly, layers are notified strictly in declared
  order, `output_norm` and `output` land last every token;
- **the mapping is INCOMPLETE** — `token_embd` (chunk 2) is signalled **zero
  times** in the whole run.

`notify_layers` (`RESIDCTL_STATS`) = 312, `n_decoded` = 8 → 39 notifies per
decode, confirming the reftrace count.

**The count check was load-bearing, not informational.** `notify_layers /
n_decoded` = 312 / 8 = **39**, not 40. That one-short result is what first
exposed the gap; item 2 then identified *which* chunk (2 / `token_embd`). The
spec framed the count as a non-gate; in practice it was the tell.

### Item 3 (NOT a gate) — `layer_transitions` vs `declared_len`

`layer_transitions` = 288 = 8 × 36. As predicted, this counts only `blk.N`
changes (`st->transitions++` in `eval_cb`, incremented on a layer-number
change) and equals `n_decoded × n_layers`. It is **structurally** below
`declared_len` = 40 (which also counts `token_embd`, `output_norm`, `output`)
and below the 39 notifies/decode (`token_embd` excluded). Not evidence of a
broken mapping — reported for completeness only.

### Item 4 (GATE) — policy cursor behaviour

209 eviction records in `phase0_policytrace.bin`. The cursor (`cursor_chunk`,
i.e. `seq[pos]`):

- **advances monotonically** through the declared sequence within each token;
- **wraps exactly 7 times** over the 8 decode steps (one clean wrap per token
  boundary), **0 non-wrap backward jumps**;
- **never once sits at chunk 2's declared position (0)** — because
  `lo_declared_on_access()` forward-searches from `pos+1` and chunk 2 is never
  the argument, so `pos` steps straight from the `output` slot (39) to the
  `L0` slot (1) every wrap, skipping slot 0 forever.

So the cursor tracks consumption order **for the chunks that are signalled** —
but `token_embd` is permanently outside the cursor's reach.

---

## Root cause

`wp2_gen.cpp:eval_cb()` routes non-layer nodes by exact name:

```c
} else if (!strcmp(nm, "inp_embd")) {   residctl_llama_notify_role(0); /* token_embd */
} else if (!strcmp(nm, "result_norm")) { residctl_llama_notify_role(1); /* output_norm */
} else if (!strcmp(nm, "result_output")){ residctl_llama_notify_role(2); /* output */
```

An instrumented build (`wp2_gen_dbg`, dumping every distinct node name
`eval_cb` sees — `phase0_node_evidence.txt`) shows, for this llama.cpp
(`third_party/llama.cpp` @ `4ba98bf`, qwen2 arch):

| role | name `eval_cb` matches | name actually emitted | fires? |
|---|---|---|---|
| output_norm | `result_norm` | `result_norm` | ✅ |
| output | `result_output` | `result_output` | ✅ |
| **token_embd** | **`inp_embd`** | **`embd`** | ❌ **never** |

`inp_embd` appears **0 times** in the callback stream. The first node of every
decode is named **`embd`** (the `ggml_get_rows(token_embd, inp_tokens)` result;
`llm_graph_context::build_inp_embd` only `cb()`s the name `inp_embd` onto the
*vector-embedding input leaf* at `llama-graph.cpp:2280`, which is not in the
token-generation path and is a leaf the scheduler's eval callback never visits).

`token_embd.weight` **is** still demand-faulted every token (the `get_rows`
read faults chunk 2 through the pager) — it just gets no `pager_notify_access`,
so the declared cursor never advances to it and `lo_declared_dist()` treats it
as far-future. Under the current `d = 1..seq_len` loop it is assigned the
maximum distance → top eviction victim → re-fetched every token. This is a
concrete component of the "re-reads the two large non-layer chunks every token"
mechanism recorded in `results/cleanup/phase1_deadlock_fix.md`.

**Correction to that earlier characterisation:** only `token_embd` lacks a
signal. `output` (chunk 1) *does* get one (`result_output` matches). Both were
being thrashed, but for different reasons — `token_embd` because it is never
signalled, `output` because the `d = 1..seq_len` off-by-one (Defect 1) makes
even a just-signalled chunk the furthest-future victim.

---

## Why this stops the session

1. **The spec's GATE is explicit:** "if the notified order does not match the
   declared sequence … **stop and report** … that is a different fix from the
   three above. Do not proceed on a broken mapping." The notified order does not
   match (chunk 2 absent from every token). This is a **fourth defect**.

2. **None of the three fixes address it.** Defect 1 (`d = 0` loop) cannot help a
   chunk the cursor never reaches. Defect 2 (act on the pre-compute callback
   pass) still won't match a node named `embd` against `"inp_embd"`. Defect 3 is
   unrelated. With all three applied, `token_embd` (175 MiB) would still be
   evicted-and-refetched every token — Phase 3 expectation 1 (arm E completes at
   r = 0.25) and expectation 2 (arm D reads fewer bytes) would be measured
   against a still-broken consumption model, and any result mis-attributed.

3. **It changes what the re-measurement means.** The spec's premise — "with
   Defects 1 and 2 both fixed, `pos` points at the chunk about to be read and
   its distance is 0 by construction; `protect_current` should then be
   unnecessary" — only holds once *every* declared chunk, `token_embd` included,
   is signalled. Whether `protect_current` becomes redundant, and how Claim 10
   should finally be framed, both depend on the fourth fix being in.

4. **The fourth fix is a real design decision, not a mechanical rename.** `embd`
   fires the eval callback on both the `ask` (pre-compute) and `eval` (post)
   passes; picking the pass is the same decision Defect 2 makes for layers. And
   `embd` is the *get_rows result*, one op downstream of the `token_embd.weight`
   read — near-simultaneous in practice, but the timing should be reasoned about
   deliberately, the way Defects 1–3 were, not patched in ad hoc under an
   unattended run.

---

## The fix, when authorised

Minimal: in `wp2_gen.cpp:eval_cb()`, match the embedding node by its real name.
Robust against llama.cpp version drift: match **either** name.

```c
} else if (!strcmp(nm, "inp_embd") || !strcmp(nm, "embd")) {
    st->last_layer = -1;
    residctl_llama_notify_role(0);
}
```

Then re-run Phase 0 and confirm the reftrace period is 40 (not 39), chunk 2
leads every token, and the cursor sits at declared position 0 once per token.
Only then apply Defects 1–3 and proceed to Phases 1–5. Fold the fourth fix into
spec amendment A-14 alongside the other three.

**The spec's proposed `node_layer()` guard ("require `blk.`") must NOT be
applied.** Compute-graph nodes are named `attn_norm-0`, `ffn_gate-0`, `l_out-0`,
… (`ggml_format_name(cur, "%s-%d", name, il)`, `llama-context.cpp:2488`); the
string `blk.` appears only in **GGUF weight-tensor** names (`blk.0.attn_norm.weight`).
Requiring `blk.` would make `node_layer()` return −1 for every graph node and
suppress **all 36 layer signals** — replacing a 1-chunk gap with a 37-chunk one.
`residctl_llama_notify_layer()` already bounds-checks `layer >= g_n_layers`
(`residctl_llama.c:409`), so a stray large trailing index is already harmless;
and this diagnostic shows no such stray index exists (`node_NNN` ops use a `_`
separator, which `node_layer()` correctly rejects at the `nm[i-1] != '-'` check).

---

## Decision required (this is a fork, not a status report)

**Option (a) — recommended.** Add the fourth fix
(`!strcmp(nm,"inp_embd") || !strcmp(nm,"embd")` in `eval_cb`, on the same
callback-pass choice Defect 2 makes), re-run Phase 0, confirm the reftrace
period is 40 with chunk 2 leading every token and the cursor sitting at declared
position 0 once per token, then apply Defects 1–3 and run Phases 1–5. Record all
four under spec amendment A-14. Rationale: without it the spec's own premise —
"`pos` points at the chunk about to be read and its distance is 0 by
construction" — is false for `token_embd`, the single largest chunk in the
region (~175 MiB), and Phase 3's headline expectations (arm E completes at
r = 0.25; arm D reads fewer bytes) would be measured against a consumption model
that still thrashes it.

**Option (b).** Proceed with the three fixes exactly as written, accept that
`token_embd` keeps thrashing, and carry it as a documented confound on every
Phase 3 number. Cheaper, but the re-measurement then can't cleanly answer
"is `protect_current` now redundant?" — the question the session exists to
settle.

## Note for Phase 2, whichever option is chosen

Defect 1's `d = 0` loop is correct **only on the real-model path once Defect 2
(and the fourth fix) land**. On the synthetic `--consumption-signal all-threads`
path, `replay.c:lookahead_mark_done()` fires `pager_notify_access()` when
`completed[s] == n_threads` — i.e. **after** every driver thread has read *and*
computed over the chunk. There `seq[pos]` is genuinely just-consumed, its next
use is a full lap away, and the current `d = 1..seq_len` loop returning
`seq_len` for it is **correct Belady**. Defect 1's fix makes it distance 0 —
protecting the chunk that should be the top eviction victim — and Defect 2 does
not reach that path. So **Phase 2 expectation 3 ("128 MiB D/OPT improves on
1.08 / 1.04 / 1.00") is likely to come out backwards.** Per the spec's "do not
soften a result that comes back worse," that would be a legitimate finding
(Defect 1 applied without its timing partner on a path Defect 2 can't reach),
not a bug — but the user may want to rewrite that expectation before
authorising.

## Environment finding — `$?` is unreliable through the shell wrapper

Empirically verified this session: when `$?` (or any `$var` the inner shell
should expand) appears **literally in an inline `wsl.exe -- bash -lc '...'`
string**, the Git Bash tool layer expands it *before* `wsl.exe` runs — even
inside single quotes and quoted heredocs — yielding `0` or empty. A script
**written to disk** (Write tool) and run as `bash -lc 'bash /path/script.sh'`
behaves correctly: `false` → 1, `timeout` kill → 124, `python3 sys.exit(7)` → 7.

**Impact on Phases 3+:** every sweep script records `rc=$?` after
`timeout … wp2_gen` to tell a completed run from a timed-out one (rc 124) —
which is how a livelock is detected. Any such logic MUST live in an on-disk
script, never in an inline command. The existing `run_final_phase*.sh` and this
session's `livelock_phase0.sh` are on-disk and safe. Also logged in
`results/livelock/BLOCKERS.md`.

## Machine state

Load average 0.00 → 0.66 (own run) → settled. No foreign workload before or
after. `stat_fetching_timeout = 0`, `infeasible = 0` on the diagnostic run
(rc = 0, 8 tokens produced: `13 576 1156 12677 311 5545 264 11580`).
