#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static double now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint64_t get_size_ioctl(int fd)
{
	uint64_t sz = 0;
	if (ioctl(fd, 0, &sz) != 0)
		return 0;
	return sz;
}

static void bench_one(const char *path, size_t size_bytes, int iters)
{
	int fd;
	void *map;
	volatile uint64_t *p;
	size_t n64;
	int iter;
	uint64_t i;
	double t0, t1;
	uint64_t sum = 0;

	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		exit(1);
	}

	if (!size_bytes) {
		uint64_t sz = get_size_ioctl(fd);
		if (!sz) {
			fprintf(stderr, "size not provided and ioctl failed for %s\n", path);
			exit(1);
		}
		size_bytes = (size_t)sz;
	}

	map = mmap(NULL, size_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap %s failed: %s\n", path, strerror(errno));
		exit(1);
	}

	n64 = size_bytes / sizeof(uint64_t);
	p = (volatile uint64_t *)map;

	t0 = now_sec();
	for (iter = 0; iter < iters; iter++) {
		for (i = 0; i < n64; i++)
			p[i] = (uint64_t)(i + (uint64_t)iter);
	}
	t1 = now_sec();
	{
		double dt = t1 - t0;
		double bytes = (double)size_bytes * (double)iters;
		printf("%s write: %.2f MB/s (%.3f s)\n", path, (bytes / (1024.0 * 1024.0)) / dt, dt);
	}

	t0 = now_sec();
	for (iter = 0; iter < iters; iter++) {
		for (i = 0; i < n64; i++)
			sum += p[i];
	}
	t1 = now_sec();
	{
		double dt = t1 - t0;
		double bytes = (double)size_bytes * (double)iters;
		printf("%s read : %.2f MB/s (%.3f s) sum=%" PRIu64 "\n", path,
		       (bytes / (1024.0 * 1024.0)) / dt, dt, sum);
	}

	munmap(map, size_bytes);
	close(fd);
}

static void usage(const char *argv0)
{
	fprintf(stderr, "Usage: %s [-s size_mb] [-i iters]\n", argv0);
	fprintf(stderr, "Default size uses kernel module ioctl.\n");
}

int main(int argc, char **argv)
{
	size_t size_bytes = 0;
	int iters = 50;
	int opt;

	while ((opt = getopt(argc, argv, "s:i:h")) != -1) {
		switch (opt) {
		case 's':
			size_bytes = (size_t)strtoul(optarg, NULL, 0) * 1024 * 1024;
			break;
		case 'i':
			iters = atoi(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return 1;
		}
	}

	bench_one("/dev/memcache_wb", size_bytes, iters);
	bench_one("/dev/memcache_uc", size_bytes, iters);
	bench_one("/dev/memcache_wc", size_bytes, iters);
	return 0;
}
