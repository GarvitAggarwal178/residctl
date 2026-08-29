# Repository Inventory

Analysis only. **Nothing was moved, renamed, or deleted; `.gitignore` was not
touched.** Raw census in `results/inventory/raw/`; the scripts that produced it
in `results/inventory/scripts/`. Safety tag `pre-restructure` pushed before this
pass. The restructure is a separate session against this report.

---

## Headline numbers

| | |
|---|---|
| Working tree (excl. `.git`) | **5.0 GB** |
| `.git` | **8.3 MB** (`.git/objects` 8.1 MB) |
| Tracked files | **310** |
| Untracked files | **51,937** (≈ 51,900 are inside `third_party/llama.cpp/build/tools/ui/…/node_modules`) |
| Largest tracked file | `experiments/logs/final__phase1_equal_budget_log.txt` — 4.0 MB |
| Largest tracked class | `*_log.txt` run logs — ~13 MB across ~40 files |
| Largest untracked | `scratch/pattern_2g.bin` 2.1 GB, `models/model.gguf` 2.1 GB, `third_party/llama.cpp/` 0.93 GB (0.76 GB of it is `build/`) |

**The 5 GB working tree is entirely regenerable:** `scratch/pattern_2g.bin` +
`models/model.gguf` (≈ 4.2 GB, reconstructed by `gen_pattern` / a re-download at
a known sha256) and `third_party/llama.cpp/build/` (0.76 GB, a CMake rebuild).
The tracked repo is **~5 MB of source + reports + figures**.

---

## Bucket summary

Rolled up (`third_party/` counted as one line):

| bucket | files | size | tracked |
|---|---|---|---|
| SOURCE (`src/*.c/.h/.cpp/Makefile/.patch`) | 50 | 0.4 MB | 50 |
| TEST (`src/test_*.c` + the T-1..T-7 harness scripts) | 20 | 2.1 MB | 9 (the `.c`; 11 compiled test binaries untracked) |
| SWEEP_SCRIPT (`src/run_*.sh` + supervisors) | 56 | 0.2 MB | 54 |
| TOOL (`make_wp3.py`) | 1 | 0.02 MB | 1 |
| SPEC (`docs/`, `CLAUDE.md`, `.gitignore`) | 9 | 0.2 MB | 9 |
| REPORT (`results/**/*.md`) | 39 | 7.1 MB | 34 |
| DATA (CSV / log / trace / cfg under `results/`) | 120 | 13.2 MB | 112 |
| FIGURE (`results/overnight/figures/*`) | 14 | 1.3 MB | 14 |
| DELIVERABLE (`WRITEUP_PACKAGE`, `CLAIMS`, `PROJECT_STATE`, final tables) | 11 | 0.1 MB | 11 |
| BUILD (`*.o`, compiled binaries, `src/wp2_obj/`) | 21 | 1.7 MB | **0** |
| SCRATCH (`scratch/`) | 1380 | 23.6 MB (+ the 2 GB pattern file) | **0** |
| MODEL (`*.gguf`, `scratch/pattern_*.bin`, `scratch/*model*.bin`) | 4 | 4.09 GB | **0** |
| THIRD_PARTY (`third_party/llama.cpp/` rolled up) | ~52,000 | 0.93 GB | **0** |
| UNKNOWN | 0 | — | — (all 16 initial UNKNOWNs identified — see below) |

### The 16 initial UNKNOWNs, resolved

- `.gitignore`, `results/.gitkeep` — repo hygiene (→ SPEC / keep).
- `tools/analyze-wp1-sweep.py`, `analyze_wp2.py` — session-2 analysis
  TOOLs (one-off).
- `results/overnight/{policy.c, residctl_llama.c, residctl_llama.h, test_policy.c,
  wp2_gen.cpp, wp2_opt.c, wp2_llama_mmap.patch, build_wp2.sh, setup_wp2_llama.sh,
  run_wp2_smoke.sh, run_wp2_sweep.sh, wp2_run_opt.sh}` — **copies of `src/` files**
  committed into a results directory during session 2. **7 are byte-identical
  to `src/`** (Q6); the other 5 (`policy.c`, `residctl_llama.c`,
  `residctl_llama.h`, `test_policy.c`, `analyze_*.py`) are **stale** copies —
  `src/` has changed since (Phase 2 + this cleanup). → DATA_DEAD / dedupe.

