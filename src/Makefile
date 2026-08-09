CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread
LDFLAGS = -pthread

all: test_startup test_pager test_eviction gen_pattern

test_startup: test_startup.c region.c region.h
	$(CC) $(CFLAGS) -o test_startup test_startup.c region.c $(LDFLAGS)

test_pager: test_pager.c pager.c fetch.c budget.c region.c region.h pager.h fetch.h budget.h
	$(CC) $(CFLAGS) -o test_pager test_pager.c pager.c fetch.c budget.c region.c $(LDFLAGS)

test_eviction: test_eviction.c pager.c fetch.c budget.c region.c region.h pager.h fetch.h budget.h
	$(CC) $(CFLAGS) -o test_eviction test_eviction.c pager.c fetch.c budget.c region.c $(LDFLAGS)

gen_pattern: gen_pattern.c
	$(CC) $(CFLAGS) -o gen_pattern gen_pattern.c

clean:
	rm -f test_startup test_pager test_eviction gen_pattern *.o

.PHONY: all clean
