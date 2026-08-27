# CLAIMS — what the writeup may assert, and on what evidence

One entry per claim the report will make. `Strength`: strong (clean,
replicated, deterministic) / qualified (real but with a named caveat) /
preliminary (single regime, or synthetic-only where that matters).

---

### Claim 1: Kernel LRU degenerates to full-pass thrashing on a cyclic reference string.

Evidence: `campaign12_phaseD_paper_table.csv` — arm C, every one of the 20
cells: `absent_handled == touches` (1280 at 8 MiB, 80 at 128 MiB), miss
rate exactly 1.000, `read_bytes == 10,737,418,240` (the whole 2 GiB region
every run). Reproduced in every sweep back to item 7's smoke test
(`lru` 40/40 faults). `results/overnight/figures/figure2_miss_rate.csv`.

Figure: Figure 2 (the flat line at 1.000); Figure 1 (arm C flat at the
region size).

Strength: **strong.** Deterministic, universal across every cell and every
sweep, and mechanically inevitable (the working set exceeds budget and LRU
evicts the chunk that will be needed soonest on a cyclic scan).

Caveats: only demonstrated on the cyclic-scan replay workload — but a
repeating layer scan is exactly the real llama.cpp access pattern (WP2
would have confirmed on a real model; not run — BLOCKER 1). A reviewer
could argue a real workload has enough irregularity to give LRU some hits;
the answer is that the irregularity would have to be large relative to the
budget deficit, and inference over fixed layers has none.

Superseded prior claims: none.

---

### Claim 2: Application-authoritative residency reduces bytes read per unit of work, and the reduction scales with available budget.

Evidence: `figure1_bytes_per_work.csv` / Table 1. compute=0, arm D
(`layer_order_declared`, WP1): read_bytes/touch falls monotonically as
budget rises — 8 MiB: 6.41 → 4.80 → 3.36 MiB at r=0.25/0.5/0.75; 128 MiB:
104.2 → 76.6 → 51.3 MiB — against arm C flat at 8.00 / 134.2 MiB and arm A
flat at ~8.06 / ~134 MiB. Declared arm D equals OPT exactly at all six
compute=0 cells (`wp1_declared_order.md` §1.4).

Figure: Figure 1 (central figure), Figure 2.

Strength: **strong at compute=0** (deterministic, equals the Belady
bound). **Qualified under a concurrent compute phase**: at
`--compute-ns-per-mib 400000` the declared policy becomes
non-deterministic and reads *more* than the learned policy at 5 of 6 cells
(up to 1.9× OPT) — see Claim 7's caveat.

Caveats: (1) the byte reduction is measured against a baseline (arm A) some
of whose points are host-cache contaminated (Table 1 flag `h`) — but arm A
and arm C agree closely on bytes and arm C is not contaminated, so the
"D reads far less than the kernel arms" comparison stands on arm C alone.
(2) synthetic workload only.

Superseded prior claims: Campaign 12 Phase C / Campaign 13 Phase C reported
D/OPT of 1.070–1.775 for the **learned** policy. WP1 supersedes this at
compute=0: the **declared** policy reads exactly OPT (D/OPT = 1.000). The
1.07–1.78 figures remain correct for `layer_order_learned` and for every
compute=400000 cell.

---

### Claim 3: Eviction under `memory.swap.max = 0` is authoritative — the kernel enters reclaim and finds nothing eligible.

Evidence: spike S3d vs S3e (`/root/spike/results/s3d_output.log`,
`s3e_summary.txt`, `s3e_verify.txt`), `figure4_reclaim_authority.csv`.
Under `swap.max=0`: `memory.events[high]=37` (the kernel *did* enter
reclaim), `pgscan=0`, `pgsteal=0`, `memory.stat[shmem]` unchanged at
600 MiB. With swap available: `pgscan≈120,687`, `pgsteal≈60,141`,
`high=512–515`, ~236 MiB of shmem reclaimed to swap (3 runs).

Figure: Figure 4.

Strength: **strong.** The project's cleanest causal result — a controlled
A/B on one cgroup setting, 3 replicates, unambiguous direction.

Caveats: measured in the feasibility spike, not re-run this session; on the
same WSL2 VM. The mechanism (no swap ⇒ anonymous/shmem pages are
unevictable ⇒ the application's `FALLOC_FL_PUNCH_HOLE` is the only thing
that frees them) is kernel-general, not WSL-specific.

Superseded prior claims: none — this claim currently exists only as prose
in the spike addendum; Figure 4 is its first visualisation.

---

### Claim 4: The offline optimal bound is computable for this workload and both policies are measurably distant from it.

Evidence: `belady_main` (naive O(n²) cross-check 300/300, exact cyclic-floor
check on every run — `wp1_correctness_harness_log.txt` T-5). OPT per cell
in `figure1_bytes_per_work.csv` / `wp1_sweep_opt.csv`. Distance:
`layer_order_learned` D/OPT 1.06–1.53 at the clean cells;
`layer_order_declared` D/OPT = 1.000 at compute=0 but 1.5–2.0 at
compute=400000.

Figure: Figures 1 and 2 (OPT dashed reference).

Strength: **strong.** The bound is exact (provable cyclic floor) and
independently cross-checked.

Caveats: the "declared = OPT" result means one policy is *not* measurably
distant from OPT in the deterministic regime — the claim should be phrased
"both policies are distant from OPT under a concurrent compute phase; at
compute=0 the declared policy reaches it."