---

## Citation map summary

`experiments/logs/inventory-citation-map.csv` — every DATA / FIGURE / REPORT / DELIVERABLE
file × which `.md` cites it by basename or path. `orphans.txt` (69) and
`load_bearing.txt` (61).

- **Load-bearing: 61 files** (`load_bearing.txt`) — cited by `results/writeup-package.md`,
  `results/claims.md`, `PROJECT_STATE.md`, or a final/cleanup-session report. **These
  must survive any restructure.** They include: everything under
  `results/final/` and the good `results/cleanup/` files; `results/overnight/`
  `wp{1,2,3}*.md` + `results/data/declared-vs-learned-policy.csv` / `results/data/declared-vs-learned-policy-session1.csv` /
  `results/data/policy-determinism-grid.csv` / `results/data/real-model-arms.csv` / `results/data/real-model-arms-opt-bound.csv` / `figure{2..6}.csv` /
  `results/figures/table-2-environment.csv`; the campaign reports cited in PROJECT_STATE §6
  (`campaign12_phaseD_paper_table.{md,csv}`, `campaign13_phase{A,B,C}_*.md`,
  `experiments/08-compute-phase.md`, `experiments/09-chunk-size-sweep.md`, `experiments/09b-consolidated-6arm-sweep.md`); and
  the six item-10 reports (`experiments/02-first-harness-superseded.md` + `_V2.md`, `experiments/05-async-handler.md`,
  `experiments/06-concurrent-demand.md`, `experiments/04-io-pipelining-diagnostic.md`, `experiments/07-lookahead-window.md`).

