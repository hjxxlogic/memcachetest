CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

all: cache_bench

cache_bench: cache_bench.c aa.c
	$(CC) $(CFLAGS) -o $@ cache_bench.c aa.c

clean:
	rm -f cache_bench
