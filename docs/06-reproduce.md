# Reproduce

From a clean clone. The repo is ~5 MB of source, reports, and figures; the
model, the pattern files, and llama.cpp are regenerated (they are gitignored —
`.gitignore` excludes `models/`, `scratch/`, `third_party/`).

## 0. Environment

One Linux host with cgroup v2, root, and a real block device. The project's own
data is all from one WSL2 VM (16 cores / 7 GiB / kernel
6.18-microsoft-standard-WSL2); a bare-metal run has never been done — see
[`05-limitations.md`](05-limitations.md) and
[`bare-metal-plan.md`](bare-metal-plan.md).

Create the measurement cgroup with `memory.swap.max = 0` (invariant I-3 — init
aborts otherwise):

```
mkdir /sys/fs/cgroup/residctl && echo 0 > /sys/fs/cgroup/residctl/memory.swap.max
```

## 1. Build the synthetic side

```
cd src && make          # pager, replay driver, belady_main, the test binaries
```

Regenerate the pattern files the harness reads:

```
src/gen_pattern scratch/pattern_16m.bin 16777216      # T-1..T-7
src/gen_pattern scratch/pattern_2g.bin  2147483648     # the learned-policy gate
```

## 2. Run the correctness harness

```
bash scripts/run-correctness-harness.sh     # T-1..T-5, --eager-reconcile
bash scripts/run-storm-t6-t7.sh             # T-6, T-7
```

All must report `mismatches = 0` and PASS before any performance number is
trusted.

## 3. The real model

| item | value |
|---|---|
| file | `Qwen2.5-3B-Instruct-GGUF`, `q4_k_m` quant |
| size | 2,104,932,768 B |
| sha256 | `626b4a6678b86442240e33df819e00132d3ba7dddfe1cdc4fbb18e0a9615c62d` |
| layout | 435 tensors / 36 layers; 41 chunks (0.01–243 MiB); tensors stored name-lexicographic, not layer order; layer 21 split across two non-contiguous chunks |

Download it to `models/model.gguf` and verify the sha256. Then:

```
bash scripts/setup-llama-cpp.sh              # clones/patches/builds llama.cpp under third_party/
bash scripts/build-real-model-integration.sh # builds src/wp2_gen (the ~35-line dlsym hook is src/wp2_llama_mmap.patch)
bash scripts/run-real-model-correctness-gate.sh   # mmap vs residctl -> byte-identical tokens
```

## 4. A worked example — regenerate Figure 7

```
bash scripts/run-livelock-real-model.sh      # -> results/data/livelock-real-model-arms.csv  (~30 min, machine-exclusive)
python3 tools/make_figures.py figure7        # -> results/figures/07-throughput-scaling.png + .csv
```

The `run-livelock-*` scripts check `pgrep cn-spike|iperf3|gate5` before and
after and print the machine load — do not run them alongside anything else.

## 5. What each other script produces

See [`../scripts/README.md`](../scripts/README.md).

## Notes

- **`$?` / `$var` in inline `wsl.exe -- bash -lc '…'` strings** get expanded by
  the Git Bash layer before `wsl` runs (they yield 0 / empty). Run scripts from
  a file (`bash path/script.sh`), not inline, when exit codes matter.
- The `pgrep -af "cn-spike|iperf3|gate5"` exclusivity check matches its own
  command line — use `pgrep -af "[c]n-spike|…"` when checking by hand.
