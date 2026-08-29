#!/usr/bin/env python3
"""Generate docs/cleanup/rename-map.csv for the repository restructure.

Every tracked file gets exactly one row: old_path, new_path, kind, reason.
Files that keep their path have old_path == new_path.

Rules (bulk) + an explicit OVERRIDES table (the ~45 reports / syntheses /
specs that need individual judgement). Run from repo root.
"""
import subprocess, csv, sys, os

ROOT = "/root/residctl"
os.chdir(ROOT)
tracked = subprocess.check_output(["git", "ls-files"], text=True).splitlines()

# ---------------------------------------------------------------- OVERRIDES
# old_path -> (new_path, kind, reason).  new_path "" means DROP-TO-ARCHIVE.
OV = {
  # ---- root ----
  ".gitignore": (".gitignore", "keep", "repo hygiene"),
  "CLAUDE.md": ("CLAUDE.md", "keep", "agent working file; kept per spec, docs/project-log.md is the readable version"),

  # ---- session prompt / instruction files -> archive (user: do not commit prompt files) ----
  "docs/cleanup.md": ("ARCHIVE/session-prompts/cleanup.md", "archive", "agent session instructions; substance folded into docs/project-log.md"),
  "docs/inventory.md": ("ARCHIVE/session-prompts/inventory.md", "archive", "agent session instructions; substance folded into docs/project-log.md"),
  "docs/overnight/WP1.md": ("ARCHIVE/session-prompts/overnight-WP1.md", "archive", "agent session instructions"),
  "docs/overnight/WP2.md": ("ARCHIVE/session-prompts/overnight-WP2.md", "archive", "agent session instructions"),
  "docs/overnight/WP3.md": ("ARCHIVE/session-prompts/overnight-WP3.md", "archive", "agent session instructions"),
  "docs/overnight/Runbook.md": ("ARCHIVE/session-prompts/overnight-Runbook.md", "archive", "agent session instructions"),
  "docs/overnight/final session.md": ("ARCHIVE/session-prompts/final-session.md", "archive", "agent session instructions"),

  # ---- design / methodology syntheses (rewritten in Phase 4) ----
  "docs/MECHANISM_SPEC.md": ("docs/02-design.md", "rewrite", "rewritten as a current-state design document; amendment history moves to design-history.md"),
  "docs/design-history.md": ("docs/design-history.md", "rewrite", "gains the full A-1..A-14 amendment table extracted from MECHANISM_SPEC"),

  # ---- results-root syntheses ----
  "results/PROJECT_STATE.md": ("SPLIT", "rewrite", "split into results/findings.md (S1+narrative), results/superseded.md (S6), docs/05-limitations.md (S2+S3); S4/S5 -> design-history.md"),
  "results/overnight/CLAIMS.md": ("results/claims.md", "move", "the claim->evidence->strength->caveats document; content unchanged in Phase 3, cross-refs fixed in Phase 6"),
  "results/final/WRITEUP_PACKAGE.md": ("results/writeup-package.md", "move", "writeup reference; feeds findings.md / methodology.md, kept as the consolidated source"),
  "results/cleanup/RELATED_WORK.md": ("docs/04-related-work.md", "move", "written for this purpose; moved unchanged"),
  "results/BARE_METAL_PLAN.md": ("docs/bare-metal-plan.md", "move", "readiness plan, not an experiment record; referenced by limitations"),

  # ---- session summaries -> fold into docs/project-log.md, archive the raw ----
  "results/final/FINAL_SUMMARY.md": ("ARCHIVE/session-summaries/final-summary.md", "archive", "session wrapper; narrative folded into docs/project-log.md"),
  "results/overnight/OVERNIGHT_SUMMARY.md": ("ARCHIVE/session-summaries/overnight-summary.md", "archive", "session wrapper; narrative folded into docs/project-log.md"),
  "results/cleanup/CLEANUP_SUMMARY.md": ("ARCHIVE/session-summaries/cleanup-summary.md", "archive", "session wrapper; narrative folded into docs/project-log.md"),
  "results/livelock/LIVELOCK_SUMMARY.md": ("ARCHIVE/session-summaries/livelock-summary.md", "archive", "session wrapper; narrative folded into docs/project-log.md"),
  "results/cleanup/phase2_claims_reconciled.md": ("ARCHIVE/session-summaries/cleanup-claims-reconciled.md", "archive", "synthesis-reconciliation process note; its decisions live in claims.md and design-history.md"),
  "results/livelock/phase4_propagation.md": ("ARCHIVE/session-summaries/livelock-propagation.md", "archive", "doc-propagation process note; the number moves are in superseded.md"),

  # ---- BLOCKERS trackers -> substance into limitations / project-log ----
  "results/final/BLOCKERS.md": ("ARCHIVE/session-summaries/final-blockers.md", "archive", "open-item tracker; live items folded into docs/05-limitations.md"),
  "results/overnight/BLOCKERS.md": ("ARCHIVE/session-summaries/overnight-blockers.md", "archive", "open-item tracker; live items folded into docs/05-limitations.md"),
  "results/livelock/BLOCKERS.md": ("ARCHIVE/session-summaries/livelock-blockers.md", "archive", "open-item tracker; the $?-quirk note folded into docs/06-reproduce.md, findings into project-log"),

  # ---- inventory pass (input to this restructure) ----
  "results/inventory/INVENTORY_REPORT.md": ("experiments/logs/inventory-report.md", "move", "the census this restructure consumed; kept as evidence"),
  "results/inventory/citation_map.csv": ("experiments/logs/inventory-citation-map.csv", "move", "census artifact"),
  "results/inventory/classification.csv": ("experiments/logs/inventory-classification.csv", "move", "census artifact"),
  "results/inventory/load_bearing.txt": ("experiments/logs/inventory-load-bearing.txt", "move", "census artifact"),
  "results/inventory/orphans.txt": ("experiments/logs/inventory-orphans.txt", "move", "census artifact"),
  "results/inventory/raw/all_files.csv": ("experiments/logs/inventory-raw-all-files.csv", "move", "census artifact"),
  "results/inventory/raw/file_type_summary.csv": ("experiments/logs/inventory-raw-type-summary.csv", "move", "census artifact"),
  "results/inventory/raw/git_history_size.txt": ("experiments/logs/inventory-raw-git-size.txt", "move", "census artifact"),
  "results/inventory/raw/repo_size.txt": ("experiments/logs/inventory-raw-repo-size.txt", "move", "census artifact"),
  "results/inventory/scripts/inventory_census.sh": ("scripts/historical/inventory-census.sh", "move", "one-off census script"),
  "results/inventory/scripts/inventory_citemap.sh": ("scripts/historical/inventory-citemap.sh", "move", "one-off census script"),
  "results/inventory/scripts/inventory_classify.sh": ("scripts/historical/inventory-classify.sh", "move", "one-off census script"),

  # ---- EXPERIMENT RECORDS (moved verbatim, renamed by content) ----
  "results/HARNESS_REPORT.md": ("experiments/02-first-harness-superseded.md", "move", "experiment record, verbatim; V1 harness, superseded by V2"),
  "results/HARNESS_REPORT_V2.md": ("experiments/03-corrected-harness.md", "move", "experiment record, verbatim"),
  "results/DIAGNOSTIC_REPORT.md": ("experiments/04-io-pipelining-diagnostic.md", "move", "experiment record, verbatim (item 10b)"),
  "results/ASYNC_REPORT.md": ("experiments/05-async-handler.md", "move", "experiment record, verbatim (item 10c)"),
  "results/CONCURRENCY_REPORT.md": ("experiments/06-concurrent-demand.md", "move", "experiment record, verbatim (item 10d)"),
  "results/LOOKAHEAD_REPORT.md": ("experiments/07-lookahead-window.md", "move", "experiment record, verbatim (item 10e)"),
  "results/phase0_analysis.md": ("experiments/07b-existing-data-analysis.md", "move", "experiment record, verbatim (Campaign 11 Phase 0)"),
  "results/phase1_platform_io.md": ("experiments/07c-platform-io-microbenchmark.md", "move", "experiment record, verbatim (Campaign 11 Phase 1)"),
  "results/phase2_compute.md": ("experiments/08-compute-phase.md", "move", "experiment record, verbatim (Campaign 11 Phase 2)"),
  "results/phase3_chunk_size.md": ("experiments/09-chunk-size-sweep.md", "move", "experiment record, verbatim (Campaign 11 Phase 3)"),
  "results/phase4_consolidated.md": ("experiments/09b-consolidated-6arm-sweep.md", "move", "experiment record, verbatim (Campaign 11 Phase 4)"),
  "results/campaign12_phaseA_baseline.md": ("experiments/09c-baseline-io-repair.md", "move", "experiment record, verbatim (Campaign 12 Phase A: drop_caches guest-side bug)"),
  "results/campaign12_phaseB_chunk_floor.md": ("experiments/09d-chunk-size-floor.md", "move", "experiment record, verbatim (Campaign 12 Phase B)"),
  "results/campaign12_phaseC_hitrate_audit.md": ("experiments/12b-hitrate-count-hypothesis.md", "move", "experiment record, verbatim (Campaign 12 Phase C, hit-rate audit precursor)"),
  "results/campaign12_phaseD_paper_table.md": ("experiments/10-consolidated-sweep.md", "move", "experiment record, verbatim (Campaign 12 Phase D)"),
  "results/campaign13_phaseA_policy_determinism.md": ("experiments/11-policy-determinism.md", "move", "experiment record, verbatim (Campaign 13 Phase A)"),
  "results/campaign13_phaseB_metric_repair.md": ("experiments/12-metric-audit.md", "move", "experiment record, verbatim (Campaign 13 Phase B)"),
  "results/campaign13_phaseC_claims.md": ("experiments/12c-claims-rederivation.md", "move", "experiment record, verbatim (Campaign 13 Phase C)"),
  "results/overnight/wp1_declared_order.md": ("experiments/13-declared-access-order.md", "move", "experiment record, verbatim (WP1)"),
  "results/overnight/wp2_llamacpp.md": ("experiments/14-real-model-integration.md", "move", "experiment record, verbatim (WP2)"),
  "results/overnight/wp3_figures.md": ("experiments/14b-figure-generation-notes.md", "move", "figure-build record, verbatim (WP3)"),
  "results/final/phase1_equal_budget.md": ("experiments/15-equal-budget-baseline.md", "move", "experiment record, verbatim (final session Phase 1)"),
  "results/final/phase2_consumption_signal.md": ("experiments/16-consumption-signal.md", "move", "experiment record, verbatim (final session Phase 2)"),
  "results/final/phase3_arm_e_collapse.md": ("experiments/17-prefetch-collapse.md", "move", "experiment record, verbatim (final session Phase 3)"),
  "results/final/phase4_figures.md": ("experiments/17c-figure-refresh-notes.md", "move", "figure-refresh record, verbatim (final session Phase 4)"),
  "results/cleanup/phase1_deadlock_fix.md": ("experiments/17b-livelock-diagnosis.md", "move", "experiment record, verbatim (cleanup session: gdb + 420s trace, livelock not deadlock)"),
  "results/livelock/phase0_cursor_diagnostic.md": ("experiments/18-signal-audit.md", "move", "experiment record, verbatim (livelock session Phase 0: node-name mismatch)"),
  "results/livelock/phase1_fixes.md": ("experiments/19-livelock-fix.md", "move", "experiment record, verbatim (livelock session Phase 1: the four defects + fixes)"),
  "results/livelock/phase2_synthetic.md": ("experiments/20-livelock-synthetic-recheck.md", "move", "experiment record, verbatim (livelock session Phase 2: synthetic unchanged)"),
  "results/livelock/phase3_real_model.md": ("experiments/21-livelock-real-model.md", "move", "experiment record, verbatim (livelock session Phase 3/3b/3c + A-14)"),

  # ---- figure tables (markdown) ----
  "results/overnight/figures/table1_final_real_model.md": ("results/figures/table-1-real-model-results.md", "move", "current results table"),
  "results/overnight/figures/table2_final_environment.md": ("results/figures/table-2-environment.md", "move", "current environment table"),
  "results/overnight/figures/table1_main_results.md": ("results/figures/historical/table-1-synthetic-main-results.md", "move", "Campaign 13 synthetic table, superseded scope"),
  "results/overnight/figures/table2_environment.md": ("results/figures/historical/table-2-environment-campaign13.md", "move", "Campaign 13 environment table, superseded by table-2"),

  # ---- duplicate source snapshots in results/overnight/ -> archive (byte-compare in Phase 2) ----
  "results/overnight/policy.c": ("ARCHIVE/source-snapshots/overnight-policy.c", "archive", "stale copy of src/policy.c"),
  "results/overnight/residctl_llama.c": ("ARCHIVE/source-snapshots/overnight-residctl_llama.c", "archive", "stale copy of src/residctl_llama.c"),
  "results/overnight/residctl_llama.h": ("ARCHIVE/source-snapshots/overnight-residctl_llama.h", "archive", "copy of src/residctl_llama.h"),
  "results/overnight/test_policy.c": ("ARCHIVE/source-snapshots/overnight-test_policy.c", "archive", "stale copy of src/test_policy.c"),
  "results/overnight/wp2_gen.cpp": ("ARCHIVE/source-snapshots/overnight-wp2_gen.cpp", "archive", "copy of src/wp2_gen.cpp"),
  "results/overnight/wp2_opt.c": ("ARCHIVE/source-snapshots/overnight-wp2_opt.c", "archive", "copy of src/wp2_opt.c"),
  "results/overnight/wp2_llama_mmap.patch": ("ARCHIVE/source-snapshots/overnight-wp2_llama_mmap.patch", "archive", "copy of src/wp2_llama_mmap.patch"),
  "results/overnight/build_wp2.sh": ("ARCHIVE/source-snapshots/overnight-build_wp2.sh", "archive", "copy of src/build_wp2.sh"),
  "results/overnight/setup_wp2_llama.sh": ("ARCHIVE/source-snapshots/overnight-setup_wp2_llama.sh", "archive", "copy of src/setup_wp2_llama.sh"),
  "results/overnight/run_wp2_smoke.sh": ("ARCHIVE/source-snapshots/overnight-run_wp2_smoke.sh", "archive", "copy of src/run_wp2_smoke.sh"),
  "results/overnight/run_wp2_sweep.sh": ("ARCHIVE/source-snapshots/overnight-run_wp2_sweep.sh", "archive", "copy of src/run_wp2_sweep.sh"),
  "results/overnight/wp2_run_opt.sh": ("ARCHIVE/source-snapshots/overnight-wp2_run_opt.sh", "archive", "copy of src/wp2_run_opt.sh"),
  "results/overnight/analyze_wp1_sweep.py": ("tools/analyze-wp1-sweep.py", "move", "one-off analysis tool, kept for reproducibility"),
  "results/overnight/analyze_wp2.py": ("tools/analyze-wp2.py", "move", "one-off analysis tool, kept for reproducibility"),
  "results/overnight/make_wp3.py": ("tools/make_figures.py", "move", "the figure generator; rewired for the new data paths in Phase 6"),

  # ---- .gitkeep ----
  "results/.gitkeep": ("results/.gitkeep", "keep", "keeps results/ in tree"),

  # ---- livelock one-off diagnostic scripts ----
  "results/livelock/audit_negtest.sh": ("scripts/historical/livelock-audit-negtest.sh", "move", "one-off: reverts the embd match to confirm the startup audit aborts"),
  "results/livelock/dbg_nodes.sh": ("scripts/historical/livelock-dbg-nodes.sh", "move", "one-off: instrumented build dumping eval_cb node names"),

  # ---- cleanup-session verify data: cited by superseded.md (the +67-78% numbers) ----
  "results/cleanup/phase1_verify.csv": ("results/data/livelock-protect-off-regression.csv", "move", "the pre-fix +67-78% protect-off arm-D numbers; cited by superseded.md"),
  "results/cleanup/phase1_reverify.csv": ("results/data/livelock-protect-off-regression-reverify.csv", "move", "reverification of the protect-off regression; cited by superseded.md"),
  "results/cleanup/phase1_verify_log.txt": ("experiments/logs/17b-livelock-diagnosis-verify.txt", "move", "raw log for experiment 17b"),
  "results/cleanup/repro_deadlock.log": ("experiments/logs/17b-livelock-diagnosis-repro-deadlock.log", "move", "gdb / counter trace evidence for experiment 17b"),
  "results/cleanup/repro_decisive.log": ("experiments/logs/17b-livelock-diagnosis-repro-decisive.log", "move", "the decisive 420s counter trace for experiment 17b"),
}

