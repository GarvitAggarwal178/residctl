CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread
LDFLAGS = -pthread

CORE_SRC = pager.c fetch.c budget.c trace.c metrics.c cgroup_stat.c replay.c policy.c region.c
CORE_HDR = region.h pager.h fetch.h budget.h trace.h metrics.h cgroup_stat.h replay.h policy.h

all: test_startup test_pager test_eviction test_trace test_replay test_policy replay_main gen_pattern

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

replay_main: replay_main.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o replay_main replay_main.c $(CORE_SRC) $(LDFLAGS)

gen_pattern: gen_pattern.c
	$(CC) $(CFLAGS) -o gen_pattern gen_pattern.c

clean:
	rm -f test_startup test_pager test_eviction test_trace test_replay test_policy replay_main gen_pattern *.o

.PHONY: all clean
