# Table 2 — Environment and configuration

Every value needed to reproduce the sweeps behind Figures 1–5 and Table 1.

| parameter | value |
|---|---|
| kernel release | 6.18.33.2-microsoft-standard-WSL2 (WSL2, guest-only; bare metal out of scope) |
| CPU / RAM | 16 logical cores / 7.6 GiB (WSL2 VM) |
| filesystem (model + region backing) | ext4 on /dev/sdd (rw,relatime,discard,data=ordered); Windows VHDX host cache present and unreachable by guest drop_caches |
| cgroup version | v2 (cgroup2fs); controllers: cpuset cpu io memory hugetlb pids rdma |
| THP transparent_hugepage/enabled | madvise |
| THP shmem_enabled | never (shmem pages are not transparently collapsed to hugepages) |
| memory.swap.max | 0 in every sweep (I-3) — eviction authority test |
| memory.high | budget_bytes (per cell); memory.max = budget_bytes + 64 MiB margin |
| region size | 2 GiB (2,147,483,648 B), fixed across every sweep except chunk-size sweeps |
| chunk sizes swept | Phase D / WP1: {8 MiB (256 chunks), 128 MiB (16 chunks)}; chunk-size sweeps: {4,8,16,32,64,128,256} MiB |
| n_passes | 5 (references = 5 x n_chunks) |
| handler mode | async dispatch-only (A-5); --sync-handler available but unused in these sweeps |
| fetch workers | 4 (--fetch-workers 4) |
| driver threads | 8 (--driver-threads 8) |
| lookahead window | 1 (--lookahead-window 1; W+1=2 chunks in flight max) |
| prefetch depth | 2 for arm E (--prefetch-depth 2); n/a for arm D (prefetch off) |
| prefetch retention | pinned (A-9, --prefetch-retention pinned) |
| prefetch admission | guarded (A-6 default) |
| reconcile interval | amortized 16 in sweeps; 1 (eager) hard-compiled in T-1..T-7 |
| policy | WP1: layer_order_declared (default, A-12) and layer_order_learned; prior campaigns: layer_order (== learned) |
| compute phase | --compute-ns-per-mib in {0, 400000}; achieved rate ~1.5-1.8 Mns/MiB at the 400000 setting (calibration overshoot, disclosed Campaign 11 Phase 2) |
| O_DIRECT bandwidth ceiling | 3396 MiB/s (spike max; points above = host-cache contamination) |
| model file hash (WP2) | n/a — WP2 not run, models/model.gguf absent (BLOCKER 1) |
