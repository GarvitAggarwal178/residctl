CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread
LDFLAGS = -pthread

CORE_SRC = pager.c fetch.c budget.c trace.c metrics.c cgroup_stat.c replay.c policy.c prefetch.c prefetch_pool.c fetch_trace.c region.c
CORE_HDR = region.h pager.h fetch.h budget.h trace.h metrics.h cgroup_stat.h replay.h policy.h prefetch.h prefetch_pool.h fetch_trace.h

all: test_startup test_pager test_eviction test_trace test_replay test_policy test_prefetch test_correctness test_storm test_t6 test_t7 replay_main belady_main baseline_main gen_pattern bench_concurrent_read

test_startup: test_startup.c region.c region.h
	$(CC) $(CFLAGS) -o test_startup test_startup.c region.c $(LDFLAGS)

test_pager: test_pager.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_pager test_pager.c $(CORE_SRC) $(LDFLAGS)

test_eviction: test_eviction.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_eviction test_eviction.c $(CORE_SRC) $(LDFLAGS)

test_trace: test_trace.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_trace test_trace.c $(CORE_SRC) $(LDFLAGS)

test_replay: test_replay.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_replay test_replay.c $(CORE_SRC) $(LDFLAGS)

test_policy: test_policy.c region.c region.h policy.c policy.h
	$(CC) $(CFLAGS) -o test_policy test_policy.c region.c policy.c $(LDFLAGS)

test_prefetch: test_prefetch.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_prefetch test_prefetch.c $(CORE_SRC) $(LDFLAGS)

replay_main: replay_main.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o replay_main replay_main.c $(CORE_SRC) $(LDFLAGS)

belady_main: belady_main.c belady.c belady.h trace.h trace.c
	$(CC) $(CFLAGS) -o belady_main belady_main.c belady.c trace.c $(LDFLAGS)

baseline_main: baseline_main.c cgroup_stat.c cgroup_stat.h
	$(CC) $(CFLAGS) -o baseline_main baseline_main.c cgroup_stat.c $(LDFLAGS)

test_correctness: test_correctness.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_correctness test_correctness.c $(CORE_SRC) $(LDFLAGS)

test_storm: test_storm.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_storm test_storm.c $(CORE_SRC) $(LDFLAGS)

test_t6: test_t6.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_t6 test_t6.c $(CORE_SRC) $(LDFLAGS)

test_t7: test_t7.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_t7 test_t7.c $(CORE_SRC) $(LDFLAGS)

gen_pattern: gen_pattern.c
	$(CC) $(CFLAGS) -o gen_pattern gen_pattern.c

bench_concurrent_read: bench_concurrent_read.c
	$(CC) $(CFLAGS) -o bench_concurrent_read bench_concurrent_read.c $(LDFLAGS)

clean:
	rm -f test_startup test_pager test_eviction test_trace test_replay test_policy test_prefetch test_correctness test_storm test_t6 test_t7 replay_main belady_main baseline_main gen_pattern bench_concurrent_read *.o

.PHONY: all clean
