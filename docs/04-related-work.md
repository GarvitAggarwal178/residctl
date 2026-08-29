# Related work and novelty

## §1 — Application-controlled memory management: the idea

That the application, not the kernel, is often the right place to decide which
pages stay resident is not new. Mach's external pagers (Young et al., 1987) let a
user task back a memory object and service its faults. Appel & Li (ASPLOS 1991)
catalogued the virtual-memory primitives — user fault handlers, protection
changes, dirty-bit access — that user-level memory managers need, and showed a
range of applications (concurrent GC, checkpointing, distributed shared memory)
that beat kernel defaults when given them. Harty & Cheriton (ASPLOS 1992)
proposed application-controlled physical memory: an external page-cache manager
that the kernel consults for eviction decisions. The exokernel (Engler et al.,
SOSP 1995) took the position to its conclusion — the kernel should multiplex
hardware and export it, leaving policy (including page replacement) to library
operating systems.

The position — the application frequently knows its own reference pattern and
should be allowed to act on it — was accepted in principle and is decades old.
**It is not this project's contribution.**

## §2 — What Linux shipped, and why it is insufficient

Mainline Linux exposes three levers, and none of them is a residency-control
mechanism for a large read-only working set under memory pressure:

- `madvise(MADV_WILLNEED / MADV_DONTNEED / MADV_SEQUENTIAL / MADV_RANDOM)` and
  `posix_fadvise` are **advisory in the retain direction**. `MADV_DONTNEED` frees
  pages immediately, but nothing tells the kernel to *keep* a specific page when
  it later needs to reclaim; the readahead and LRU heuristics remain in charge.
  `cache_ext` (2025) measured exactly this — the hint interface is a weak lever;
  the kernel's own policy dominates the outcome.
- `MADV_PAGEOUT` can force a specific range out, but it returns `EINVAL` on an
  `mlock`'d range, so it cannot be combined with pinning to express "keep this
  set, drop that set."
- `mlock` is **binding but one-directional**: it pins, and it cannot express
  eviction, cannot rank a working set, and cannot oversubscribe — an `mlock`'d
  region that exceeds the cgroup limit is an OOM, not a managed spill.

So a workload whose working set exceeds its budget and whose reference order is
known has no in-tree way to say "these are the pages, this is the order, evict
against it." That gap is what this project fills.

## §3 — Contemporary approaches, and where each sits

| approach | where control resides | binding? | why it does not cover this position |
|---|---|---|---|
| Classical kernel replacement (LRU/CLOCK/MGLRU) | kernel | binding | no model of the future; degenerates on a cyclic scan (this project, Claim 1) |
| `cache_ext` (2025) | eBPF program in the kernel's eviction path | binding | programs the *kernel's* eviction; still the kernel's page cache, still subject to its readahead and accounting. A different point than owning the pages. |
| `sched_ext` | eBPF in the scheduler | binding | the scheduling analogue — same "program the kernel's policy" model, not "the app owns the resource" |
| DAMON | kernel, access-frequency monitor + `DAMOS` actions | binding-ish | reactive (observes access, then acts); no notion of a declared future order |
| vLLM / PagedAttention, FlexGen, Pie | the inference framework, in user space | binding (framework owns its buffers) | manages the framework's *own* allocations (KV cache, activation tensors); does not give the OS-level weight working set a residency policy — it assumes the weights fit or are streamed |
| LLM-in-a-Flash, P2Cache, ssd-llm | the inference engine + a bespoke weight loader | binding | weight streaming with engine-specific prefetch; a vertical solution inside one engine, not a reusable OS mechanism, and not evaluated against a computable optimum |

The unoccupied position: **a general OS-level mechanism that gives a read-only
mmap'd working set an application-authored residency policy, binding against the
kernel, reusable across engines.**

## §4 — `userfaultfd`'s established uses, and the gap

`userfaultfd` is in production for two things: post-copy live migration (resolve
a guest's fault by pulling the page from the source host) and CRIU lazy restore
(resolve from a checkpoint image). Both resolve a fault from a **remote or
snapshot source**, on demand, once.

It has **not** been reported as a residency-control mechanism for a large,
local, read-only working set — repeatedly faulting the same file-backed pages in
and punching them back out under a budget, with the application choosing the
victim. The interface is also still moving: the January 2026 `vm_uffd_ops` RFC
generalises the fault-handling hooks but is scoped to anonymous, shmem and
hugetlb memory. This project's use — `UFFDIO_CONTINUE` over a double-mapped
`memfd`, with `FALLOC_FL_PUNCH_HOLE` eviction — sits just inside what the current
interface allows and just outside what it was built for.

## §5 — The contribution, stated precisely

1. **The mechanism composition:** `userfaultfd` over a double-mapped `memfd`,
   with `FALLOC_FL_PUNCH_HOLE` eviction under `memory.swap.max = 0`, giving the
   application a residency decision the kernel cannot override — demonstrated
   causally (with `swap.max = 0` the kernel enters direct reclaim 37 times and
   steals zero pages; with swap it steals ~60 k).
2. **The evaluation against a computable optimum**, which this workload uniquely
   permits because its reference string — a cyclic transformer layer scan — is
   known in advance, so Belady's MIN is exact rather than an approximation.
3. **A declared-order policy that reaches that optimum** (D/OPT = 1.000 on the
   synthetic path with an accurate consumption signal; 1.09–1.14 on a real 3B
   model) — the concrete form of the old claim that application knowledge beats
   kernel inference.
4. **End-to-end validation:** byte-identical output from llama.cpp with the pager
   substituted for `mmap`, and throughput that scales 2.1× with budget where the
   kernel's own policy gives none.

## §6 — What we do not claim

- We did not invent application-controlled memory management (§1) or
  `userfaultfd` (§4).
- We do not claim generality beyond **one dense 3B model on one CPU-only WSL2
  VM**. No larger models, no MoE, no GPU offload, no bare metal.
- The prefetching component **does not pay on real inference** — real per-layer
  compute is ~30× lighter than the synthetic regime where prefetch won on bytes,
  and at a tight budget prefetch is a net loss (it livelocks with current-chunk
  protection on).
- The declared-order determinism guarantee holds only where the workload's
  consumption signal is exact; on the real model it lags, and a heuristic
  compensates.
- The host-cache and I/O-ceiling questions are unresolved without bare-metal
  measurement, which is out of scope.

Naming these is what makes §5 credible.

---
*≈ 900 words. Sources: the citations named in `docs/overnight/final session.md`
§3, `docs/MECHANISM_SPEC.md`, and this project's own measured results
(`CLAIMS.md`, `phase1_equal_budget.md`, `phase2_consumption_signal.md`,
`phase1_deadlock_fix.md`, spike S3d/S3e). No new literature search.*
