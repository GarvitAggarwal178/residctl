// trace.h -- MECHANISM_SPEC.md §9 trace recorder.
#ifndef RESIDCTL_TRACE_H
#define RESIDCTL_TRACE_H

#include <stdint.h>

// The reference string. Written from the handler, one record per genuine
// ABSENT->FETCHING transition (i.e. once per real fault-driven fetch, never
// per dedup hit). This is the ONLY legitimate source for the offline Belady
// solver (item 9) -- derived from actual faults, never from declared access
// order, or the bound becomes circular.
typedef struct {
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t chunk_id;
    uint8_t fault_type;      // TRACE_FAULT_MISSING or TRACE_FAULT_MINOR
    uint8_t was_prefetched;  // always 0 until build-order item 8
    uint16_t _pad;
} __attribute__((packed)) trace_record_t;

#define TRACE_FAULT_MISSING 0
#define TRACE_FAULT_MINOR   1

// Tag must be `trace` to match region.h's forward declaration
// (`typedef struct trace trace_t;`) -- both headers get included together
// in pager.c, and a mismatched anonymous struct here would conflict.
typedef struct trace {
    int fd;
    uint64_t records_written;
} trace_t;

// Opens (creates/truncates) path for append-only binary trace writing.
// Aborts on failure to open -- a run whose trace can't be written shouldn't
// proceed silently, since the trace is the only legitimate Belady input.
trace_t *trace_open(const char *path);

// Aborts on a failed/partial write for the same reason.
void trace_record(trace_t *t, uint64_t seq, uint32_t chunk_id, uint8_t fault_type, uint8_t was_prefetched);

void trace_close(trace_t *t);

#endif
