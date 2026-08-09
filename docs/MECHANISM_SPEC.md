# MECHANISM SPEC — `residctl`

Application-authoritative page residency for large read-only working sets.

**Status:** all spike gates passed. This spec is the implementation contract.
Every design decision below traces to a measured result in
`/root/spike/results/`. Where a number appears, it came from the spike.

**Audience:** this is written to be handed to a code-generating assistant *and*
to be the thing generated code is checked against. The Invariants section is the
part that matters most — code that violates an invariant is wrong even if it
runs.

---

## 0. SCOPE

**In scope:** the pager mechanism, three replacement policies, the trace
recorder, the offline optimal solver, the trace-replay driver, the measurement
harness.

**Out of scope, do not build:** llama.cpp integration (final stage, separate
spec), multi-region support, huge pages, write support, network-backed fetch,
non-root operation.

**Build order is fixed** (§12). Do not reorder. The replay driver comes before
the engine integration so a complete result exists even if integration runs long.

---

## 1. INVARIANTS

Violating any of these produces silently wrong results, not crashes. Each must
be enforced by an assertion in code, not by convention.

**I-1 — The handler never touches the registered mapping.**
All writes go through mapping B (unregistered). A handler read or write through
mapping A deadlocks the handler against itself. Enforce by keeping the mapping A
pointer out of every handler-side struct; the handler struct holds only `map_b`.

**I-2 — The uffd is opened `O_NONBLOCK` and `EAGAIN` is the only authoritative
empty-queue signal.**
`poll()` readiness is advisory. A single `UFFDIO_CONTINUE` resolving many waiters
dequeues messages a `poll()`-trusting handler was about to read, and a blocking
`read()` then hangs forever. This is not a hypothetical — it hung 5/5 spike runs
before diagnosis. Drain loop: `read()` until `-1/EAGAIN`.

**I-3 — `memory.swap.max` must read exactly `0` at startup, or initialisation
fails.**
S3e measured 236 MiB of the region swapped out in every run when swap was
available. Without this setting the kernel evicts weights while our accounting
believes them resident. Read the file, compare, `abort()` with a clear message.
Do not warn and continue.

**I-4 — A chunk is never punched while in `FETCHING`, and never punched while it
is the current or prefetch-target chunk.**
Punching a `FETCHING` chunk races the population write. Punching the chunk under
active compute causes immediate refault and can livelock. The policy layer must
respect a pin set; the mechanism must assert it.

**I-5 — `UFFDIO_CONTINUE` is issued only after every byte of the chunk exists in
the memfd.**
`CONTINUE` maps pages that already exist. Issuing it over a range containing a
hole fails or maps short. Loop the read until the full chunk length is populated,
then check `uffdio_continue.mapped == chunk_len` and abort on mismatch.

**I-6 — Chunk state transitions happen only under that chunk's lock, and the
lock is held across the entire fetch (read + `CONTINUE` + state update).**

**I-7 — Our resident-bytes accounting is reconciled against
`memory.stat[shmem]` at every policy decision point.**
If they diverge by more than one chunk, abort. This is the only defence against
an eviction that silently didn't happen.

**I-8 — Fault handling dispatches on chunk *state*, never on fault *type*.**
Both `MISSING` and `MINOR` occur in normal operation: `MISSING` for an absent or
punched chunk (S3b confirmed post-punch faults are `MISSING`), `MINOR` for a
chunk whose pages exist in the memfd but whose PTEs are not yet installed — which
happens to any thread faulting during the population window. Record the type as a
metric; do not branch on it.

**I-9 — THP is disabled on both mappings via `MADV_NOHUGEPAGE`.**
This system already has `shmem_enabled=[never]`, but bare metal commonly differs.
Large-folio splitting under hole-punching would make eviction non-deterministic,
which attacks the thesis directly. Assert, don't assume.

**I-10 — No policy decision may be made from data a teammate produced.**
Carried forward from the project rules. The harness validates its own inputs.

---

## 2. MEASURED CONSTRAINTS

These numbers shape the design and should not be re-derived.

