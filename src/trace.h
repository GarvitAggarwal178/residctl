// trace.h -- MECHANISM_SPEC.md §9 trace recorder.
//
// SPEC AMENDMENT A-1 (item 10 correction): there are now two distinct kinds
// of trace, and they are NOT interchangeable:
//
//   TRACE_TYPE_REFERENCE -- the ground-truth reference string, authored by
//     the WORKLOAD (the replay driver, or a future llama.cpp integration).
//     Every access, hit or miss, in order. This is the ONLY legitimate
//     input to item 9's offline Belady solver.
//
//   TRACE_TYPE_FAULT -- the pager's own view, authored by the HANDLER.
//     One record per genuine ABSENT->FETCHING transition. A pager only
//     ever observes misses (a hit generates no uffd event at all), so this
//     is a function of whichever policy was running, not the workload's
//     true reference string. It remains useful for metrics, dedup, and
//     prefetch accounting -- it is simply never fed to the solver. Feeding
//     a fault trace to the solver computes "the best you could do given
//     the references this policy happened to miss on," which is circular
//     and was V1's Defect 1.
//
// Every trace file starts with a trace_header_t naming which kind it is.
// belady_main aborts if handed a FAULT trace -- see belady_main.c.
#ifndef RESIDCTL_TRACE_H
#define RESIDCTL_TRACE_H

#include <stdint.h>

#define TRACE_MAGIC "RTRC"
#define TRACE_TYPE_REFERENCE 0
#define TRACE_TYPE_FAULT     1

typedef struct {
    char magic[4];     // TRACE_MAGIC, for basic file-sanity checking
    uint8_t trace_type; // TRACE_TYPE_REFERENCE or TRACE_TYPE_FAULT
    uint8_t version;    // format version, currently 2 (post-correction)
    uint16_t _pad;
} __attribute__((packed)) trace_header_t;

#define TRACE_FORMAT_VERSION 2

// The reference string. Written once per fault-trace record OR once per
// reference-trace record, same layout either way.
//
// For TRACE_TYPE_REFERENCE traces, fault_type and was_prefetched are not
// applicable (a reference isn't a fault) and are always TRACE_NA -- the
// workload doesn't know or care whether an access was a hit or a miss;
// that's the pager's business, not the reference string's.
typedef struct {
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t chunk_id;
    uint8_t fault_type;      // TRACE_FAULT_MISSING, TRACE_FAULT_MINOR, or TRACE_NA
    uint8_t was_prefetched;  // 0, 1, or TRACE_NA
    uint16_t _pad;
} __attribute__((packed)) trace_record_t;

#define TRACE_FAULT_MISSING 0
#define TRACE_FAULT_MINOR   1
#define TRACE_NA            0xFF // "not applicable" -- reference-trace records only

// Tag must be `trace` to match region.h's forward declaration
// (`typedef struct trace trace_t;`) -- both headers get included together
// in pager.c, and a mismatched anonymous struct here would conflict.
typedef struct trace {
    int fd;
    uint8_t trace_type;
    uint64_t records_written;
} trace_t;

// Opens (creates/truncates) path for append-only binary trace writing and
// writes the header immediately. Aborts on failure -- a run whose trace
// can't be written shouldn't proceed silently.
trace_t *trace_open(const char *path, uint8_t trace_type);

// Aborts on a failed/partial write for the same reason.
void trace_record(trace_t *t, uint64_t seq, uint32_t chunk_id, uint8_t fault_type, uint8_t was_prefetched);

void trace_close(trace_t *t);

// Reads and validates the header at the front of an already-open fd
// (positioned at offset 0). Returns 0 and sets *out_type on success; -1 on
// I/O error or bad magic/version. Leaves the fd positioned right after the
// header, ready for the caller to read trace_record_t entries.
int trace_read_header(int fd, uint8_t *out_type);

#endif
