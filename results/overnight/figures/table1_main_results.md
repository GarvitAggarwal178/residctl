# Table 1 — Main results

read_bytes per touch (MiB) = pager_bytes_fetched / total references. total fetches = demand faults + prefetches. arm/OPT = read_bytes / OPT bytes.

Flag key: **x** = excluded, non-deterministic arm D cell (Campaign 13 Phase A — the number shown is one sample from a distribution, not stable). **X** = non-deterministic under WP1 declared order (WP1 §1.3, same three-factor trigger). **h** = arm A/B host-cache contaminated (achieved bandwidth > 3396 MiB/s O_DIRECT ceiling — Windows VHDX host cache, out of scope to defeat). **s** = superseded on this metric by the declared-order row below it (WP1). **d** = WP1 declared-order policy (`layer_order_declared`).

| chunk | ratio | arm | policy | compute | read_bytes_per_touch_MiB | total_fetches | demand_faults | wall_s | arm/OPT | flags |
|---|---|---|---|---|---|---|---|---|---|---|
| 8MiB | 0.25 | A | madvise:normal | n/a | 8.06 | n/a | n/a | 3.931 | 1.260 |  |
| 8MiB | 0.25 | B | madvise+hints | n/a | 7.97 | n/a | n/a | 3.812 | 1.245 |  |
| 8MiB | 0.25 | C | lru | 0 | 8.00 | 1280 | 1280 | 8.301 | 1.250 |  |
| 8MiB | 0.25 | D | layer_order_learned | 0 | 6.85 | 1096 | 1096 | 7.435 | 1.070 | s |
| 8MiB | 0.25 | E | layer_order_learned | 0 | 7.96 | 1255 | 695 | 6.221 | 1.243 | s |
| 8MiB | 0.25 | C | lru | 400000 | 8.00 | 1280 | 1280 | 8.753 | 1.250 |  |
| 8MiB | 0.25 | D | layer_order_learned | 400000 | 9.15 | 1464 | 1464 | 10.216 | 1.430 | x |
| 8MiB | 0.25 | E | layer_order_learned | 400000 | 8.29 | 1322 | 772 | 7.441 | 1.295 |  |
| 8MiB | 0.25 | D | layer_order_declared | 0 | 6.41 | 1025 | 1025 | 4.501 | 1.001 | d |
| 8MiB | 0.25 | E | layer_order_declared | 0 | 7.04 | 1126 | 451 | 3.884 | 1.100 | d |
| 8MiB | 0.25 | D | layer_order_declared | 400000 | 12.18 | 1949 | 1949 | 8.805 | 1.903 | dX |
| 8MiB | 0.25 | E | layer_order_declared | 400000 | 8.39 | 1342 | 664 | 5.249 | 1.311 | d |
| 8MiB | 0.25 | OPT | belady | n/a | 6.40 | 1024 | 1024 | n/a | 1.000 |  |
| 8MiB | 0.375 | A | madvise:sequential | n/a | 8.00 | n/a | n/a | 3.415 | 1.429 |  |
| 8MiB | 0.375 | B | madvise+hints | n/a | 7.97 | n/a | n/a | 3.002 | 1.423 | h |
| 8MiB | 0.375 | C | lru | 0 | 8.00 | 1280 | 1280 | 8.437 | 1.429 |  |
| 8MiB | 0.375 | D | layer_order_learned | 0 | 6.21 | 994 | 994 | 6.326 | 1.109 |  |
| 8MiB | 0.375 | E | layer_order_learned | 0 | 6.59 | 1056 | 614 | 4.699 | 1.176 |  |
| 8MiB | 0.375 | C | lru | 400000 | 8.00 | 1280 | 1280 | 7.729 | 1.429 |  |
| 8MiB | 0.375 | D | layer_order_learned | 400000 | 7.96 | 1273 | 1273 | 6.820 | 1.421 | x |
| 8MiB | 0.375 | E | layer_order_learned | 400000 | 6.55 | 1048 | 606 | 5.699 | 1.170 |  |
| 8MiB | 0.375 | OPT | belady | n/a | 5.60 | 896 | 896 | n/a | 1.000 |  |
| 8MiB | 0.5 | A | madvise:normal | n/a | 8.00 | n/a | n/a | 3.836 | 1.668 |  |
| 8MiB | 0.5 | B | madvise+hints | n/a | 7.97 | n/a | n/a | 3.121 | 1.660 |  |
| 8MiB | 0.5 | C | lru | 0 | 8.00 | 1280 | 1280 | 6.397 | 1.667 |  |
| 8MiB | 0.5 | D | layer_order_learned | 0 | 5.62 | 900 | 900 | 4.457 | 1.172 | s |
| 8MiB | 0.5 | E | layer_order_learned | 0 | 5.60 | 896 | 533 | 4.932 | 1.167 | s |
| 8MiB | 0.5 | C | lru | 400000 | 8.00 | 1280 | 1280 | 9.539 | 1.667 |  |
| 8MiB | 0.5 | D | layer_order_learned | 400000 | 6.87 | 1099 | 1099 | 8.298 | 1.431 |  |
| 8MiB | 0.5 | E | layer_order_learned | 400000 | 7.02 | 1123 | 638 | 6.760 | 1.462 |  |
| 8MiB | 0.5 | D | layer_order_declared | 0 | 4.80 | 768 | 768 | 3.393 | 1.000 | d |
| 8MiB | 0.5 | E | layer_order_declared | 0 | 4.96 | 796 | 292 | 3.054 | 1.033 | d |
| 8MiB | 0.5 | D | layer_order_declared | 400000 | 8.57 | 1372 | 1372 | 7.153 | 1.786 | dX |
| 8MiB | 0.5 | E | layer_order_declared | 400000 | 6.15 | 984 | 478 | 4.415 | 1.281 | d |
| 8MiB | 0.5 | OPT | belady | n/a | 4.80 | 768 | 768 | n/a | 1.000 |  |
| 8MiB | 0.625 | A | madvise:sequential | n/a | 8.00 | n/a | n/a | 3.905 | 2.000 |  |
| 8MiB | 0.625 | B | madvise+hints | n/a | 7.97 | n/a | n/a | 2.982 | 1.992 | h |
| 8MiB | 0.625 | C | lru | 0 | 8.00 | 1280 | 1280 | 8.695 | 2.000 |  |
| 8MiB | 0.625 | D | layer_order_learned | 0 | 5.01 | 802 | 802 | 4.753 | 1.253 |  |
| 8MiB | 0.625 | E | layer_order_learned | 0 | 4.83 | 773 | 488 | 3.877 | 1.208 |  |
| 8MiB | 0.625 | C | lru | 400000 | 8.00 | 1280 | 1280 | 7.903 | 2.000 |  |
| 8MiB | 0.625 | D | layer_order_learned | 400000 | 5.66 | 906 | 906 | 5.453 | 1.416 |  |
| 8MiB | 0.625 | E | layer_order_learned | 400000 | 5.19 | 831 | 516 | 4.577 | 1.298 |  |
| 8MiB | 0.625 | OPT | belady | n/a | 4.00 | 640 | 640 | n/a | 1.000 |  |
| 8MiB | 0.75 | A | madvise:sequential | n/a | 8.00 | n/a | n/a | 2.865 | 2.500 | h |
| 8MiB | 0.75 | B | madvise+hints | n/a | 7.97 | n/a | n/a | 2.547 | 2.490 | h |
| 8MiB | 0.75 | C | lru | 0 | 8.00 | 1280 | 1280 | 6.228 | 2.500 |  |
| 8MiB | 0.75 | D | layer_order_learned | 0 | 4.01 | 641 | 641 | 3.337 | 1.252 | s |
| 8MiB | 0.75 | E | layer_order_learned | 0 | 3.68 | 594 | 411 | 2.776 | 1.150 | s |
| 8MiB | 0.75 | C | lru | 400000 | 8.00 | 1280 | 1280 | 7.599 | 2.500 |  |
| 8MiB | 0.75 | D | layer_order_learned | 400000 | 4.29 | 687 | 687 | 4.505 | 1.342 |  |
| 8MiB | 0.75 | E | layer_order_learned | 400000 | 3.81 | 621 | 432 | 3.574 | 1.189 |  |
| 8MiB | 0.75 | D | layer_order_declared | 0 | 3.20 | 512 | 512 | 2.688 | 1.000 | d |
| 8MiB | 0.75 | E | layer_order_declared | 0 | 3.23 | 516 | 184 | 2.135 | 1.008 | d |
| 8MiB | 0.75 | D | layer_order_declared | 400000 | 5.06 | 810 | 810 | 4.633 | 1.582 | dX |
| 8MiB | 0.75 | E | layer_order_declared | 400000 | 3.87 | 619 | 281 | 3.030 | 1.209 | d |
| 8MiB | 0.75 | OPT | belady | n/a | 3.20 | 512 | 512 | n/a | 1.000 |  |
| 128MiB | 0.25 | A | madvise:sequential | n/a | 128.00 | n/a | n/a | 2.673 | 1.231 | h |
| 128MiB | 0.25 | B | madvise+hints | n/a | 121.50 | n/a | n/a | 2.438 | 1.168 | h |
| 128MiB | 0.25 | C | lru | 0 | 128.00 | 80 | 80 | 4.678 | 1.231 |  |
| 128MiB | 0.25 | D | layer_order_learned | 0 | 113.60 | 71 | 71 | 3.946 | 1.092 | s |
| 128MiB | 0.25 | E | layer_order_learned | 0 | 155.20 | 98 | 60 | 4.334 | 1.492 | s |
| 128MiB | 0.25 | C | lru | 400000 | 128.00 | 80 | 80 | 5.389 | 1.231 |  |
| 128MiB | 0.25 | D | layer_order_learned | 400000 | 161.60 | 101 | 101 | 6.210 | 1.554 | x |
| 128MiB | 0.25 | E | layer_order_learned | 400000 | 204.80 | 128 | 81 | 6.353 | 1.969 |  |
| 128MiB | 0.25 | D | layer_order_declared | 0 | 104.00 | 65 | 65 | 3.943 | 1.000 | d |
| 128MiB | 0.25 | E | layer_order_declared | 0 | 169.60 | 106 | 53 | 4.139 | 1.631 | d |
| 128MiB | 0.25 | D | layer_order_declared | 400000 | 208.00 | 130 | 130 | 7.736 | 2.000 | dX |
| 128MiB | 0.25 | E | layer_order_declared | 400000 | 172.80 | 108 | 53 | 5.461 | 1.662 | d |
| 128MiB | 0.25 | OPT | belady | n/a | 104.00 | 65 | 65 | n/a | 1.000 |  |
| 128MiB | 0.375 | A | madvise:sequential | n/a | 128.00 | n/a | n/a | 2.969 | 1.429 | h |
| 128MiB | 0.375 | B | madvise+hints | n/a | 121.50 | n/a | n/a | 3.027 | 1.356 |  |
| 128MiB | 0.375 | C | lru | 0 | 128.00 | 80 | 80 | 5.303 | 1.429 |  |
| 128MiB | 0.375 | D | layer_order_learned | 0 | 100.80 | 63 | 63 | 3.698 | 1.125 |  |
| 128MiB | 0.375 | E | layer_order_learned | 0 | 136.00 | 85 | 50 | 3.913 | 1.518 |  |
| 128MiB | 0.375 | C | lru | 400000 | 128.00 | 80 | 80 | 5.520 | 1.429 |  |
| 128MiB | 0.375 | D | layer_order_learned | 400000 | 142.40 | 89 | 89 | 5.887 | 1.589 | x |
| 128MiB | 0.375 | E | layer_order_learned | 400000 | 158.40 | 98 | 60 | 5.307 | 1.768 |  |
| 128MiB | 0.375 | OPT | belady | n/a | 89.60 | 56 | 56 | n/a | 1.000 |  |
| 128MiB | 0.5 | A | madvise:sequential | n/a | 128.00 | n/a | n/a | 2.887 | 1.667 | h |
| 128MiB | 0.5 | B | madvise+hints | n/a | 121.50 | n/a | n/a | 2.731 | 1.582 | h |
| 128MiB | 0.5 | C | lru | 0 | 128.00 | 80 | 80 | 6.003 | 1.667 |  |
| 128MiB | 0.5 | D | layer_order_learned | 0 | 91.20 | 57 | 57 | 3.657 | 1.188 | s |
| 128MiB | 0.5 | E | layer_order_learned | 0 | 120.00 | 74 | 46 | 3.774 | 1.562 | s |
| 128MiB | 0.5 | C | lru | 400000 | 128.00 | 80 | 80 | 5.835 | 1.667 |  |
| 128MiB | 0.5 | D | layer_order_learned | 400000 | 128.00 | 80 | 80 | 5.571 | 1.667 | x |
| 128MiB | 0.5 | E | layer_order_learned | 400000 | 129.60 | 79 | 48 | 4.863 | 1.688 |  |
| 128MiB | 0.5 | D | layer_order_declared | 0 | 76.80 | 48 | 48 | 3.164 | 1.000 | d |
| 128MiB | 0.5 | E | layer_order_declared | 0 | 131.20 | 82 | 43 | 3.364 | 1.708 | d |
| 128MiB | 0.5 | D | layer_order_declared | 400000 | 145.60 | 91 | 91 | 6.148 | 1.896 | dX |
| 128MiB | 0.5 | E | layer_order_declared | 400000 | 126.40 | 80 | 47 | 4.602 | 1.646 | d |
| 128MiB | 0.5 | OPT | belady | n/a | 76.80 | 48 | 48 | n/a | 1.000 |  |
| 128MiB | 0.625 | A | madvise:normal | n/a | 128.30 | n/a | n/a | 2.733 | 2.005 | h |
| 128MiB | 0.625 | B | madvise+hints | n/a | 121.50 | n/a | n/a | 3.258 | 1.898 |  |
| 128MiB | 0.625 | C | lru | 0 | 128.00 | 80 | 80 | 5.658 | 2.000 |  |
| 128MiB | 0.625 | D | layer_order_learned | 0 | 81.60 | 51 | 51 | 3.549 | 1.275 |  |
| 128MiB | 0.625 | E | layer_order_learned | 0 | 104.00 | 66 | 42 | 3.704 | 1.625 |  |
| 128MiB | 0.625 | C | lru | 400000 | 128.00 | 80 | 80 | 5.934 | 2.000 |  |
| 128MiB | 0.625 | D | layer_order_learned | 400000 | 94.40 | 59 | 59 | 4.628 | 1.475 |  |
| 128MiB | 0.625 | E | layer_order_learned | 400000 | 113.60 | 71 | 49 | 4.814 | 1.775 |  |
| 128MiB | 0.625 | OPT | belady | n/a | 64.00 | 40 | 40 | n/a | 1.000 |  |
| 128MiB | 0.75 | A | madvise:sequential | n/a | 128.00 | n/a | n/a | 3.199 | 2.500 |  |
| 128MiB | 0.75 | B | madvise+hints | n/a | 121.50 | n/a | n/a | 2.764 | 2.373 | h |
| 128MiB | 0.75 | C | lru | 0 | 128.00 | 80 | 80 | 5.317 | 2.500 |  |
| 128MiB | 0.75 | D | layer_order_learned | 0 | 65.60 | 41 | 41 | 2.650 | 1.281 | s |
| 128MiB | 0.75 | E | layer_order_learned | 0 | 84.80 | 53 | 36 | 2.934 | 1.656 | s |
| 128MiB | 0.75 | C | lru | 400000 | 128.00 | 80 | 80 | 5.932 | 2.500 |  |
| 128MiB | 0.75 | D | layer_order_learned | 400000 | 80.00 | 50 | 50 | 4.003 | 1.562 |  |
| 128MiB | 0.75 | E | layer_order_learned | 400000 | 75.20 | 48 | 37 | 4.463 | 1.469 |  |
| 128MiB | 0.75 | D | layer_order_declared | 0 | 51.20 | 32 | 32 | 2.793 | 1.000 | d |
| 128MiB | 0.75 | E | layer_order_declared | 0 | 67.20 | 42 | 22 | 2.223 | 1.312 | d |
| 128MiB | 0.75 | D | layer_order_declared | 400000 | 76.80 | 48 | 48 | 3.832 | 1.500 | dX |
| 128MiB | 0.75 | E | layer_order_declared | 400000 | 73.60 | 46 | 26 | 3.377 | 1.438 | d |
| 128MiB | 0.75 | OPT | belady | n/a | 51.20 | 32 | 32 | n/a | 1.000 |  |
