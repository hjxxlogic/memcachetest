CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

all: cache_bench

cache_bench: cache_bench.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f cache_bench
