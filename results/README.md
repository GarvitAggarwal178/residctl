# Results

| document | what it is |
|---|---|
| [`findings.md`](findings.md) | **the results document** — the argument in eight steps, problem to payoff, each step linked to its figure and experiment record |
| [`claims.md`](claims.md) | one entry per claim the writeup may assert: evidence, strength (strong / qualified / preliminary), caveats, and every superseded prior version |
| [`superseded.md`](superseded.md) | every retracted number, what replaced it, and why — the project's audit trail |
| [`writeup-package.md`](writeup-package.md) | the consolidated reference the writeup is drawn from (abstract numbers with source cells, narrative skeleton, the dead list, methodology bullets, the five negative results) |
| [`figures/`](figures/) | the seven figures + the two current tables, each with its backing CSV |
| [`data/`](data/) | content-named CSVs behind the current claims; `data/historical/` holds closed-campaign sweeps kept as evidence |

## figures/

| figure | shows | backing data |
|---|---|---|
| `01-bytes-vs-budget` | synthetic: arm D reaches OPT at realistic chunk counts | `01-bytes-vs-budget.csv` |
| `02-miss-rate-vs-optimal` | synthetic: arm C flat at 1.000 miss | `02-miss-rate-vs-optimal.csv` |
| `03-chunk-size-tradeoff` | bytes vs wall-clock optimise in opposite directions | `03-chunk-size-tradeoff.csv` |
| `04-reclaim-authority` | `swap.max=0` → the kernel finds nothing to reclaim | `04-reclaim-authority.csv` |
| `05-prefetch-total-fetches` | prefetch: total fetches, not the denominator-artefact hit rate | `05-prefetch-total-fetches.csv` |
| `06-real-model-bytes` | **real model**: arm D 13–69 % below arm C, D/OPT 1.08–1.13 | `06-real-model-bytes.csv` |
| `07-throughput-scaling` | **the closing figure** — kernel arms flat, arm D rises 2.5× | `07-throughput-scaling.csv` |

`table-1-real-model-results.md` is the real-model main-results table;
`table-2-environment.md` is the environment. `figures/historical/` holds the
Campaign 13 synthetic tables (superseded scope).

Figures 1–5 are synthetic / spike-derived and unchanged by the LIVELOCK FIX.
Figures 6–7 and Table 1 are regenerated from `data/livelock-real-model-arms.csv`
(arms C/D/E) and `data/real-model-bytes-by-budget.csv` (arm A). Regenerate with
`tools/make_figures.py`.

## data/ — the load-bearing files

| file | behind |
|---|---|
| `real-model-bytes-by-budget.csv` | arm A, real model (unchanged since the final session) |
| `livelock-real-model-arms.csv` | arms C/D/E, real model, all four A-14 fixes |
| `livelock-arm-d-protect-on.csv` / `livelock-arm-e-protect-on.csv` | the `protect_current` one-variable cells |
| `livelock-protect-off-regression.csv` | the pre-fix +67–78 % numbers (cited by `superseded.md`) |
| `real-model-opt-bound.csv` | the 65-pass Belady bound |
| `consumption-signal-comparison.csv` / `-determinism-grid.csv` | the synthetic exact-signal sweep |
| `declared-vs-learned-policy.csv` | declared vs inferred order |
| `synthetic-consolidated-sweep.csv` | the Campaign 12 6-arm paper table |
