# Table 1 (FINAL) — real model, equal budget

Qwen2.5-3B Q4_K_M, CPU, 64 tokens, n=3, `memory.max = B` (arm A) / `budget_bytes = B` (+128 MiB `memory.max`, pager arms). read/OPT uses `wp2_opt` over the declared sequence (65 layer-scans). Arm A `read` is the `/proc/self/io` delta; C/D/E is `pager_bytes_fetched`.

| ratio | arm | config | read_GB | read/OPT | demand_faults | tokens_s_med | p99_inter_ms_med | note |
|---|---|---|---|---|---|---|---|---|
| 0.25 | A | kernel mmap, memory.max=B | 129.1 | 1.167 | - | 0.65 | 1922 | achieved 1244 MiB/s (< 3396 ceiling, no host-cache flag) |
| 0.25 | C | lru | 144.4 | 1.305 | 2817 | 0.82 | 1337 |  |
| 0.25 | D | layer_order_declared, prefetch off | 126.1 | 1.140 | 2499 | 0.91 | 1359 |  |
| 0.25 | E | prefetch on (fallback) | 166.5 | 1.505 | 1605 | 1.02 | - | COLLAPSES in default config; Phase 3 fallback protect-off+retention-none |
| 0.25 | OPT | belady, declared seq, 65 passes | 110.6 | 1.000 | - | - | - |  |
| 0.375 | A | kernel mmap, memory.max=B | 110.4 | 1.225 | - | 0.79 | 1496 | achieved 1298 MiB/s (< 3396 ceiling, no host-cache flag) |
| 0.375 | C | lru | 143.2 | 1.589 | 2753 | 0.85 | 1325 |  |
| 0.375 | D | layer_order_declared, prefetch off | 98.4 | 1.091 | 2030 | 1.20 | 942 |  |
| 0.375 | E | declared + prefetch d2 pinned | 111.1 | 1.233 | 895 | 1.33 | 892 |  |
| 0.375 | OPT | belady, declared seq, 65 passes | 90.1 | 1.000 | - | - | - |  |
| 0.5 | A | kernel mmap, memory.max=B | 111.3 | 1.532 | - | 0.62 | 1820 | achieved 1020 MiB/s (< 3396 ceiling, no host-cache flag) |
| 0.5 | C | lru | 134.5 | 1.852 | 2561 | 0.81 | 1511 |  |
| 0.5 | D | layer_order_declared, prefetch off | 79.3 | 1.092 | 1614 | 1.57 | 680 |  |
| 0.5 | E | declared + prefetch d2 pinned | 86.2 | 1.187 | 757 | 1.24 | 970 |  |
| 0.5 | OPT | belady, declared seq, 65 passes | 72.6 | 1.000 | - | - | - |  |
| 0.625 | A | kernel mmap, memory.max=B | 105.1 | 1.904 | - | 0.64 | 1687 | achieved 999 MiB/s (< 3396 ceiling, no host-cache flag) |
| 0.625 | C | lru | 134.5 | 2.438 | 2561 | 0.72 | 1657 |  |
| 0.625 | D | layer_order_declared, prefetch off | 60.6 | 1.098 | 1236 | 1.51 | 766 |  |
| 0.625 | E | declared + prefetch d2 pinned | 64.7 | 1.173 | 623 | 1.56 | 741 |  |
| 0.625 | OPT | belady, declared seq, 65 passes | 55.2 | 1.000 | - | - | - |  |
| 0.75 | A | kernel mmap, memory.max=B | 83.3 | 2.175 | - | 0.75 | 1876 | achieved 927 MiB/s (< 3396 ceiling, no host-cache flag) |
| 0.75 | C | lru | 134.5 | 3.511 | 2561 | 0.73 | 1542 |  |
| 0.75 | D | layer_order_declared, prefetch off | 43.4 | 1.132 | 871 | 1.95 | 596 |  |
| 0.75 | E | declared + prefetch d2 pinned | 46.2 | 1.207 | 431 | 2.28 | 588 |  |
| 0.75 | OPT | belady, declared seq, 65 passes | 38.3 | 1.000 | - | - | - |  |
