# WP2 — llama.cpp integration

## Verdict

**NOT STARTED — stopped at the Phase 2.0 GATE.** The required model file
`/root/residctl/models/model.gguf` is absent (the `models/` directory does
not exist). Per `WP2.md` Phase 2.0: "If it is absent: write that to
`results/overnight/BLOCKERS.md` and skip the entire work package. Do not
attempt to download one ... Move to WP3."

The correctness gate (Phase 2.2), arm D vs arm A on bytes (Phase 2.3), and
arm E vs arm D (Phase 2.3) — the three questions WP2's verdict was to
answer — are all **unanswered**. WP2 exists to validate every prior
finding against a real, non-synthetic access pattern; that validation did
not happen this session.

## What was checked

```
$ ls -la /root/residctl/models/model.gguf
ls: cannot access 'models/': No such file or directory
$ find /root/residctl -iname '*.gguf'
(no output)
```

No GGUF anywhere under the project tree. `llama.cpp` is present at
`/root/spike/src/llama.cpp` (commit `69bf6437…`) and could be built, but
Phase 2.0 is explicit that a build with no model to run is not worth the
time-box, and every later phase depends on the model.

## Phases

| Phase | Status | Note |
|---|---|---|
| 2.0 Model and build | **not started** | GATE: model absent |
| 2.1 Chunk table from real tensors | not started | needs the GGUF tensor inventory |
| 2.2 Wire the pager into the loader | not started | — |
| 2.3 Measurement | not started | — |
| 2.4 Compare synthetic to real | not started | — |

## To run WP2

Place a 1.5–3 GiB GGUF at `/root/residctl/models/model.gguf` and run
`docs/overnight/WP2.md` end to end. This is item 1 of the "what the next
session should do first" list in `OVERNIGHT_SUMMARY.md` — it is the single
biggest open structural question in the project and the reason the WP order
put it second (long and risky) rather than last.

See `results/overnight/BLOCKERS.md` — BLOCKER 1.