# tools that live in src/ but are figure/trace/opt tooling per the target layout.
# NOTE: .c/.h/.cpp/.patch/Makefile in src/ are NEVER moved (hard spec rule).
SRC_TOOL_SCRIPTS = {}  # none: wp2_opt.c stays in src/ (compiled binary source)

CURRENT_SWEEPS = {
  "src/run_correctness_harness.sh": "run-correctness-harness.sh",
  "src/run_t6_t7.sh": "run-storm-t6-t7.sh",
  "src/run_final_phase1.sh": "run-real-model-equal-budget.sh",
  "src/run_final_phase1_opt.sh": "run-real-model-opt.sh",
  "src/run_final_phase2.sh": "run-consumption-signal-sweep.sh",
  "src/run_final_phase3.sh": "run-prefetch-collapse-probe.sh",
  "src/run_wp1_sweep.sh": "run-declared-vs-learned-sweep.sh",
  "src/run_wp1_determinism.sh": "run-policy-determinism-grid.sh",
  "src/run_wp1_gate.sh": "run-learned-policy-gate.sh",
  "src/run_wp1_policytrace.sh": "run-policy-trace.sh",
  "src/run_wp2_sweep.sh": "run-real-model-arms-sweep.sh",
  "src/run_wp2_gate.sh": "run-real-model-correctness-gate.sh",
  "src/run_wp2_smoke.sh": "run-real-model-smoke.sh",
  "src/setup_wp2_llama.sh": "setup-llama-cpp.sh",
  "src/build_wp2.sh": "build-real-model-integration.sh",
  "src/wp2_run_opt.sh": "run-real-model-opt-solver.sh",
  "src/livelock_phase0.sh": "run-signal-audit.sh",
  "src/livelock_phase2.sh": "run-livelock-synthetic-recheck.sh",
  "src/livelock_phase3.sh": "run-livelock-real-model.sh",
  "src/livelock_phase3b.sh": "run-livelock-protect-on-probe.sh",
  "src/livelock_phase3c.sh": "run-livelock-arm-d-protect-on.sh",
  "src/livelock_phase0_analyze.py": None,   # -> tools/
  "src/livelock_phase2_analyze.py": None,
  "src/livelock_phase3_analyze.py": None,
}
ANALYZE_TO_TOOLS = {
  "src/livelock_phase0_analyze.py": "tools/analyze-signal-audit.py",
  "src/livelock_phase2_analyze.py": "tools/analyze-livelock-synthetic.py",
  "src/livelock_phase3_analyze.py": "tools/analyze-livelock-real-model.py",
}

