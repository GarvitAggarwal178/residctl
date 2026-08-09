// replay.h -- build-order item 6: the trace-replay driver.
#ifndef RESIDCTL_REPLAY_H
#define RESIDCTL_REPLAY_H

#include "region.h"

typedef struct {
    uint64_t wall_ns;
    uint32_t n_passes;
    uint32_t n_touches; // n_passes * n_chunks
} replay_result_t;

// Cyclic layer-order replay: n_passes full sweeps over chunks
// 0..n_chunks-1, touching each chunk once via map_a per sweep. This is the
// "known cyclic order" access pattern §8's layer_order policy and §9's
// Belady solver are built around -- LLM inference traverses layers in the
// same fixed order every token/pass. Until build-order item 11 (llama.cpp
// integration) exists, this synthetic pattern IS the trace-replay driver's
// primary workload, per §12: "the replay driver comes before the engine
// integration so a complete result exists even if integration runs long."
//
// Caller must have already run region_startup() and started the pager
// thread; this function only issues the touches and times them.
replay_result_t replay_cyclic(region_t *r, uint32_t n_passes);

#endif
