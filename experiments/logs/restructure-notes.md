# Repository restructure — notes

2026-08. Renamed every file by content, rewrote the synthesis documents,
archived process noise out of git. Safety tag `pre-restructure` was pushed
before the inventory pass. ~20 checkpoint commits from `86aa892`, plus a
follow-up pass that archived two source documents that had been removed without
being copied first (see "Archived", below).

## What moved where

| from | to | how |
|---|---|---|
| `results/HARNESS_REPORT.md`, …, `results/livelock/phase3_real_model.md` (30 reports) | `experiments/NN-<content>.md` | `git mv`, verbatim |
| `results/**/*.csv` behind current claims | `results/data/<content>.csv` | `git mv` + rename |
| closed-campaign `task_*` / `item*` CSVs | `results/data/historical/` | `git mv` |
| `results/overnight/figures/*` | `results/figures/NN-<content>.{png,csv}` | `git mv` |
| `src/run_*.sh` (20 current) | `scripts/run-<what>.sh` | `git mv` |
| `src/run_*.sh` (53 one-offs) | `scripts/historical/` | `git mv` |
| `results/overnight/{analyze_*,make_wp3}.py`, `src/livelock_*_analyze.py` | `tools/` | `git mv` |
| raw `*_log.txt`, `*.trace`, gate `.tok` | `experiments/logs/` | `git mv` |
| `docs/MECHANISM_SPEC.md` | `docs/02-design.md` | **rewritten** (current-state; 14 amendments → `design-history.md`) |
| `results/PROJECT_STATE.md` | `results/findings.md` + `results/superseded.md` + `docs/05-limitations.md` | **split**; §4/§5 → `design-history.md` |
| `CLAUDE.md` (narrative) | `docs/project-log.md` | **rewritten** as a readable narrative; `CLAUDE.md` kept as the agent working file |
| `results/cleanup/RELATED_WORK.md` | `docs/04-related-work.md` | `git mv`, unchanged |
| `results/overnight/CLAIMS.md` | `results/claims.md` | `git mv` |
| `results/final/WRITEUP_PACKAGE.md` | `results/writeup-package.md` | `git mv` |

New: `README.md`, `docs/01-problem.md`, `docs/03-methodology.md`,
`docs/06-reproduce.md`, and the four index READMEs.

## Archived (not on GitHub) — `/root/residctl-archive/`

56 files, sha256-verified against `MANIFEST.csv`, then `git rm`'d. Process noise,
plus the two rewritten-from source documents kept verbatim:

| category | count | substance preserved in |
|---|---|---|
| `run-configs/` — per-run `.cfg` | 14 | regenerable from `scripts/` |
| `process-logs/` — `*_console.txt`, `*_supervisor.log` | 11 | the experiment records |
| `session-prompts/` — per-session agent instructions | 7 | `docs/project-log.md` |
| `session-summaries/` — wrap-ups + blocker trackers | 9 | `project-log.md`, `05-limitations.md`, `superseded.md` |
| `source-snapshots/` — `src/` copies committed into `results/overnight/` | 12 | `src/` is authoritative |
| `census/` — the 6.8 MB raw all-files inventory | 1 | not load-bearing |
| `rewritten-sources/` — `MECHANISM_SPEC.md`, `PROJECT_STATE.md` full text | 2 | rewritten into `docs/02-design.md` + `design-history.md` / split into `findings.md` + `superseded.md` + `05-limitations.md` — but the long-form amendment arguments only survive in full here |

The `rewritten-sources/` pair was added in a follow-up pass: the first restructure
run `git rm`'d both after their successors were committed, without first copying
them to the archive (the spec's "copy + hash-verify before delete" rule). They
were recovered from `git show <commit>~1:<path>` — byte-identical to what was
removed — archived, and added to `MANIFEST.csv` (integrity re-check: 56/56).
`docs/design-history.md` now states where the full spec lives and that older
`MECHANISM_SPEC.md §N` section citations resolve against it.

## Verification — all seven checks

| # | check | result |
|---|---|---|
| 1 | `make clean && make` | **PASS** — every synthetic binary builds |
| 2 | T-1…T-7 with `--eager-reconcile` | **PASS**, `mismatches = 0` (`scripts/run-correctness-harness.sh`, `run-storm-t6-t7.sh`) |
| 3 | no dangling citations | **PASS** for `results/claims.md` + `results/findings.md` (`experiments/logs/restructure/check_refs.py`); 14 residual warnings, all in frozen experiment records or forward-refs to this file |
| 4 | fresh-clone test (`git clone /root/residctl /tmp/clone-test`) | **PASS** — `make` succeeds, `docs/06-reproduce.md` present, no tracked file > 5 MB (the 6.8 MB census file was archived to fix this), 331 files / ~14 MB |
| 5 | read-the-README test | **PASS** — from `README.md` alone a reader gets: what residctl is, the headline (kernel flat / pager 2.5×, LRU ~100 % miss, D/OPT 1.08–1.13, byte-identical gate), the reading order, and where the evidence is |
| 6 | archive integrity | **PASS** — 56/56 files match their recorded sha256 (54 from Phase 2 + the 2 rewritten-sources added in the follow-up pass) |
| 7 | repo size | tracked content 13.9 MB (was ~5 MB pre-restructure + the reports that stayed); `.git` 12 MB; working tree 5.0 GB is entirely untracked regenerables (model, pattern files, `third_party/llama.cpp/`), absent from any clone |

## Deviations from the restructure spec

- **Experiment numbering**: the spec's table stops at 17. Campaign sub-phases it
  omits got topic-grouped sub-letters (`09b/09c/09d`, `12b/12c`, `17b/17c`)
  rather than new mainline numbers, so the final/livelock session boundaries the
  spec fixes (`15–17`, `18–21`) hold. Rationale in the rename map's README.
- **Session prompt files archived** (the user asked that prompt files not be
  committed). The spec's Phase 2 anticipated only configs/logs/snapshots.
- **The 5 GB of untracked regenerables were left in the working tree** rather
  than deleted — they are gitignored and absent from every clone, so deleting
  them only reclaims local disk, and keeping `third_party/llama.cpp/` avoids a
  slow re-clone. `docs/06-reproduce.md` covers regenerating them.
- **`docs/claeanup/restructure.md`** (the spec for this work) was never tracked
  and stays out of git, per the same user instruction.