def classify(p):
    if p in OV:
        return OV[p]
    d = p.split("/")
    base = d[-1]

    # src/ source: never move
    if p.startswith("src/") and (base.endswith((".c",".h",".cpp",".patch")) or base == "Makefile"):
        return (p, "keep", "mechanism / test / integration source; spec forbids moving src/*")

    # src/ analyze .py -> tools/
    if p in ANALYZE_TO_TOOLS:
        return (ANALYZE_TO_TOOLS[p], "move", "analysis tool -> tools/")

    # src/ scripts
    if p.startswith("src/") and base.endswith(".sh"):
        if p in CURRENT_SWEEPS and CURRENT_SWEEPS[p]:
            return ("scripts/" + CURRENT_SWEEPS[p], "move", "current reproducible sweep -> scripts/")
        return ("scripts/historical/" + base.replace("_","-"), "move", "closed-campaign one-off -> scripts/historical/")
    if p.startswith("src/") and base.endswith(".py"):
        return ("tools/" + base.replace("_","-"), "move", "analysis / tooling script -> tools/")

    # figures (png/csv pairs)
    if p.startswith("results/overnight/figures/") and base.endswith((".png",".csv")):
        stem = base.rsplit(".",1)[0]; ext = base.rsplit(".",1)[1]
        fmap = {
          "figure1_bytes_per_work":"01-bytes-vs-budget",
          "figure2_miss_rate":"02-miss-rate-vs-optimal",
          "figure3_chunk_size_tradeoff":"03-chunk-size-tradeoff",
          "figure4_reclaim_authority":"04-reclaim-authority",
          "figure5_prefetch_total_fetches":"05-prefetch-total-fetches",
          "figure6_llamacpp":"06-real-model-bytes",
          "figure7_throughput_scaling":"07-throughput-scaling",
          "table1_final_real_model":"table-1-real-model-results",
          "table1_main_results":"historical/table-1-synthetic-main-results",
          "table2_environment":"historical/table-2-environment-campaign13",
          "table2_final_environment":"table-2-environment",
        }
        if stem in fmap:
            return (f"results/figures/{fmap[stem]}.{ext}", "move", "current figure/table backing file")
        return (f"results/figures/{stem.replace('_','-')}.{ext}", "move", "figure backing file")

    # data CSVs under results/
    if p.startswith("results/") and base.endswith(".csv"):
        return (data_target(p, base), "move", data_reason(p))

    # logs / traces / tokens / cfg
    if p.startswith("results/") and base.endswith((".log",".txt",".trace",".tok",".cfg")):
        if base.endswith(".cfg"):
            return (f"ARCHIVE/run-configs/{p.replace('/','__')}", "archive", "per-run config snapshot, regenerable from scripts/")
        if "supervisor" in base or base.endswith(("_console.txt","_console.log")) or base == "phase1_supervisor.log":
            return (f"ARCHIVE/process-logs/{p.replace('/','__')}", "archive", "supervisor / console log, process noise")
        if base.endswith("_console.txt"):
            return (f"ARCHIVE/process-logs/{p.replace('/','__')}", "archive", "console log, process noise")
        # raw run logs and evidence text -> experiments/logs/
        return (f"experiments/logs/{p.replace('results/','').replace('/','__')}", "move", "raw run log / evidence, kept under experiments/logs/")

    # anything else
    return (p, "REVIEW", "unclassified - needs a manual rule")

