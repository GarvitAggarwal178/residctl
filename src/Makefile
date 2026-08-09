CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread
LDFLAGS = -pthread

CORE_SRC = pager.c fetch.c budget.c trace.c metrics.c cgroup_stat.c region.c
CORE_HDR = region.h pager.h fetch.h budget.h trace.h metrics.h cgroup_stat.h

all: test_startup test_pager test_eviction test_trace gen_pattern

test_startup: test_startup.c region.c region.h
	$(CC) $(CFLAGS) -o test_startup test_startup.c region.c $(LDFLAGS)

test_pager: test_pager.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_pager test_pager.c $(CORE_SRC) $(LDFLAGS)

test_eviction: test_eviction.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_eviction test_eviction.c $(CORE_SRC) $(LDFLAGS)

test_trace: test_trace.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o test_trace test_trace.c $(CORE_SRC) $(LDFLAGS)

gen_pattern: gen_pattern.c
	$(CC) $(CFLAGS) -o gen_pattern gen_pattern.c

clean:
	rm -f test_startup test_pager test_eviction test_trace gen_pattern *.o

.PHONY: all clean
