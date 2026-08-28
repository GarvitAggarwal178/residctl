// residctl_llama.h -- WP2 shim between llama.cpp's model loader and the
// residctl userfaultfd pager. See residctl_llama.c.
//
// Wiring: third_party/llama.cpp/src/llama-mmap.cpp, when the environment
// variable RESIDCTL_CONFIG is set, resolves residctl_llama_mmap via
// dlsym(RTLD_DEFAULT, ...) and calls it instead of mmap()ing the model
// file. The config file named by RESIDCTL_CONFIG supplies the cgroup,
// budget, policy, and prefetch settings.
#ifndef RESIDCTL_LLAMA_H
#define RESIDCTL_LLAMA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called by the patched llama_mmap::impl constructor. Reads $RESIDCTL_CONFIG,
// parses the GGUF at the configured model path, builds a per-transformer-layer
// chunk table, brings up the residctl region + pager thread, and returns the
// userfaultfd-registered mapping base (region.map_a). The mapping is
// file_size rounded up to 4096; tensor pointers computed as base + file_offset
// are serviced by the pager. Returns MAP_FAILED-equivalent ((void*)-1) on any
// failure (the caller throws).
void *residctl_llama_mmap(int llama_fd, size_t file_size);

// Fire the workload's per-layer consumption signal (pager_notify_access) for
// the chunk backing transformer layer `layer`. Called from the eval callback
// once per layer transition. layer < 0 or out of range is ignored.
void residctl_llama_notify_layer(int layer);

// Fire the consumption signal for a non-layer chunk: role 0 = token_embd,
// 1 = output_norm, 2 = output. Called from the eval callback on the
// inp_embd / result_norm / result_output nodes.
void residctl_llama_notify_role(int role);

// Number of transformer layers discovered in the GGUF (blk.N.* groups).
int residctl_llama_n_layers(void);

// Stop the pager, print one machine-parseable RESIDCTL_STATS line to stdout,
// and tear the region down. Safe to call once; a no-op if the pager was never
// started.
void residctl_llama_teardown(void);

// Write the parsed tensor inventory (name, file offset, size) and the derived
// chunk table to `path`. For the Phase 2.1 report. Call after
// residctl_llama_mmap.
void residctl_llama_write_inventory(const char *path);

#ifdef __cplusplus
}
#endif

#endif
