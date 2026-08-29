# LIVELOCK FIX — Blocker Log

---

## BLOCKER 1 (Phase 0 GATE) — token_embd has no consumption signal

**When:** Phase 0, before any fix.

**Finding:** on unmodified code, `token_embd.weight` (chunk 2, ~175 MiB,
declared position 0) is never passed to `pager_notify_access()`. The other 39
declared chunks are signalled in exact declared order, once per token; the
cursor is monotone with clean per-token wraps. Cause: `wp2_gen.cpp:eval_cb()`
matches the embedding node as `"inp_embd"`, but this llama.cpp names it
`"embd"` (the `ggml_get_rows(token_embd, inp_tokens)` result; `inp_embd` is a
non-path input leaf the eval callback never visits). Evidence:
`phase0_node_evidence.txt` (`inp_embd` count = 0).

**Spec's pre-decided answer for this branch:** "Do not proceed on a broken
mapping … Report and stop." Followed. The three fixes are NOT applied.

**The spec's proposed remedy is wrong:** requiring the node name to contain
`blk.` would make `node_layer()` return −1 for every compute-graph node (they
are named `attn_norm-0` etc.; `blk.` is GGUF weight-tensor naming only) and
suppress all 36 layer signals.

**Fix, when authorised (fourth defect, for A-14):**
`!strcmp(nm, "inp_embd") || !strcmp(nm, "embd")` in `eval_cb`, on the same
callback-pass choice Defect 2 makes. Re-run Phase 0, expect reftrace period 40
with chunk 2 leading each token. Then apply Defects 1–3.

Full write-up: `phase0_cursor_diagnostic.md`.

---

## NOTE 1 — `$?` is unreliable in inline `bash -lc` strings through this shell

**When:** Phase 0.

`$?` (and any `$var` the inner shell must expand) placed **literally in an
inline `wsl.exe -- bash -lc '…'` command** is expanded by the Git Bash tool
layer before `wsl.exe` runs — inside single quotes and quoted heredocs alike —
and reads `0` or empty. Verified: a Write-tool script on disk, run as
`bash -lc 'bash /path/script.sh'`, gives correct codes (`false`→1,
`timeout`→124, `python3 sys.exit(7)`→7).

**Impact:** Phase 3 sweep scripts detect a livelock via `rc=$?` == 124 after
`timeout … wp2_gen`. That logic must stay in on-disk scripts. `run_final_*.sh`
and `livelock_phase0.sh` are already on-disk; keep it that way for every new
sweep/verify script.

---

## NOTE 2 — correction to results/cleanup/phase1_deadlock_fix.md

That doc says arm E "re-reads the two large non-layer chunks (`token_embd`
175 MiB, `output` 243 MiB) on nearly every token." Phase 0 shows **`output`
(chunk 1) does get a consumption signal** (`result_output` matches); only
`token_embd` is signal-less. `output` was being thrashed for a different
reason — the `d = 1..seq_len` off-by-one (Defect 1) ranks even a just-signalled
chunk as furthest-future. Both end up thrashed; the mechanisms differ.