Superseded prior claims: "both policies are measurably distant from OPT"
(Campaign 13 Phase C, D/OPT 1.070–1.775) is now regime-dependent.

---

### Claim 5: Chunk size trades byte efficiency against wall-clock, optimising in opposite directions.

Evidence: `figure3_chunk_size_tradeoff.csv` — Campaign 12 Phase B {4,8,16,32}
+ Campaign 11 Phase 3 {32,64,128,256} MiB, arm D, compute=0. read_bytes has
a shallow minimum near 8–16 MiB then climbs to 256 MiB; wall-clock has a
valley near 16–32 MiB and is worst at 4 MiB.

Figure: Figure 3.

Strength: **qualified.** The two source sweeps used different scripts and
did **not** agree exactly at their 32 MiB overlap point (Phase B read
3–7% more bytes and ran ~10% slower than Phase 3 at 32 MiB — machine-load
and script differences, disclosed on the figure). The *direction* of each
trend is consistent within each sweep; the absolute cross-sweep join is
approximate.

Caveats: `layer_order_learned` only; compute=0 only; region size fixed at
2 GiB throughout. Campaign 11 Phase 3's own "bytes and wall-clock move
together" finding was for its coarser 32–256 MiB range and is superseded
below 32 MiB.

Superseded prior claims: Campaign 11 Phase 3 expectation 5 ("wall-clock
and bytes move together, no divergence") — superseded by Campaign 12
Phase B and shown in Figure 3.

---

### Claim 6: Prefetch pays only when there is a compute phase to overlap against.

Evidence: `figure5_prefetch_total_fetches.csv` (Campaign 12 Phase D,
128 MiB, learned). compute=0: arm E's total fetches (demand + prefetch)
**exceed** arm D's at every ratio (71→98, 63→85, 57→74, 51→66, 41→53).
compute=400000: E ≈ D or below at r ≥ 0.5 (80→79 at r=0.5, 50→48 at
r=0.75) — the only cells where prefetch does not cost extra volume.

Figure: Figure 5.

Strength: **qualified.** The effect is real and replicated (Campaign 11
Phase 2 and Phase 4 independently), but the win is marginal (1–2 fetches)
and only appears at looser budgets; at r ≤ 0.375 prefetch still costs
volume even with compute. The metric itself (total fetches) replaced hit
rate, which Campaign 13 Phase B showed was an artifact of its own
denominator.

Caveats: synthetic compute phase (`--compute-ns-per-mib`, a busy loop),
not real matmul — WP2 Phase 2.3 expectation 3 was written specifically to
test this against real inference compute and was not run (BLOCKER 1).
WP1's declared-order arm E shows the same qualitative pattern
(`wp1_sweep.csv`).

Superseded prior claims: the "14–46% prefetch hit-rate ceiling" as
evidence of mechanism saturation (items 10b–10e) — superseded by Campaign
13 Phase B; Figure 5 uses the replacement metric.

---

### Claim 7: Declared access order outperforms inferred access order — in the deterministic regime; under a concurrent compute phase it does not.

Evidence: `wp1_declared_order.md`, `wp1_sweep.csv`, `wp1_determinism.csv`,
`wp1_policytrace_log.txt`. compute=0: `layer_order_declared` reads exactly
OPT at all 6 cells vs `layer_order_learned` 1.06–1.28× OPT; deterministic
at A.2 cells 1–4 at the Belady floor. compute=400000: declared is
non-deterministic (same three-factor trigger as learned) and reads +2% to
+47% more than learned at 5 of 6 cells, exceeding arm C at both r=0.25
cells.

Figure: Figures 1 and 2 (declared arm D on the OPT line).

Strength: **qualified — two-sided.** Strong for the compute=0 half
(deterministic, equals OPT, verification gate passed). The compute=400000
half is a genuine negative result reported in full.

Caveats: the non-determinism is the same one Campaign 13 Phase A found for
the learned policy — WP1 moved it from chain construction to the
consumption-position signal but did not remove it (`--policy-trace`:
identical resident set, cursor differs by one). A reviewer will ask "so is
declared order actually better?" — the honest answer is "strictly better
whenever the workload's own timing is deterministic; worse than the
learned policy's hedge when 8 concurrent driver threads race a real
compute phase, because declared confidently evicts a just-consumed chunk a
straggler still needs."

Superseded prior claims: the project's `layer_order` (now
`layer_order_learned`) was the only informed policy through Campaign 13;
A-12 adds the declared variant. Campaign 13 Phase A's finding that the
learned chain is fault-dispatch-order-dependent stands.

Only valid because WP1 completed.

---

### Claim 8: The findings hold on a real model.

Evidence: **NONE — WP2 was not run.** `models/model.gguf` is absent
(`results/overnight/BLOCKERS.md` BLOCKER 1). Every finding in this project
remains validated only against the synthetic cyclic-scan replay driver.

Figure: Figure 6 was to show this; not produced.

Strength: **cannot be claimed.** The writeup must state explicitly that no
finding has been tested against a real, non-synthetic access pattern, and
that WP2 (llama.cpp integration) is the single largest open question.

Caveats: the synthetic driver was built as a faithful model of llama.cpp's
layer-scan access pattern (`replay.h`, `MECHANISM_SPEC.md` §11), and the
one structural difference WP2 would have measured — real per-layer compute
time vs the `--compute-ns-per-mib` proxy — is exactly the axis on which
WP1 found the declared policy's behaviour flips. So the real-model result
is not merely "unconfirmed detail" but potentially "changes which policy
wins."

Superseded prior claims: none.