| Quantity | Measured | Implication |
|---|---|---|
| `O_DIRECT pread`, 150 MiB | 61 ms median (2450 MiB/s) | The fetch cost. Dominates. |
| `UFFDIO_CONTINUE`, 150 MiB | 2.9 ms median (50,900 MiB/s) | 4.5% of fetch. Negligible. |
| `UFFDIO_COPY`, 150 MiB | 29.0 ms (5164 MiB/s) | Would add 48% to every fetch. Rejected. |
| Buffered `pread`, 150 MiB | 128 ms median, 6× spread | Rejected; also pollutes page cache. |
| `mapped` on single 150 MiB `CONTINUE` | exactly 157,286,400, 5/5 | Chunk-granular resolution confirmed. |
| Hole punch, 400 MiB | exact, no delay | Eviction is synchronous. |
| Logical block size, `/dev/sdd` | 512 B | Align to 4096 for margin. |

**Consequence:** the resolution ioctl is not a cost centre. Optimise the read
path and the policy; do not micro-optimise `CONTINUE`.

---

## 3. DATA STRUCTURES

```c
typedef enum { CHUNK_ABSENT = 0, CHUNK_FETCHING, CHUNK_RESIDENT } chunk_state_t;

typedef struct {
    uint64_t      file_off;      // offset in the source model file
    uint64_t      region_off;    // offset within the mapped region
    uint64_t      len;           // bytes; == aligned chunk size except possibly last
    uint32_t      layer_id;      // for the layer-order policy
    chunk_state_t state;
    uint64_t      last_fault_seq;  // for LRU
    uint32_t      pin;             // >0 => never evict (I-4)
    pthread_mutex_t lock;          // guards state + fetch (I-6)
    pthread_cond_t  cv;            // for waiters on FETCHING
} chunk_t;

typedef struct {
    int        memfd;
    uint8_t   *map_a;            // registered; HANDLER MUST NOT TOUCH (I-1)
    uint8_t   *map_b;            // unregistered; population path
    uint64_t   region_len;
    int        uffd;             // O_NONBLOCK (I-2)
    int        model_fd;         // O_DIRECT
    chunk_t   *chunks;           // sorted by region_off
    uint32_t   n_chunks;
    uint64_t   resident_bytes;   // our accounting (I-7)
    uint64_t   budget_bytes;
    uint64_t   fault_seq;        // monotonic, for LRU and trace ordering
    policy_t  *policy;
    trace_t   *trace;
    metrics_t *metrics;
} region_t;
```

**Lookup:** binary search on `region_off` over a sorted, static, non-overlapping
array. A few hundred entries; sub-microsecond against a 61 ms fetch. Do not build
an interval tree.

---

## 4. STARTUP SEQUENCE

Order matters. Each step has an assertion.

1. **Verify cgroup.** Read `memory.swap.max` → must be `"0"` (I-3). Read
   `memory.max`; record. Read `memory.high` if set. Abort with a specific message
   on any failure.
2. **Verify uffd features.** `userfaultfd(O_CLOEXEC|O_NONBLOCK)`, `UFFDIO_API`
   with `features=0`, assert `UFFD_FEATURE_MISSING_SHMEM` and
   `UFFD_FEATURE_MINOR_SHMEM` present. (Spike measured `0x1ffff` — all seven.)
3. **Create backing store.** `memfd_create("residctl", MFD_CLOEXEC)`,
   `ftruncate(region_len)`. Do **not** `fallocate` the whole thing — that would
   make the entire model resident at startup.
4. **Map twice.** `map_b = mmap(..., PROT_READ|PROT_WRITE, MAP_SHARED, memfd, 0)`,
   then `map_a = mmap(...)` same flags, same fd. Two distinct virtual addresses,
   one backing object.
5. **`madvise(MADV_NOHUGEPAGE)` on both** (I-9).
6. **Register mapping A** with `UFFDIO_REGISTER_MODE_MISSING |
   UFFDIO_REGISTER_MODE_MINOR`. Assert success.
7. **Open the model file** `O_RDONLY|O_DIRECT`. If `O_DIRECT` fails, fall back to
   buffered + `POSIX_FADV_DONTNEED` after each read and **record the fallback in
   the run manifest** — it changes the copy count and must appear in results.
