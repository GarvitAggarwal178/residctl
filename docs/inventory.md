# REPOSITORY INVENTORY — analysis only, move nothing

Work in `/root/residctl/`.

**This is an inventory pass. Do not move, rename, delete, `git rm`, or reorganise
anything. Do not edit `.gitignore`. Do not commit anything except the inventory
report itself at the end.** The restructure happens in a later session, against
this report.

**§0 non-negotiable rules apply**: no fabricated numbers, no guessing at file
purpose when it can be checked, report contradictions rather than resolving them.

Estimated: 90 minutes.

---

## STEP 0 — Safety

Before anything else:

```
git status
git tag pre-restructure && git push origin pre-restructure
```

Confirm the tree is clean and the tag pushed. If the tree is dirty, list what is
uncommitted and **stop** — report it and wait.

---

## STEP 1 — Raw census · box: 20 min

Produce, into `results/inventory/raw/`:

1. **`all_files.csv`** — every file under `/root/residctl/` excluding `.git/`.
   Columns: path, size_bytes, extension, mtime, `git_tracked` (yes/no),
   `in_gitignore` (yes/no).
2. **`repo_size.txt`** — total working-tree size; total `.git/` size; the
   **30 largest tracked files** with sizes; the 30 largest untracked files.
3. **`git_history_size.txt`** — the 20 largest objects ever committed, via
   `git rev-list --objects --all` piped through `git cat-file --batch-check`.
   **This is how we find out whether the model or a build artifact was ever
   committed, which history alone hides.**
4. **`file_type_summary.csv`** — count and total size grouped by extension.

Report the headline numbers in the final report: working tree size, `.git` size,
tracked file count, untracked file count.

---

## STEP 2 — Classify every file · box: 30 min

Classify **every** file into exactly one bucket. Where the bucket is not obvious
from path and extension, open the file and check. Do not guess — if you cannot
determine a file's purpose after looking, put it in `UNKNOWN` and say so.

Buckets:

| Bucket | Meaning |
|---|---|
| `SOURCE` | The mechanism itself: `src/*.c`, `*.h`, Makefile, the llama.cpp patch |
| `TEST` | T-1..T-7 harness, unit tests, correctness scripts |
| `SWEEP_SCRIPT` | One-off run/sweep/analysis scripts for a specific campaign |
| `TOOL` | Reusable instruments: figure generators, trace analysers, solvers |
| `SPEC` | `MECHANISM_SPEC.md`, `CLAUDE.md`, work-package and campaign specs |
| `REPORT` | A written report of findings — campaign/phase/session reports |
| `DATA_LIVE` | Raw data (CSV/log/trace) that backs a number in a **current, non-superseded** claim |
| `DATA_DEAD` | Raw data backing only **superseded** numbers (see `PROJECT_STATE.md` §6) |
| `DATA_ORPHAN` | Raw data referenced by **no** report or claim |
| `FIGURE` | Generated figures and their backing CSVs |
| `DELIVERABLE` | `WRITEUP_PACKAGE.md`, `CLAIMS.md`, `PROJECT_STATE.md`, final tables |
| `BUILD` | Compiled binaries, `.o`, build dirs — should never be tracked |
| `THIRD_PARTY` | The llama.cpp clone and anything under it |
| `MODEL` | `models/*.gguf` and anything of that size class |
| `SCRATCH` | `scratch/`, temp files, `nohup.out`, editor leftovers |
| `UNKNOWN` | Purpose could not be determined after inspection |

Output `results/inventory/classification.csv`: path, bucket, size, `git_tracked`,
and a one-line reason for anything not obvious from its path.

Report per-bucket counts and total sizes.

---

## STEP 3 — Citation map · box: 25 min · THE IMPORTANT ONE

This is what decides what stays.

For every file in `DATA_LIVE`, `DATA_DEAD`, `DATA_ORPHAN`, `FIGURE`, and
`REPORT`: search **every** `.md` file in the repo for references to it, by
filename and by path.

Produce `results/inventory/citation_map.csv`: data_file, cited_by (list of
documents), citation_count.

Then produce two lists explicitly:

