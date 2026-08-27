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