8. **Build the chunk table** from the model's tensor metadata.
9. **Compute the budget.** `budget = memory.max − N_peak − margin`, where `N_peak`
   is the measured non-weight footprint from a pilot run and
   `margin = max(128 MiB, 0.2 × N_peak)`. Pre-allocate all handler scratch and
   metadata *now* so the pager's own footprint cannot spike later.
10. **Start the handler thread.** One thread. Multi-threading it is a cut item
    (§12) and must be justified by measured handler queue depth, not assumed.
11. **Write the run manifest**: kernel version, `shmem_enabled`, `memory.max`,
    `swap.max`, chunk size, policy name, `O_DIRECT` yes/no, model hash, git SHA.
    Every run. No exceptions — this is how the WSL2-vs-bare-metal comparison stays
    honest.

---

## 5. HANDLER LOOP

```
loop:
  poll(uffd, POLLIN, timeout)          # advisory only
  loop:
    n = read(uffd, &msg, sizeof msg)
    if n < 0 and errno == EAGAIN: break       # THE authoritative signal (I-2)
    if n < 0: fatal
    assert msg.event == UFFD_EVENT_PAGEFAULT
    metrics.record_fault(msg.arg.pagefault.flags)   # record type, don't branch (I-8)
    chunk = lookup(msg.arg.pagefault.address)
    handle(chunk)
```

`handle(chunk)`:

```
lock(chunk)
switch (chunk->state):
  RESIDENT:
      # already resolved, possibly by a CONTINUE covering a different fault
      ioctl(UFFDIO_WAKE, chunk range)
      metrics.dedup_hit++
  FETCHING:
      # another fetch in flight; its CONTINUE will wake this thread
      metrics.dedup_hit++
      # drop the message; do NOT wait, do NOT issue I/O
  ABSENT:
      chunk->state = FETCHING
      chunk->last_fault_seq = ++region->fault_seq
      trace.record(chunk->id, region->fault_seq)
      policy->on_fault(chunk)                    # may request evictions
      ensure_budget(chunk->len)                  # §7
      fetch(chunk)                               # §6, lock held (I-6)
      chunk->state = RESIDENT
      region->resident_bytes += chunk->len
      policy->on_resident(chunk)
      maybe_prefetch(chunk)                      # §6.3
unlock(chunk)
```

**Note on the `RESIDENT` case:** it is reachable and normal. It is the S2
finding's other face — many threads fault into one chunk, one `CONTINUE` resolves
all, but their messages were already queued. `UFFDIO_WAKE` is harmless and cheap.

---

## 6. FETCH PATH

### 6.1 Read

`O_DIRECT` requires offset, length, and buffer address aligned. Chunk boundaries
come from tensor offsets and will not be aligned.

```
aligned_start = align_down(chunk->file_off, 4096)
aligned_end   = align_up(chunk->file_off + chunk->len, 4096)
dest          = map_b + chunk->region_off - (chunk->file_off - aligned_start)
```

`dest` must itself be 4096-aligned. Since `region_off` and `file_off` share the
same relative layout, choose the chunk table so that `region_off ≡ file_off (mod
4096)`. **Simplest correct approach: align all chunk boundaries to 4096 when
building the table**, absorbing the slack into the preceding chunk. Do this at
table-construction time and the alignment problem disappears from the hot path.

`pread` in a loop until the full aligned range is populated. Short reads are
legal; handle them.

### 6.2 Resolve

```
struct uffdio_continue c = { .range = { chunk->region_off + region->map_a,
                                        chunk->len }, .mode = 0 };
ioctl(uffd, UFFDIO_CONTINUE, &c);
assert(c.mapped == chunk->len);        # I-5
```

`mode = 0` wakes waiters. Use `UFFDIO_CONTINUE_MODE_DONTWAKE` only if you later
batch resolutions, which is not in scope.

### 6.3 Prefetch

On transition of chunk *N* to `RESIDENT`, if the policy exposes a successor
prediction, enqueue chunk *N+1* for asynchronous fetch. One outstanding prefetch
maximum in v1.

Prefetch pins its target (I-4) until it completes or is cancelled. A prefetched
chunk goes to `RESIDENT` without ever generating a fault — **this is where the
performance comes from and it must be counted separately in metrics** (§9), since
it's the D→E delta in the arm table.

