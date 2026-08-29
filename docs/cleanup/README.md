# Restructure working files

Transient artifacts for the repository restructure (see the commit sequence in
the restructure spec). These move to `experiments/logs/` at the end.

- `gen_rename_map.py` — regenerates `rename-map.csv` from `git ls-files` + rules.
- `rename-map.csv` — `old_path, new_path, kind, reason` for every tracked file.
  Every tracked file appears exactly once. `kind` is one of:
  - `keep` — path unchanged (all of `src/*.c|*.h|*.cpp|*.patch|Makefile`, `.gitignore`, `CLAUDE.md`).
  - `move` — `git mv` to the new path in Phase 3, content untouched.
  - `rewrite` — the document is rewritten in Phase 4 (`MECHANISM_SPEC.md`, `design-history.md`); `SPLIT` means it becomes several documents.
  - `archive` — copied to `/root/residctl-archive/` (shown as `ARCHIVE/...`) and `git rm`'d in Phase 2. Process noise only: per-run `.cfg`, console/supervisor logs, duplicate source snapshots, session prompt/summary files. Their *substance* is preserved — session narratives fold into `docs/project-log.md`, blocker items into `docs/05-limitations.md`.

## Experiment numbering

Experiment records move **verbatim** to `experiments/NN-<content>.md`. The
mainline numbers `01`–`21` follow the restructure spec's own table and the
final/livelock session boundaries it fixes (`15`–`17` = final session,
`18`–`21` = livelock session).

Campaign sub-phases the spec's table does not enumerate are given a
topic-grouped sub-letter next to their sibling rather than a new mainline
number — so all of the chunk-size work reads together as `09`, `09b`, `09c`,
`09d`, and the metric-audit thread as `12`, `12b`, `12c`. `experiments/README.md`
carries the full table (number → question → answer → current/superseded), which
is the intended navigation aid.

Figure-build notes (`wp3_figures.md` → `14b`, `phase4_figures.md` → `17c`) are
kept as records because they say which figure comes from which CSV, but they
are not pre-registered experiments.