def data_target(p, base):
    stem = base[:-4]
    dmap = {
      "phase1_equal_budget": "results/data/real-model-bytes-by-budget.csv",
      "phase1_opt": "results/data/real-model-opt-bound.csv",
      "phase2_sweep": None,          # collision - resolved below
      "phase2_determinism": None,
      "phase2_opt": "results/data/consumption-signal-opt-bound.csv",
      "phase3_arm_e": "results/data/prefetch-collapse-fallback-configs.csv",
      "wp1_sweep": "results/data/declared-vs-learned-policy.csv",
      "wp1_sweep_session1": "results/data/declared-vs-learned-policy-session1.csv",
      "wp1_sweep_opt": "results/data/declared-vs-learned-opt-bound.csv",
      "wp1_sweep_opt_session1": "results/data/declared-vs-learned-opt-bound-session1.csv",
      "wp1_determinism": "results/data/policy-determinism-grid.csv",
      "wp2_sweep": "results/data/real-model-arms.csv",
      "wp2_sweep_before_fix": "results/data/real-model-arms-before-wp0-fix.csv",
      "wp2_opt": "results/data/real-model-arms-opt-bound.csv",
      "campaign12_phaseD_paper_table": "results/data/synthetic-consolidated-sweep.csv",
      "campaign12_phaseB_chunk_floor": "results/data/synthetic-chunk-size-floor.csv",
      "campaign12_phaseA_phase3_AB": "results/data/baseline-io-repair-phase3-grid.csv",
      "campaign12_phaseA_phase4_AB": "results/data/baseline-io-repair-phase4-grid.csv",
      "campaign13_phaseA1_reproduce": "results/data/policy-determinism-reproduction.csv",
      "campaign13_phaseA2_determinism": "results/data/policy-determinism-6cell.csv",
      "phase1_platform_io": "results/data/platform-io-microbenchmark.csv",
      "phase2_compute": "results/data/synthetic-compute-phase-sweep.csv",
      "phase3_chunk_size": "results/data/synthetic-chunk-size-sweep.csv",
      "phase4_consolidated": "results/data/synthetic-6arm-consolidated-sweep.csv",
      "harness_sweep": "results/data/harness-v1-sweep-superseded.csv",
      "harness_v2_sweep": "results/data/harness-v2-sweep.csv",
      "phase3_real_model": "results/data/livelock-real-model-arms.csv",
      "phase3b_arm_e_protect_on": "results/data/livelock-arm-e-protect-on.csv",
      "phase3c_arm_d_protect_on": "results/data/livelock-arm-d-protect-on.csv",
      "phase2_determinism_": None,
    }
    # livelock phase2 CSVs (results/livelock/phase2_*.csv) are copies of the
    # synthetic recheck; keep under experiments/logs to avoid clashing with
    # results/final/phase2_*.csv canonical names.
    if p.startswith("results/livelock/phase2_"):
        return f"experiments/logs/livelock__{base}"
    if p.startswith("results/final/") and stem in ("phase2_sweep","phase2_determinism"):
        return {"phase2_sweep":"results/data/consumption-signal-comparison.csv",
                "phase2_determinism":"results/data/consumption-signal-determinism-grid.csv"}[stem]
    if p.startswith("results/livelock/") and stem in ("phase2_sweep","phase2_determinism"):
        return f"experiments/logs/livelock__{base}"
    if stem in dmap and dmap[stem]:
        return dmap[stem]
    # task_* / item* / campaign13 loose CSVs -> historical data
    return f"results/data/historical/{stem.replace('_','-')}.csv"