---

## 7. EVICTION AND BUDGET

`ensure_budget(need)`:

```
reconcile()                                   # I-7
while (region->resident_bytes + need > region->budget_bytes):
    victim = policy->select_victim()          # returns chunk_id or NONE
    if victim == NONE:
        # policy cannot free enough: infeasible at this budget
        metrics.infeasible++
        return E_INFEASIBLE                   # caller records censored point
    evict(victim)
```

`evict(chunk)`:

```
lock(chunk)
assert(chunk->state == RESIDENT)              # I-4
assert(chunk->pin == 0)                       # I-4
fallocate(memfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
          chunk->region_off, chunk->len)
chunk->state = ABSENT
region->resident_bytes -= chunk->len
metrics.evictions++; metrics.bytes_punched += chunk->len
unlock(chunk)
```

`reconcile()`: read `memory.stat[shmem]`, compare to `resident_bytes + known
overhead`. Divergence beyond one chunk → abort (I-7).

**Correctness note worth having ready for viva:** evicting a chunk a compute
thread is actively reading is *correct* — the read faults, the handler refetches,
the thread gets right data. It is a performance and liveness problem, not a
correctness one. The pin set exists to prevent the pathology, not to preserve
correctness.

**Note on the pager's own overhead:** `MAP_SHARED|MAP_ANONYMOUS` allocations are
shmem-backed and charge to `memory.stat[shmem]` — the spike caught a 4096-byte
step from exactly this. Any such allocation must be counted in the known
overhead term or `reconcile()` will false-positive.

---

## 8. POLICY INTERFACE

```c
typedef struct {
    const char *name;
    void      (*on_fault)(region_t*, chunk_t*);
    void      (*on_resident)(region_t*, chunk_t*);
    uint32_t  (*select_victim)(region_t*);      // CHUNK_NONE if none evictable
    int32_t   (*predict_next)(region_t*, chunk_t*);  // -1 if no prediction
} policy_t;
```

Three implementations:

| Policy | `select_victim` | `predict_next` | Purpose |
|---|---|---|---|
| `lru` | max age by `last_fault_seq` | −1 | **Control.** Same rule as the kernel. Isolates authority from policy. Predicted to tie the baseline. |
| `layer_order` | chunk whose next use is furthest, from the known cyclic order | `chunk_id + 1` | The informed policy. |
| `belady` | — | — | Offline only; never runs live. §10. |

`layer_order` must not hardcode "next = +1" in `select_victim`; it computes next-use
distance from the recorded cyclic order so the same code works if the access
pattern turns out to be less regular than assumed.

---

## 9. TRACE AND METRICS

**Trace record** (binary, append-only, one per fault, written from the handler):

```
{ uint64 seq; uint64 timestamp_ns; uint32 chunk_id; uint8 fault_type;
  uint8 was_prefetched; uint16 _pad; }
```

This is the reference string. It feeds the Belady solver and it is the only
legitimate source for it — derived from actual faults, never from declared access
order, or the bound becomes circular.

**Cross-arm metrics** (identical collection in both arms):

- **`/proc/PID/io:read_bytes` per generated token — PRIMARY.** Arm-independent.
- Tokens/s, TTFT, p99 inter-token latency.
- `memory.peak`, `memory.events[high, max, oom, oom_kill]`.
- `memory.pressure` PSI `some`/`full` avg10.

**Baseline-arm only:** `maj_flt` from `/proc/PID/stat`; from `memory.stat`:
`pgscan`, `pgsteal`, `workingset_refault_file`. Read `memory.stat` **once per
sample into a single buffer** and extract all fields from it — the spike logged
two transient bare-vs-split mismatches from five separate `fopen()` calls racing
live counters.

**Treatment-arm only:** fault count by type; dedup hit count; bytes fetched vs
served-resident; **prefetch hits (faults avoided)**; eviction count and bytes
punched; handler service latency histogram (HDR); handler queue depth high-water.

Do **not** use `pgmajfault` as a cross-arm metric. In the treatment arm the
handler performs the I/O, so the kernel records minor faults and it reads near
zero regardless of performance.

---

