# The problem — why kernel paging fails on this workload

## The workload

A large language model's weights are a read-only working set that does not fit
in the memory budget you want to give it. Inference reads them in a **fixed,
cyclic order**: layer 0, layer 1, …, layer N, then back to layer 0 for the next
token, forever. Every byte of a layer's weights is read on every visit. The
access sequence is known in advance — the application authored it.

## What the kernel does with it

The kernel's page cache manages residency by an LRU-style rule: when it needs to
free a page, it evicts the one used least recently. On a cyclic scan whose
working set exceeds the budget, this is exactly wrong. The least-recently-used
page is the one the scan just passed — which, on a cycle, is also the one it
will return to *soonest*. LRU evicts the page it is about to need and keeps the
pages it won't touch for a full cycle.

The result, measured (see [`findings.md`](../results/findings.md) step 1): a
**~100 % miss rate at every budget ratio**, including budgets that hold most of
the model. The kernel reads the entire model from disk on every scan. More
memory does not help — the miss rate stays at 1.0 until the budget holds the
whole working set, at which point the problem disappears entirely. There is no
gradient for the kernel to climb.

`madvise` hints (`MADV_WILLNEED`, `MADV_DONTNEED`, `MADV_SEQUENTIAL`) let the
application nudge the kernel, but they are advisory and coarse — they cannot
express "keep chunk 7 resident and evict chunk 3 *because chunk 3's next use is
further away*." The kernel has no interface to accept a next-use ordering, and
no mechanism to act on one if it did.

## The claim

The application knows its access order in advance; the kernel must infer the
future from the past. If the application is given **authority** over which pages
stay resident — a real mechanism, enforced, not a hint — it can hold the right
pages and read far less from disk, close to the offline optimum, and turn extra
memory budget into throughput the way the kernel cannot.

`residctl` is that mechanism, built from primitives that already ship in Linux:
`userfaultfd` (`UFFDIO_CONTINUE`) to serve pages on demand, a double-mapped
`memfd` so the application can populate pages without faulting itself,
`FALLOC_FL_PUNCH_HOLE` to evict authoritatively, and cgroup v2
`memory.swap.max = 0` so the kernel cannot quietly undo an eviction by swapping.
No kernel module, no patched kernel.

The rest: [`02-design.md`](02-design.md) for the mechanism,
[`03-methodology.md`](03-methodology.md) for how the claim is tested,
[`findings.md`](../results/findings.md) for what the tests showed.