def data_reason(p):
    if "/historical/" in data_target(p, p.split('/')[-1]):
        return "closed-campaign data, kept as evidence under results/data/historical/"
    if p.startswith("results/livelock/phase2_"):
        return "livelock synthetic-recheck raw data (byte-identical to final phase2); kept under experiments/logs/"
    return "backing data for a current claim -> results/data/, content-named"

rows = []
for p in tracked:
    new, kind, reason = classify(p)
    rows.append((p, new, kind, reason))

os.makedirs("docs/cleanup", exist_ok=True)
with open("docs/cleanup/rename-map.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["old_path","new_path","kind","reason"])
    for r in sorted(rows):
        w.writerow(r)

# report
from collections import Counter
c = Counter(r[2] for r in rows)
print("rows:", len(rows), "  tracked:", len(tracked))
for k,v in sorted(c.items()): print(f"  {k:9} {v}")
rev = [r for r in rows if r[2] == "REVIEW"]
if rev:
    print("\nREVIEW (%d):" % len(rev))
    for r in rev: print("  ", r[0])
# collision check
seen = {}
for old,new,kind,_ in rows:
    if kind in ("archive",): continue
    if new in ("SPLIT",): continue
    if new in seen:
        print(f"COLLISION: {new}  <- {seen[new]}  AND  {old}")
    seen[new] = old
