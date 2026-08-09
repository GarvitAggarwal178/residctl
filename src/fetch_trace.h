// fetch_trace.h -- item 10b Task A: per-fetch wall-clock breakdown.
//
// Distinct from trace.h's TRACE_TYPE_REFERENCE/TRACE_TYPE_FAULT traces
// (item 5/item 10 correction) -- this is a diagnostic-only record kept in
// a preallocated in-memory array and written to disk ONCE AT EXIT, never
// from inside the handler, so the measurement doesn't perturb the thing
// being measured. Not part of MECHANISM_SPEC's data model; a debugging
// tool, gated behind --fetch-trace.
#ifndef RESIDCTL_FETCH_TRACE_H
#define RESIDCTL_FETCH_TRACE_H

#include <stdint.h>
#include <pthread.h>

typedef struct {
    uint64_t seq;
    uint32_t chunk_id;
    uint8_t was_prefetch;
    uint8_t _pad[3];
    uint64_t t_handler_entry_ns;
    uint64_t t_read_start_ns;
    uint64_t t_read_end_ns;
    uint64_t t_continue_end_ns;
    uint64_t t_handler_exit_ns;
    uint64_t bytes_read;
} __attribute__((packed)) fetch_trace_record_t;

// Tag must be `fetch_trace` to match region.h's forward declaration
// (`typedef struct fetch_trace fetch_trace_t;`).
typedef struct fetch_trace {
    fetch_trace_record_t *records;
    uint64_t capacity;
    uint64_t count;       // next free slot; advanced under lock
    uint64_t next_seq;
    pthread_mutex_t lock; // protects count/next_seq -- needed once Task B's
                           // worker pool can call fetch_trace_reserve() from
                           // multiple threads concurrently
    char path[256];
} fetch_trace_t;

// Preallocates `capacity` records. Aborts if capacity is exceeded at
// runtime (a diagnostic run should size this generously; silently
// truncating would misreport the measurement).
fetch_trace_t *fetch_trace_open(const char *path, uint64_t capacity);

// Thread-safe. Reserves and returns a pointer to the next record slot,
// pre-filled with a monotonic seq. Caller fills in the rest.
fetch_trace_record_t *fetch_trace_reserve(fetch_trace_t *ft);

// Writes all reserved records to `path` as a flat binary array (no
// header -- this is a diagnostic tool consumed by its own analysis
// script, not part of the project's trace format). Call once, after the
// handler thread(s) have stopped.
void fetch_trace_flush(fetch_trace_t *ft);

void fetch_trace_close(fetch_trace_t *ft);

#endif