- **Orphans: 69 files** (`orphans.txt`), but the filename-only test has **two
  systematic false-negative classes** — corrected here:
  1. **Figures/tables cited as prose ("Figure 6", "Table 1") not by filename.**
     `figure1_bytes_per_work.{png,csv}`, `figure{2..7}_*.png`,
     `results/figures/07-throughput-scaling.csv`, `table1_final_real_model.{md,csv}`,
     `results/figures/table-2-environment.md` are all in `orphans.txt` but are the
     **deliverable figures** — reclassify **load-bearing**. (`experiments/17c-figure-refresh-notes.md`
     and `experiments/14b-figure-generation-notes.md` describe them; the reports reference "Figure N".)
  2. **Raw `*_log.txt` cited as "the run log" not by name.**
     `wp1_sweep_log.txt`, `wp1_determinism_log.txt`, `wp2_sweep_log.txt`,
     `phase{2,3,4}_*_log.txt`, `campaign1{2,3}_*_log.txt`, `item{1..9}_test_log.txt`
     back numbers in cited reports. Treat as **evidence** (archive, not delete).
  - **Genuine orphans (safe to archive):** all `*.cfg` per-run config snapshots
    (~22: `phase1_*.cfg`, `phase3_*.cfg`, `wp2_sweep_*.cfg`, `wp2_gate.cfg`);
    all `*_console.txt` and `*_supervisor.log` from the final/cleanup sessions
    (~10); `experiments/logs/17b-livelock-diagnosis-repro-deadlock.log` (superseded by
    `repro_decisive.log`, which is load-bearing); `wp2_gate_tokens_{mmap,
    residctl}.txt` (the gate's raw output — the verdict is in `experiments/14-real-model-integration.md`).
  - **DATA_DEAD (superseded, keep only as audit trail):**
    `table1_main_results.{md,csv}` + `table2_environment.{md,csv}` (the synthetic
    tables, superseded by `table1_final_real_model` / `table2_final_environment`);
    `results/data/declared-vs-learned-opt-bound-session1.csv` (session-1, superseded by `results/data/declared-vs-learned-opt-bound.csv` —
    note `results/data/declared-vs-learned-policy-session1.csv` IS still cited, session-1 data preserved on
    purpose); `results/overnight/*.{c,h,cpp}` source snapshots.

### Dangling citations in `results/writeup-package.md` — **NONE**

Every one of the 12 file references in `results/writeup-package.md` §1 resolves to a file
on disk (`inventory/scripts/inventory_citemap.sh` output, all `OK`). The one
thing worth noting: `results/writeup-package.md` references `phase2_sweep.csv` /
`results/data/consumption-signal-opt-bound.csv` which resolve to `results/data/consumption-signal-comparison.csv` — correct, but
there is also a `results/overnight/` layer with similarly-named files; the
restructure must not merge the two namespaces.

---

## Bloat and hygiene (Step 4, each yes/no with evidence)

**1. Is the GGUF model tracked or in history?** **NO.** `git ls-files | grep
'\.gguf$'` → 0. `git rev-list --objects --all | cat-file --batch-check` → the 25
largest blobs ever committed are all `results/` logs/PNGs; the largest is 4.0 MB;
no `.gguf`, no 2 GB object anywhere. `.git/objects` is **8.1 MB total**. **No
history rewrite is warranted** — `models/` was gitignored from the start.

**2. Is the llama.cpp clone tracked?** **NO.** 0 tracked files under
`third_party/`. Size **926 MB** (757 MB `build/` incl. a Svelte UI
`node_modules`; 93 MB source clone; 75 MB `third_party/llama.cpp/models/` vocab
GGUFs). **No nested `.git`.** Reconstructable via
`scripts/setup-llama-cpp.sh` + `src/wp2_llama_mmap.patch` + a CMake build.

**3. Are compiled binaries / `.o` / build dirs tracked?** **NO.** BUILD bucket =
0 tracked. The `.gitignore` explicitly lists every test binary, `wp2_gen`,
`wp2_opt`, `replay_main`, `belady_main`, `baseline_main`, `gen_pattern`,
`bench_concurrent_read`, `*.o`, `src/wp2_obj/`, `build/`. (An earlier count of
"12 tracked binaries" was a grep false-positive — those are the `test_*.c`
**source** files.)

**4. Is anything under `scratch/` tracked?** **NO.** `git ls-files | grep
'^scratch/'` → 0. `scratch/` (2.1 GB, dominated by `pattern_2g.bin`) is
`.gitignore`d wholesale.

**5. `.gitignore` contents / what's missing.** Present and thorough:
`scratch/`, `*.o`, `*.bin`, `build/`, `third_party/`, `models/`, every named
test/tool binary, and `results/*.csv` + `results/*.log` **with a `!`-allowlist**
of ~30 specific campaign CSVs. The allowlist pattern is fragile (each new
top-level `results/*.csv` must be explicitly un-ignored) but the per-subdirectory
CSVs (`results/final/*.csv`, `results/cleanup/*.csv`, `results/overnight/*.csv`)
are **not** matched by the top-level `results/*.csv` rule, so they commit
freely — which is why every load-bearing final-session CSV is tracked.
*Arguably missing:* `*.trace` / `*.reftrace` (only ~5 committed, small) and
`__pycache__/` (none tracked, but not ignored). Neither is urgent.

**6. Duplicate files (content sha256, tracked, >1 KiB).** **7 exact-duplicate
pairs**, all `results/overnight/X` == `src/X`:
`run_wp2_sweep.sh`, `run_wp2_smoke.sh`, `residctl_llama.h`, `wp2_gen.cpp`,
`wp2_opt.c`, `build_wp2.sh`, `wp2_llama_mmap.patch`.
Session 2 copied source into `results/overnight/`. Plus 5 near-copies
(`policy.c`, `residctl_llama.c`, `test_policy.c`, `analyze_*.py`) that have since
diverged from `src/`. → the `results/overnight/` code snapshots should be
deleted or archived (they exist in the `pre-restructure` tag regardless).

**7. Near-duplicate reports.** One clear supersession pair kept on purpose:
`experiments/02-first-harness-superseded.md` (item 10 V1, marked SUPERSEDED in place) vs
`experiments/03-corrected-harness.md`. Superseded table pairs: `table1_main_results` →
`table1_final_real_model`; `table2_environment` → `table2_final_environment`.
The other five item-10 reports (`ASYNC`/`CONCURRENCY`/`DIAGNOSTIC`/`LOOKAHEAD`
+ the two harness ones) are distinct items, not duplicates.

---

## Proposed structure (PROPOSAL ONLY — do not implement)

Two destinations: **REPO** (GitHub) and **ARCHIVE** (`/root/residctl-archive/`,
by campaign, never deleted, not on GitHub). A third category — **REGENERABLE** —
is neither: delete and rebuild.

### REPO (target ~6–8 MB, ~140 files)

```
README.md                        NEW — what this is, the headline result (Fig 7), how to build+run
docs/
  SPEC.md                        = MECHANISM_SPEC.md
  docs/04-related-work.md                = docs/04-related-work.md
  PROJECT_STATE.md               (moved from results/)
  HISTORY.md                     NEW or = CLAUDE.md — the campaign-by-campaign log
src/
  *.c *.h *.cpp Makefile *.patch          the 50 SOURCE files
  test_*.c  run_correctness_harness.sh  run_t6_t7.sh   the T-1..T-7 harness
  build_wp2.sh setup_wp2_llama.sh                       llama.cpp integration glue
tools/
  make_figures.py                = tools/make_figures.py
scripts/
  run_final_phase{1,2,3}*.sh  run_wp{1,2}_*.sh  cleanup_p1_sweep.sh   the reproducible sweeps
results/
  FINDINGS.md                    NEW — consolidation of the 8 campaign reports (judgement call 3)
  results/claims.md
  results/writeup-package.md
  FINAL_SUMMARY.md  CLEANUP_SUMMARY.md  OVERNIGHT_SUMMARY.md  BLOCKERS.md
  figures/                       the 7 final PNGs + their CSVs + table1_final/table2_final
  data/                          ONLY load-bearing CSVs (~25): phase1_equal_budget, phase1_opt,
                                 phase2_{determinism,sweep,opt}, phase3_arm_e, wp1_sweep(+session1),
                                 wp1_determinism, wp2_sweep, wp2_opt, figure{2..6}.csv,
                                 results/data/synthetic-consolidated-sweep.csv, campaign13_phase{A2,B,C}_*.csv
  reports/                       the per-phase/session .md reports (final/, cleanup/, overnight/, item-10)
```

| bucket → REPO | files | size |
|---|---|---|
| SOURCE + TEST(.c) + harness scripts | ~65 | 0.5 MB |
| the ~15 reproducible sweep scripts (of 56) | 15 | 0.06 MB |
| TOOL | 1 | 0.02 MB |
| SPEC / docs | 4 | 0.16 MB |
| DELIVERABLE + the phase/session reports | ~40 | 1.5 MB |
| FIGURE (7 PNG + ~10 CSV + 2 final tables) | ~20 | 1.4 MB |
| load-bearing DATA CSVs | ~25 | 2 MB |

### ARCHIVE (`/root/residctl-archive/by-campaign/…`, ~15 MB)

| bucket → ARCHIVE | files | size | why |
|---|---|---|---|
| all `*_log.txt` raw run logs | ~40 | 13 MB | evidence, but noisy; a reader wants the report, not the log |
| `*.cfg`, `*_console.txt`, `*_supervisor.log` | ~35 | 0.5 MB | genuine orphans — per-run scratch that leaked into `results/` |
| `results/overnight/*.{c,h,cpp,sh,py,patch}` code snapshots | 16 | 0.3 MB | dup / stale copies of `src/` (Q6) |
| superseded tables `table1_main_results.*`, `table2_environment.*` | 4 | 0.1 MB | superseded by the `_final` versions |
| `results/data/declared-vs-learned-opt-bound-session1.csv`, other DATA_DEAD | ~5 | 0.1 MB | back only superseded numbers (PROJECT_STATE §6) |
| the ~40 one-off campaign sweep scripts (`run_task_*`, `run_phase*`, `run_c13_*`, `run_item*`, `run_phaseB/D_*`) | ~40 | 0.15 MB | reproducibility evidence for closed campaigns (judgement call 2) |

### REGENERABLE (delete; not archived)

`models/model.gguf` (sha256 `626b4a66…62d`) · `scratch/pattern_2g.bin` +
`pattern_16m.bin` + `test_model.bin` (`gen_pattern`) · `third_party/llama.cpp/`
(the `setup_wp2_llama.sh` + patch + CMake) · `src/*.o` + all compiled binaries +
`src/wp2_obj/` (`make`) · `scratch/__pycache__/`. **≈ 4.9 GB reclaimed.**

---

## Judgement calls for human decision

**1. Superseded reports: keep in repo (audit trail) or move to archive
(clarity)?** *Keep:* the superseded-numbers discipline (a `Superseded prior
claims` field on every claim; a whole PROJECT_STATE §6) is explicitly part of
this project's contribution — the reports it points at should be reachable from
the same repo. *Archive:* six item-10 reports + `experiments/02-first-harness-superseded.md` (V1) +
`experiments/08-compute-phase.md` + `experiments/09-chunk-size-sweep.md` + `experiments/09b-consolidated-6arm-sweep.md` + the
Campaign 12/13 reports is ~20 files a new reader will not open, and the numbers
that still matter are re-stated in `PROJECT_STATE.md` / `results/claims.md`.
*Leaning:* keep, under `results/reports/`, because they are load-bearing per the
citation map.

**2. One-off sweep scripts: reproducibility evidence or clutter?** 56
`src/run_*.sh`; maybe 15 are "someone would re-run this" (the final-session and
WP sweeps), ~40 are closed-campaign one-offs (`run_task_c_sweep2_redo_e.sh`,
`run_phase4_random_only.sh`, …). *Keep all:* they are the exact commands behind
every number. *Archive the 40:* nobody re-runs Campaign 12 Phase B.
*Leaning:* the ~15 reproducible ones to `scripts/`, the rest to archive.

**3. Consolidate the 8 campaign reports into one `FINDINGS.md`, or keep them
individually?** *Consolidate:* a reader wants one results document, not eight
chronological ones; `results/writeup-package.md` §2 already has the narrative order.
*Keep separate:* each is a self-contained pre-registered experiment with its own
"expectations vs measured" table; merging loses the provenance.
*Leaning:* write `FINDINGS.md` as the front door, keep the eight under
`results/reports/` as appendices.

**4. Does `DATA_DEAD` follow its report, or archive separately?** e.g.
`results/figures/historical/table-1-synthetic-main-results.csv` — with `experiments/14b-figure-generation-notes.md` (which still references it) or
in `archive/superseded/`? *Leaning:* archive separately with a one-line
`README` mapping each dead file to the live one that replaced it, so the repo
`data/` directory contains only current numbers.

**5. `results/overnight/` source snapshots (16 files, 7 exact dups of `src/`):
delete or archive?** They are in the `pre-restructure` git tag regardless.
*Leaning:* delete — a stale `/root/residctl-archive/source-snapshots/overnight-policy.c` next to the live
`src/policy.c` is an active hazard (someone reads the wrong one).

**6. Session-1 data (`results/data/declared-vs-learned-policy-session1.csv`, `results/data/declared-vs-learned-opt-bound-session1.csv`).**
`results/data/declared-vs-learned-policy-session1.csv` **is** cited (the WP0-fix supersession); its `_opt`
sibling is not. *Leaning:* keep `results/data/declared-vs-learned-policy-session1.csv` in `data/`, archive the
`_opt` one.

**7. The `.gitignore` `!`-allowlist for `results/*.csv`.** ~30 explicit
un-ignores; every new top-level results CSV needs one. *Options:* (a) leave it;
(b) drop the `results/*.csv` ignore entirely and rely on the fact that data now
lives in `results/data/` and figures in `results/figures/`; (c) invert —
`.gitignore` only `results/**/*_log.txt` and `*_console.txt`. Not urgent, but the
restructure is the moment to simplify it.

---

## What I could not classify

Nothing. All 16 initial `UNKNOWN` files were identified on inspection (see the
bucket summary). Every tracked file has a bucket in `experiments/logs/inventory-classification.csv`; the
~52,000 `third_party/` files are rolled into one `THIRD_PARTY` line by design
(the spec's "anything under it" rule).