## 10. OFFLINE OPTIMAL SOLVER

Separate binary. Input: trace file + budget. Output: minimum achievable fault
count and bytes.

Standard Belady: build a next-use index over the reference string (single reverse
pass), then simulate with a max-heap keyed on next-use distance. O(n log k).

Must accept the same budget parameter as the live runs so the bound is computed
against the identical capacity.

Sanity check that must pass before any result is reported: on a strictly cyclic
reference string at budget ratio *r*, the solver must return approximately
`(1−r) × W` bytes per pass. If it doesn't, the solver is wrong, not the theory.

---

## 11. HARNESS AND ARMS

Six arms, one binary, flag-selected:

| Arm | Config |
|---|---|
| A | `mmap` baseline, `madvise` swept, best config reported |
| B | A + `MADV_WILLNEED`/`MADV_PAGEOUT` hints |
| C | pager, `lru`, prefetch off |
| D | pager, `layer_order`, prefetch off |
| E | pager, `layer_order`, prefetch on |
| OPT | offline solver over D's trace |

**Every arm runs in a cgroup with identical `memory.max`.** Sweep the budget
ratio across at least three values. Report the sensitivity curve, not a single
tuned point.

**Pre-registered censoring rule — write this into the methods section before the
first run:** an OOM kill or `E_INFEASIBLE` at budget *B* is recorded as *policy
infeasible at B* and plotted as a censored point. It is not discarded. The
discriminator between a finding and a bug: if the pager's accounting said it was
under budget and `memory.stat[shmem]` disagreed, that's a bug — fix and rerun.
If the accounting was correct and the budget was simply infeasible, that's the
data point.

**Machine exclusivity is a hard rule.** No CN project, no other workload, nothing
on the machine during a run. The spike was contaminated by exactly this.

---

## 12. BUILD ORDER

Fixed. Do not reorder. Hours are the estimate to beat.

| # | Item | Est. |
|---|---|---|
| 1 | Region setup + startup assertions (§4) | 2h |
| 2 | Handler loop + chunk table + state machine (§3, §5) | 4h |
| 3 | Fetch path with alignment (§6.1–6.2) | 3h |
| 4 | Eviction + budget + reconcile (§7) | 2h |
| 5 | Trace recorder + metrics (§9) | 2h |
| 6 | Trace-replay driver | 2h |
| 7 | `lru` and `layer_order` policies (§8) | 2h |
| 8 | Prefetch (§6.3) | 1h |
| 9 | Belady solver (§10) | 2h |
| 10 | Harness, arms, sweep (§11) | 3h |
| — | **Subtotal** | **23h** |
| 11 | llama.cpp integration (separate spec) | 7h, stretch |

**Cut ladder, in cut order, if 23h doesn't fit:**
1. Multi-threaded handler → stays single-threaded, report queue depth as a
   limitation. (Already assumed; do not build it in the first place.)
2. `O_DIRECT` → buffered + `POSIX_FADV_DONTNEED`. −2h, costs one copy.
3. Four budget points → three. −1h of run time.
4. llama.cpp integration → replay driver is the permanent primary workload. −7h.

---

## 13. CORRECTNESS HARNESS

Before any performance number is reported, these must pass:

- **T-1 Data integrity.** Fill the model file with a position-derived pattern.
  Read the entire region through mapping A under a budget of 25% of region size,
  verifying every byte. Any mismatch means eviction or fetch is wrong. Run under
  all three policies.
- **T-2 Punch-refetch cycle.** Fetch a chunk, evict it, refetch, verify contents
  identical. 1000 iterations.
- **T-3 Concurrent fault storm.** 8 threads randomly touching the region under a
  tight budget for 60s. No hangs, no corruption, `resident_bytes` never exceeds
  budget, `reconcile()` never trips.
- **T-4 Accounting.** After an arbitrary sequence of fetches and evictions,
  `resident_bytes` matches `memory.stat[shmem]` minus known overhead exactly.
- **T-5 Belady sanity.** §10's cyclic-string check.

T-1 and T-3 are the ones that catch a broken eviction path. Do not skip them
because the thing "seems to work" — a punched hole reads as zeros, which is a
plausible-looking wrong tensor, not a crash.