- **`orphans.txt`** — every data file cited by nothing. For each, state your best
  determination of which campaign produced it (from mtime and path) and whether
  it is plausibly a superseded intermediate.
- **`load_bearing.txt`** — every data file cited by `WRITEUP_PACKAGE.md`,
  `CLAIMS.md`, `PROJECT_STATE.md`, or the final-session reports. **These are the
  files that must survive any restructure, wherever they end up.**

Cross-check `load_bearing.txt` against `WRITEUP_PACKAGE.md` §1 — every source
cell named there must resolve to a file that exists. **Report any citation that
points at a file which is not on disk.** A dangling citation in the writeup
package is worse than a messy repo.

---

## STEP 4 — Bloat and hygiene · box: 15 min

Report explicitly, each as a yes/no with evidence:

1. **Is the GGUF model tracked or in history?** Check both HEAD and
   `git_history_size.txt`. If it is in history, the repo carries ~2 GB
   permanently and that is the one case where a history rewrite is warranted.
2. **Is the llama.cpp clone tracked?** How large, how many files, and does it
   contain its own `.git`?
3. **Are compiled binaries, `.o` files, or build directories tracked?**
4. **Is anything under `scratch/` tracked?**
5. **What does `.gitignore` currently contain**, and what is obviously missing?
6. **Duplicate files** — same content, different paths. Use content hashes over
   all files above 1 KiB. Report every duplicate set.
7. **Near-duplicate reports** — documents whose titles or headings suggest one
   supersedes another (e.g. `HARNESS_REPORT.md` vs `HARNESS_REPORT_V2.md`).

---

## STEP 5 — Propose a structure · box: 20 min · PROPOSAL ONLY

Propose a target layout. **Do not implement it.** This is for review.

Two destinations:

- **`REPO`** — what belongs on GitHub. Someone landing on the repo should
  understand what was built, see the results, and be able to reproduce them.
- **`ARCHIVE`** — everything else, moved to `/root/residctl-archive/`, organised
  by campaign, never deleted, not on GitHub.

For the REPO side, propose a concrete tree. Suggested shape, adapt it to what is
actually there:

```
README.md              — what this is, the headline result, how to run it
docs/
  SPEC.md              — the mechanism specification
  RELATED_WORK.md
  PROJECT_STATE.md
src/                   — mechanism, tests, reusable tools
results/
  FINDINGS.md          — the consolidated results document
  CLAIMS.md
  figures/
  data/                — only the CSVs backing current claims
scripts/               — only scripts someone would re-run
```

For each proposed destination, list which classification buckets go there and
roughly how many files and how much size.

Then flag every **judgement call** you would want a human to make — anything
where you can see an argument both ways. Examples of the kind of thing to flag:

- Whether superseded reports stay in the repo (audit trail) or move to archive
  (clarity). The superseded-numbers discipline is part of this project's
  contribution, which cuts toward keeping them.
- Whether one-off sweep scripts are reproducibility evidence or clutter.
- Whether the eight campaign reports get consolidated into one findings document
  or kept individually.
- Whether `DATA_DEAD` follows its report or archives separately.

**Do not decide these.** List them with the argument on each side.

---

## REPORT

Write `results/inventory/INVENTORY_REPORT.md`:

```
# Repository Inventory

## Headline numbers
Working tree size, .git size, tracked/untracked counts, largest offenders.

## Bucket summary
Table: bucket, file count, total size, tracked count.

## Citation map summary
How many data files are load-bearing, how many dead, how many orphaned.
Any dangling citation in WRITEUP_PACKAGE.md — listed individually.

## Bloat and hygiene
The seven checks from Step 4, each answered yes/no with evidence.

## Proposed structure
The tree, with bucket → destination mapping and file counts.

## Judgement calls for human decision
Each with the argument on both sides. Numbered, so they can be answered
by number.

## What I could not classify
Every UNKNOWN file, with what I checked.
```

Keep the raw census files under `results/inventory/raw/`. Commit only
`results/inventory/` and nothing else.

**Move nothing. Delete nothing. Change no `.gitignore`.** The restructure is a
separate session against this report.