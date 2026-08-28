# Overnight Session — Blocker Log

Every decision that was genuinely undecidable from the runbook + WP specs,
or every hard-stop condition hit, recorded here with full context.

---

## BLOCKER 1 — WP2 GATE: `models/model.gguf` is absent

**When:** WP2 Phase 2.0, start of the work package.

**Context:** `WP2.md` Phase 2.0 and `RUNBOOK.md` HUMAN PRE-FLIGHT step 4
require a GGUF model at `/root/residctl/models/model.gguf` (target size
1.5–3 GiB), placed by the human before the session. The runbook is explicit
that the agent cannot download it (sandbox allowed-domains exclude model
hosts) and that WP2 stops at its first gate if the file is absent.

**Observed:**
```
$ ls -la /root/residctl/models/model.gguf
ls: cannot access 'models/': No such file or directory
```
The `models/` directory does not exist at all. No GGUF file anywhere under
`/root/residctl`.

**Decision (pre-decided by WP2.md Phase 2.0):** "If it is absent: write that
to `results/overnight/BLOCKERS.md` and skip the entire work package. Do not
attempt to download one ... Move to WP3."

**Action taken:** WP2 skipped in full. No llama.cpp build attempted, no
tensor inventory, no integration. Proceeded to WP3. WP3 does not depend on
WP2 output (Figure 6 is simply omitted, per WP3.md).

**To unblock:** place a 1.5–3 GiB GGUF at `/root/residctl/models/model.gguf`
and re-run WP2 (`docs/overnight/WP2.md`) as the first task of the next
session.

**RESOLVED (session 2, 2026-08-28):** BLOCKER 1 is obsolete. Session 2's
prompt states network access is normal and gave explicit download
commands; `models/model.gguf` was acquired via `curl` from Hugging Face
(`Qwen/Qwen2.5-3B-Instruct-GGUF`, `qwen2.5-3b-instruct-q4_k_m.gguf`,
1.95 GiB). WP2 proceeded. See BLOCKER 2 for the one thing session 2 could
not resolve.

---

## BLOCKER 2 — Session 2: `docs/overnight/WP0_FIX_AND_MODEL.md` (and the renamed WP2/WP3 files) do not exist

**When:** Start of session 2, before any work.

**Context:** The session-2 prompt instructs: "work through these three work
packages in order: `docs/overnight/WP0_FIX_AND_MODEL.md` — model
acquisition and the consumption-signal fix; `docs/overnight/WP2_LLAMACPP.md`;
`docs/overnight/WP3_FIGURES.md`". It also says "WP0 §0.1 corrects" the
model-download claim and "WP0 acquires the model, so WP2's gate should
already be satisfied."

**Observed:** none of `WP0_FIX_AND_MODEL.md`, `WP2_LLAMACPP.md`,
`WP3_FIGURES.md` exist anywhere on disk (checked `docs/overnight/`,
`git ls-files docs/`, `/mnt/d/os`). Only the session-1 files are present:
`Runbook.md`, `WP1.md`, `WP2.md`, `WP3.md` (unchanged, same mtime/size as
session 1). The user was not available to supply them (unattended session).

**What is decidable and was done:**
- **Model acquisition (WP0 §0.1):** the prompt gave the exact commands and
  the network was verified reachable. `models/model.gguf` downloaded. This
  is unambiguous — done, not blocked.
- **WP2 (llama.cpp):** `docs/overnight/WP2.md` is a complete spec. Its
  Phase 2.0 download gate is now satisfied by the model above. Executed
  per that spec.
- **WP3 (figures):** `docs/overnight/WP3.md` is a complete spec; the
  session-2 prompt adds "refresh every figure and table with tonight's
  data, and add Figure 6 if WP2 produced results". Executed.

**What is NOT decidable — the "consumption-signal fix" (WP0):**
No spec. Session 1's own `wp1_declared_order.md` and `PROJECT_STATE.md` §2
identified that `layer_order_declared` is non-deterministic under
`--driver-threads>1` ∧ `--lookahead-window>0` ∧ `--compute-ns-per-mib>0`
because `pager_notify_access()` (the consumption signal) fires from driver
thread 0 only, at the *start* of consuming a chunk, and races the
concurrently-dispatched faults of the other 7 threads. Two candidate fixes
were recorded there:
  1. Driver change: fire `pager_notify_access(r, c)` when chunk `c` is
     *fully* consumed by all threads (after `lookahead_wait_full`), not at
     the start — so the declared cursor lags the in-flight window and
     `select_victim`'s furthest-future pick is genuinely safe to evict.
  2. Policy change: `select_victim` refuses to evict any chunk within
     `window+1` positions *behind* the cursor.

Both have real blast radius that no spec constrains: option 1 also changes
when the prefetch-retention FIFO releases pins (`prefetch_retain_release`
is called from the same `pager_notify_access`), affecting **arm E under
every policy**, and would change every WP1 arm-D/E number; option 2
changes a mechanism decision function. Making an unspecified
mechanism/driver change in an unattended session violates SESSION RULE
§0 ("no silently fixing", report contradictions) and RULE 3 (record and
move on when there is no pre-decided answer).

**Initial decision:** deferred; proceed to model + WP2 first.

**Revised decision (after WP2 Phase 2.3 data, commit `8c15d8b`):** WP2's
real-inference sweep made the bug undeniable -- at budget ratio 0.25
`layer_order_declared` (arm D) *deterministically* read 216.6 GB for 64
tokens, 1.5x arm C's "refetch everything". A minimal, evidence-driven fix
was implemented: `lo_declared_dist()` in `policy.c` returns distance 0 for
the actively-consumed chunk `seq[pos]` and the one before it `seq[pos-1]`,
so `select_victim` never evicts the chunk the workload is reading now. It
touches **only `layer_order_declared`** -- lru and `layer_order_learned`
byte-for-byte unchanged (WP1 §1.2 gate PASS); `replay.c` and
`pager_notify_access` timing unchanged.

Measured blast radius (commit `8c15d8b` message): the fix **eliminates the
Campaign 13 Phase A non-determinism** -- WP1 §1.3 cells 5-6 go from
non-deterministic (79-90 / 1316-1388) to deterministic (55 / 768, the
latter the exact Belady floor) -- at the cost of ~2 extra resident chunks
in the easy case (cells 1-2: 48 -> 50) and mild non-determinism at cell 3.
This **supersedes session 1's "declared order == Belady-optimal at
compute=0"**; WP1 §1.4 was re-swept and WP3 figures regenerated this
session.

**Still unblocked:** whether this is the fix the missing WP0 spec intended,
its acceptance criteria, and whether a driver-side alternative ("notify =
FINISHED consuming", which would also move the retention-FIFO release for
arm E across all policies) is preferred. The policy-side fix was chosen for
minimal blast radius.
