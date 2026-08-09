CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread
LDFLAGS = -pthread

all: test_startup

test_startup: test_startup.c region.c region.h
	$(CC) $(CFLAGS) -o test_startup test_startup.c region.c $(LDFLAGS)

clean:
	rm -f test_startup *.o

.PHONY: all clean
