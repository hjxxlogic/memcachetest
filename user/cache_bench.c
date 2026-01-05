#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <string.h>
#include <sys/ioctl.h>
#if defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MEMCACHE_IOCTL_GET_SIZE 0

#if defined(__i386__) || defined(__x86_64__)
static __inline__ __attribute__((always_inline)) uint64_t rdtsc_ordered(void)
{
	unsigned int lo, hi;
	asm volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
	return ((uint64_t)hi << 32) | lo;
}

static double tsc_hz;

static void tsc_init_once(void)
{
	struct timespec a, b, req;
	uint64_t ta, tb;
	double dt;

	if (tsc_hz > 0.0)
		return;

	req.tv_sec = 0;
	req.tv_nsec = 500000000;

	clock_gettime(CLOCK_MONOTONIC_RAW, &a);
	ta = rdtsc_ordered();
	nanosleep(&req, NULL);
	tb = rdtsc_ordered();
	clock_gettime(CLOCK_MONOTONIC_RAW, &b);

	dt = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
	if (dt > 0.0 && tb > ta)
		tsc_hz = (double)(tb - ta) / dt;
}

static __inline__ __attribute__((always_inline)) void nt_store_u64(uint64_t *addr, uint64_t v)
{
#if defined(__x86_64__)
	asm volatile("movnti %1, %0" : "=m"(*addr) : "r"(v) : "memory");
#else
	asm volatile("movnti %1, %0" : "=m"(((uint32_t *)addr)[0]) : "r"((uint32_t)v) : "memory");
	asm volatile("movnti %1, %0" : "=m"(((uint32_t *)addr)[1]) : "r"((uint32_t)(v >> 32)) : "memory");
#endif
}

static __inline__ __attribute__((always_inline)) void nt_store_2x64(uint64_t *addr, uint64_t lo, uint64_t hi)
{
#if defined(__x86_64__) || defined(__SSE2__)
	if ((((uintptr_t)addr) & 0xf) == 0) {
		__m128i v = _mm_set_epi64x((long long)hi, (long long)lo);
		_mm_stream_si128((__m128i *)addr, v);
		return;
	}
#endif
	nt_store_u64(&addr[0], lo);
	nt_store_u64(&addr[1], hi);
}

#if defined(__i386__) || defined(__x86_64__)
static int nt_avx_supported;

static void nt_init_once(void)
{
	static int inited;
	if (inited)
		return;
	inited = 1;

#if defined(__GNUC__)
	if (__builtin_cpu_supports("avx"))
		nt_avx_supported = 1;
#endif
}

__attribute__((target("avx")))
static void nt_store_4x64_avx(uint64_t *addr, uint64_t v0, uint64_t v1, uint64_t v2, uint64_t v3)
{
	__m256i v = _mm256_set_epi64x((long long)v3, (long long)v2, (long long)v1, (long long)v0);
	_mm256_stream_si256((__m256i *)addr, v);
}

static __inline__ __attribute__((always_inline)) void nt_store_4x64(uint64_t *addr, uint64_t v0, uint64_t v1,
						uint64_t v2, uint64_t v3)
{
	if (nt_avx_supported && ((((uintptr_t)addr) & 0x1f) == 0)) {
		nt_store_4x64_avx(addr, v0, v1, v2, v3);
		return;
	}
	nt_store_2x64(&addr[0], v0, v1);
	nt_store_2x64(&addr[2], v2, v3);
}
#endif

static __inline__ __attribute__((always_inline)) void nt_fence(void)
{
	asm volatile("sfence" ::: "memory");
}
#endif

static double now_sec(void)
{
	struct timespec ts;

#if defined(__i386__) || defined(__x86_64__)
	tsc_init_once();
	if (tsc_hz > 0.0) {
		uint64_t t = rdtsc_ordered();
		return (double)t / tsc_hz;
	}
#endif

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint64_t get_size_ioctl(int fd)
{
	uint64_t sz = 0;
	if (ioctl(fd, 0, &sz) != 0)
		return 0;
	return sz;
}

static uint64_t expected_sum_u64(size_t n64, int iter)
{
	__uint128_t n = n64;
	__uint128_t s0 = n * (n - 1) / 2;
	__uint128_t si = (__uint128_t)iter * n;
	__uint128_t s = s0 + si;
	return (uint64_t)s;
}

static volatile uint64_t *uc_fence_word;

static void uc_fence_init(void)
{
	int fd;
	void *map;
	uint64_t sz;
	size_t map_len;
	long page_sz;

	fd = open("/dev/memcache_uc", O_RDWR);
	if (fd < 0)
		return;

	sz = get_size_ioctl(fd);
	page_sz = sysconf(_SC_PAGESIZE);
	if (page_sz <= 0)
		page_sz = 4096;
	map_len = (sz >= (uint64_t)page_sz) ? (size_t)page_sz : (size_t)sz;
	if (map_len < sizeof(uint64_t)) {
		close(fd);
		return;
	}

	map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return;
	}

	uc_fence_word = (volatile uint64_t *)((char *)map + (map_len - sizeof(uint64_t)));
	*uc_fence_word = 0;
	close(fd);
}

static __inline__ __attribute__((always_inline)) void uc_write_fence(uint64_t v)
{
	if (!uc_fence_word)
		return;
	*uc_fence_word = v;
	//(void)*uc_fence_word;
}

static int g_verify_failures;

static void bench_ntwrite_readback(const char *path, size_t size_bytes, int iters, void *map, volatile uint64_t *p,
				 size_t n64)
{
	int nt_supported = 0;
	int failures = 0;
	int iter;
	uint64_t i;
	double t0, t1;
	double dt = 0.0;

	for (iter = 0; iter < iters; iter++) {
#if defined(__i386__) || defined(__x86_64__)
		uint64_t *np = (uint64_t *)map;
		int ok = 1;
		nt_supported = 1;
		t0 = now_sec();
		for (i = 0; i + 3 < n64; i += 4)
			nt_store_4x64(&np[i], (uint64_t)(i + (uint64_t)iter),
						(uint64_t)(i + 1 + (uint64_t)iter),
						(uint64_t)(i + 2 + (uint64_t)iter),
						(uint64_t)(i + 3 + (uint64_t)iter));
		for (; i + 1 < n64; i += 2)
			nt_store_2x64(&np[i], (uint64_t)(i + (uint64_t)iter),
						(uint64_t)(i + 1 + (uint64_t)iter));
		if (i < n64)
			nt_store_u64(&np[i], (uint64_t)(i + (uint64_t)iter));

		for (i = 0; i < n64; i++) {
			uint64_t v = p[i];
			uint64_t expect = (uint64_t)(i + (uint64_t)iter);
			if (v != expect) {
				fprintf(stderr,
					"%s ntwrite_readback verify failed iter=%d idx=%" PRIu64 " got=0x%" PRIx64 " expect=0x%" PRIx64 "\n",
					path, iter, i, v, expect);
				g_verify_failures++;
				failures++;
				ok = 0;
				break;
			}
		}
		t1 = now_sec();
		dt += (t1 - t0);
		if (!ok)
			break;
#else
		break;
#endif
	}

	if (nt_supported) {
		if (!failures)
			printf("%s ntwrite_readback verify: ok\n", path);
		else
			printf("%s ntwrite_readback verify: failed (%d)\n", path, failures);
		{
			double bytes = (double)size_bytes * (double)iters * 2.0;
			printf("%s ntwrite_readback: %.2f MB/s (%.3f s)\n", path,
			       (bytes / (1024.0 * 1024.0)) / dt, dt);
		}
	} else {
		printf("%s ntwrite_readback: unsupported arch\n", path);
	}
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

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		exit(1);
	}

	if (!size_bytes) {
		uint64_t sz = get_size_ioctl(fd);
		if (!sz) {
			fprintf(stderr, "%s ioctl size failed\n", path);
			close(fd);
			return;
		}
		size_bytes = (size_t)sz;
		printf("%s size: %zu bytes (%.2f MiB) source=ioctl\n", path, size_bytes,
		       (double)size_bytes / (1024.0 * 1024.0));
	} else {
		printf("%s size: %zu bytes (%.2f MiB) source=arg\n", path, size_bytes,
		       (double)size_bytes / (1024.0 * 1024.0));
	}

#if defined(__i386__) || defined(__x86_64__)
	nt_init_once();
#endif

	map = mmap(NULL, size_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "%s mmap failed: %s\n", path, strerror(errno));
		close(fd);
		exit(1);
	}

	n64 = size_bytes / sizeof(uint64_t);
	p = (volatile uint64_t *)map;
	{
		int fail_before = g_verify_failures;
		double dt = 0.0;
		for (iter = 0; iter < iters; iter++) {
			int ok = 1;
			t0 = now_sec();
			for (i = 0; i < n64; i++)
				p[i] = (uint64_t)(i + (uint64_t)iter);

			nt_fence();
			t1 = now_sec();
			dt += (t1 - t0);

			sum = 0;
			for (i = 0; i < n64; i++)
				sum += p[i];
			{
				uint64_t expect = expected_sum_u64(n64, iter);
				if (sum != expect) {
					fprintf(stderr,
						"%s write verify failed iter=%d sum=0x%" PRIx64 " expect=0x%" PRIx64 "\n",
						path, iter, sum, expect);
					g_verify_failures++;
					ok = 0;
				}
			}
			if (!ok)
				break;
		}

		if (g_verify_failures == fail_before) {
			printf("%s write verify: ok\n", path);
		} else {
			printf("%s write verify: failed\n", path);
		}
		{
			double bytes = (double)size_bytes * (double)iters;
			printf("%s write: %.2f MB/s (%.3f s)\n", path, (bytes / (1024.0 * 1024.0)) / dt, dt);
		}
	}

	{
		int failures = 0;
		double dt = 0.0;
		for (iter = 0; iter < iters; iter++) {
			t0 = now_sec();
			for (i = 0; i < n64; i++)
				p[i] = (uint64_t)(i + (uint64_t)iter);
			t1 = now_sec();
			dt += (t1 - t0);

			sum = 0;
			for (i = 0; i < n64; i++)
				sum += p[i];
			{
				uint64_t expect = expected_sum_u64(n64, iter);
				if (sum != expect) {
					fprintf(stderr,
						"%s write_nofence verify failed iter=%d sum=0x%" PRIx64 " expect=0x%" PRIx64 "\n",
						path, iter, sum, expect);
					g_verify_failures++;
					failures++;
				}
			}
		}

		if (!failures)
			printf("%s write_nofence verify: ok\n", path);
		else
			printf("%s write_nofence verify: failed (%d)\n", path, failures);
		{
			double bytes = (double)size_bytes * (double)iters;
			printf("%s write_nofence: %.2f MB/s (%.3f s)\n", path,
			       (bytes / (1024.0 * 1024.0)) / dt, dt);
		}
	}

	{
		int fail_before = g_verify_failures;
		double dt = 0.0;
		if (!uc_fence_word) {
			printf("%s write_ucfence: uc_fence unavailable\n", path);
		} else {
			int overlap_uc = (strcmp(path, "/dev/memcache_uc") == 0);
			long page_sz = sysconf(_SC_PAGESIZE);
			size_t fence_idx = (((size_t)(page_sz > 0 ? page_sz : 4096)) / sizeof(uint64_t)) - 1;
			if (fence_idx >= n64)
				overlap_uc = 0;
			for (iter = 0; iter < iters; iter++) {
				int ok = 1;
				t0 = now_sec();
				for (i = 0; i < n64; i++) {
					if (overlap_uc && i == fence_idx)
						continue;
					p[i] = (uint64_t)(i + (uint64_t)iter);
				}
				uc_write_fence((uint64_t)iter);
				t1 = now_sec();
				dt += (t1 - t0);

				sum = 0;
				for (i = 0; i < n64; i++) {
					if (overlap_uc && i == fence_idx)
						continue;
					sum += p[i];
				}
				{
					uint64_t expect = expected_sum_u64(n64, iter);
					if (overlap_uc)
						expect -= (uint64_t)(fence_idx + (uint64_t)iter);
					if (sum != expect) {
						fprintf(stderr,
							"%s write_ucfence verify failed iter=%d sum=0x%" PRIx64 " expect=0x%" PRIx64 "\n",
							path, iter, sum, expect);
						g_verify_failures++;
						ok = 0;
					}
				}
				if (!ok)
					break;
			}

			if (g_verify_failures == fail_before) {
				printf("%s write_ucfence verify: ok\n", path);
			} else {
				printf("%s write_ucfence verify: failed\n", path);
			}
			{
				double bytes = (double)size_bytes * (double)iters;
				printf("%s write_ucfence: %.2f MB/s (%.3f s)\n", path,
				       (bytes / (1024.0 * 1024.0)) / dt, dt);
			}
		}
	}

	{
		int nt_supported = 0;
		int fail_before = g_verify_failures;
		double dt = 0.0;
		for (iter = 0; iter < iters; iter++) {
#if defined(__i386__) || defined(__x86_64__)
			uint64_t *np = (uint64_t *)map;
			nt_supported = 1;
			{
				int ok = 1;
				t0 = now_sec();
				for (i = 0; i + 3 < n64; i += 4)
					nt_store_4x64(&np[i], (uint64_t)(i + (uint64_t)iter),
							(uint64_t)(i + 1 + (uint64_t)iter),
							(uint64_t)(i + 2 + (uint64_t)iter),
							(uint64_t)(i + 3 + (uint64_t)iter));
				for (; i + 1 < n64; i += 2)
					nt_store_2x64(&np[i], (uint64_t)(i + (uint64_t)iter),
							(uint64_t)(i + 1 + (uint64_t)iter));
				if (i < n64)
					nt_store_u64(&np[i], (uint64_t)(i + (uint64_t)iter));
				nt_fence();
				t1 = now_sec();
				dt += (t1 - t0);

				sum = 0;
				for (i = 0; i < n64; i++)
					sum += p[i];
				{
					uint64_t expect = expected_sum_u64(n64, iter);
					if (sum != expect) {
						fprintf(stderr,
							"%s ntwrite verify failed iter=%d sum=0x%" PRIx64 " expect=0x%" PRIx64 "\n",
							path, iter, sum, expect);
						g_verify_failures++;
						ok = 0;
					}
				}
				if (!ok)
					break;

			}
#else
			break;
#endif
		}

		if (nt_supported) {
			if (g_verify_failures == fail_before)
				printf("%s ntwrite verify: ok\n", path);
			else
				printf("%s ntwrite verify: failed\n", path);
			{
				double bytes = (double)size_bytes * (double)iters;
				printf("%s ntwrite: %.2f MB/s (%.3f s)\n", path,
				       (bytes / (1024.0 * 1024.0)) / dt, dt);
			}
		} else {
			printf("%s ntwrite: unsupported arch\n", path);
		}
	}

	{
		int nt_supported = 0;
		int failures = 0;
		double dt = 0.0;
		for (iter = 0; iter < iters; iter++) {
#if defined(__i386__) || defined(__x86_64__)
			uint64_t *np = (uint64_t *)map;
			nt_supported = 1;
			t0 = now_sec();
			for (i = 0; i + 3 < n64; i += 4)
				nt_store_4x64(&np[i], (uint64_t)(i + (uint64_t)iter),
							(uint64_t)(i + 1 + (uint64_t)iter),
							(uint64_t)(i + 2 + (uint64_t)iter),
							(uint64_t)(i + 3 + (uint64_t)iter));
			for (; i + 1 < n64; i += 2)
				nt_store_2x64(&np[i], (uint64_t)(i + (uint64_t)iter),
							(uint64_t)(i + 1 + (uint64_t)iter));
			if (i < n64)
				nt_store_u64(&np[i], (uint64_t)(i + (uint64_t)iter));
			t1 = now_sec();
			dt += (t1 - t0);

			sum = 0;
			for (i = 0; i < n64; i++)
				sum += p[i];
			{
				uint64_t expect = expected_sum_u64(n64, iter);
				if (sum != expect) {
					fprintf(stderr,
						"%s ntwrite_nofence verify failed iter=%d sum=0x%" PRIx64 " expect=0x%" PRIx64 "\n",
						path, iter, sum, expect);
					g_verify_failures++;
					failures++;
				}
			}
#else
			break;
#endif
		}

		if (nt_supported) {
			if (!failures)
				printf("%s ntwrite_nofence verify: ok\n", path);
			else
				printf("%s ntwrite_nofence verify: failed (%d)\n", path, failures);
			{
				double bytes = (double)size_bytes * (double)iters;
				printf("%s ntwrite_nofence: %.2f MB/s (%.3f s)\n", path,
				       (bytes / (1024.0 * 1024.0)) / dt, dt);
			}
		} else {
			printf("%s ntwrite_nofence: unsupported arch\n", path);
		}
	}

	bench_ntwrite_readback(path, size_bytes, iters, map, p, n64);

	{
		int nt_supported = 0;
		int failures = 0;
		double dt = 0.0;
		for (iter = 0; iter < iters; iter++) {
#if defined(__i386__) || defined(__x86_64__)
			uint64_t *np = (uint64_t *)map;
			nt_supported = 1;
			t0 = now_sec();
			for (i = 0; i + 3 < n64; i += 4)
				nt_store_4x64(&np[i], (uint64_t)(i + (uint64_t)iter),
							(uint64_t)(i + 1 + (uint64_t)iter),
							(uint64_t)(i + 2 + (uint64_t)iter),
							(uint64_t)(i + 3 + (uint64_t)iter));
			for (; i + 1 < n64; i += 2)
				nt_store_2x64(&np[i], (uint64_t)(i + (uint64_t)iter),
							(uint64_t)(i + 1 + (uint64_t)iter));
			if (i < n64)
				nt_store_u64(&np[i], (uint64_t)(i + (uint64_t)iter));
			t1 = now_sec();
			dt += (t1 - t0);
#else
			break;
#endif
		}

		if (nt_supported) {
			sum = 0;
			for (i = 0; i < n64; i++)
				sum += p[i];
			{
				int last_iter = iters > 0 ? (iters - 1) : 0;
				uint64_t expect = expected_sum_u64(n64, last_iter);
				if (sum != expect) {
					fprintf(stderr,
						"%s ntwrite_nofence_deferred verify failed sum=0x%" PRIx64 " expect=0x%" PRIx64 "\n",
						path, sum, expect);
					g_verify_failures++;
					failures++;
				}
			}

			if (!failures)
				printf("%s ntwrite_nofence_deferred verify: ok\n", path);
			else
				printf("%s ntwrite_nofence_deferred verify: failed (%d)\n", path, failures);
			{
				double bytes = (double)size_bytes * (double)iters;
				printf("%s ntwrite_nofence_deferred: %.2f MB/s (%.3f s)\n", path,
				       (bytes / (1024.0 * 1024.0)) / dt, dt);
			}
		} else {
			printf("%s ntwrite_nofence_deferred: unsupported arch\n", path);
		}
	}

	{
		int nt_supported = 0;
		int fail_before = g_verify_failures;
		double dt = 0.0;
		if (!uc_fence_word) {
			printf("%s ntwrite_ucfence: uc_fence unavailable\n", path);
		} else {
			int overlap_uc = (strcmp(path, "/dev/memcache_uc") == 0);
			long page_sz = sysconf(_SC_PAGESIZE);
			size_t fence_idx = (((size_t)(page_sz > 0 ? page_sz : 4096)) / sizeof(uint64_t)) - 1;
			if (fence_idx >= n64)
				overlap_uc = 0;
			for (iter = 0; iter < iters; iter++) {
#if defined(__i386__) || defined(__x86_64__)
				uint64_t *np = (uint64_t *)map;
				nt_supported = 1;
				{
					int ok = 1;
					t0 = now_sec();
					if (!overlap_uc) {
						for (i = 0; i + 3 < n64; i += 4)
							nt_store_4x64(&np[i], (uint64_t)(i + (uint64_t)iter),
										(uint64_t)(i + 1 + (uint64_t)iter),
										(uint64_t)(i + 2 + (uint64_t)iter),
										(uint64_t)(i + 3 + (uint64_t)iter));
						for (; i + 1 < n64; i += 2)
							nt_store_2x64(&np[i], (uint64_t)(i + (uint64_t)iter),
										(uint64_t)(i + 1 + (uint64_t)iter));
						if (i < n64)
							nt_store_u64(&np[i], (uint64_t)(i + (uint64_t)iter));
					} else {
						for (i = 0; i + 1 < n64; i += 2) {
							if (i == fence_idx || (i + 1) == fence_idx) {
								if (i != fence_idx)
									nt_store_u64(&np[i], (uint64_t)(i + (uint64_t)iter));
								if ((i + 1) != fence_idx)
									nt_store_u64(&np[i + 1], (uint64_t)(i + 1 + (uint64_t)iter));
								continue;
							}
							nt_store_2x64(&np[i], (uint64_t)(i + (uint64_t)iter),
										(uint64_t)(i + 1 + (uint64_t)iter));
						}
						if (n64 & 1) {
							size_t last = n64 - 1;
							if (last != fence_idx)
								nt_store_u64(&np[last], (uint64_t)(last + (uint64_t)iter));
						}
					}
					uc_write_fence((uint64_t)iter);
					t1 = now_sec();
					dt += (t1 - t0);

					sum = 0;
					for (i = 0; i < n64; i++) {
						if (overlap_uc && i == fence_idx)
							continue;
						sum += p[i];
					}
					{
						uint64_t expect = expected_sum_u64(n64, iter);
						if (overlap_uc)
							expect -= (uint64_t)(fence_idx + (uint64_t)iter);
						if (sum != expect) {
							fprintf(stderr,
								"%s ntwrite_ucfence verify failed iter=%d sum=0x%" PRIx64 " expect=0x%" PRIx64 "\n",
								path, iter, sum, expect);
							g_verify_failures++;
							ok = 0;
						}
					}
					if (!ok)
						break;
				}
#else
				break;
#endif
			}

			if (nt_supported) {
				if (g_verify_failures == fail_before)
					printf("%s ntwrite_ucfence verify: ok\n", path);
				else
					printf("%s ntwrite_ucfence verify: failed\n", path);
				{
					double bytes = (double)size_bytes * (double)iters;
					printf("%s ntwrite_ucfence: %.2f MB/s (%.3f s)\n", path,
					       (bytes / (1024.0 * 1024.0)) / dt, dt);
				}
			} else {
				printf("%s ntwrite_ucfence: unsupported arch\n", path);
			}
		}
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
		printf("%s read : %.2f MB/s (%.3f s) sum=0x%" PRIx64 "\n", path,
		       (bytes / (1024.0 * 1024.0)) / dt, dt, sum);
	}

	munmap(map, size_bytes);
	close(fd);
}

static void usage(const char *argv0)
{
	fprintf(stderr, "Usage: %s [-s size_mb] [-i iters] [-c cpu]\n", argv0);
	fprintf(stderr, "Default size uses kernel module ioctl.\n");
}

extern void test_a(uint8_t* wc_buffer, uint8_t* uc_buffer, uint8_t* check_buffer);
extern void test_b(uint8_t* wc_buffer, uint8_t* uc_buffer, uint8_t* check_buffer);
extern void test_c(uint8_t* wc_buffer, uint8_t* uc_buffer, uint8_t* check_buffer);
extern void test_d(uint8_t* wc_buffer, uint8_t* uc_buffer, uint8_t* check_buffer);

static void run_test_without_fence(void)
{
	const size_t sz = 4096;
	int fd_wc = -1, fd_uc = -1;
	uint8_t *wc_map = MAP_FAILED;
	uint8_t *uc_map = MAP_FAILED;
	uint8_t *check = NULL;

	fd_wc = open("/dev/memcache_wc", O_RDWR);
	if (fd_wc < 0)
		goto out;
	fd_uc = open("/dev/memcache_uc", O_RDWR);
	if (fd_uc < 0)
		goto out;

	wc_map = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd_wc, 0);
	if (wc_map == MAP_FAILED)
		goto out;
	uc_map = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd_uc, 0);
	if (uc_map == MAP_FAILED)
		goto out;

	if (posix_memalign((void **)&check, 4096, sz) != 0)
		goto out;
	memset(check, 0, sz);

	test_a(wc_map, uc_map, check);
	test_b(wc_map, uc_map, check);
	test_c(wc_map, uc_map, check);
	test_d(wc_map, uc_map, check);

out:
	if (check)
		free(check);
	if (uc_map != MAP_FAILED)
		munmap(uc_map, sz);
	if (wc_map != MAP_FAILED)
		munmap(wc_map, sz);
	if (fd_uc >= 0)
		close(fd_uc);
	if (fd_wc >= 0)
		close(fd_wc);
}

int main(int argc, char **argv)
{
	size_t size_bytes = 0;
	int iters = 50;
	int cpu = 0;
	int pin = 0;
	int opt;

	while ((opt = getopt(argc, argv, "s:i:c:h")) != -1) {
		switch (opt) {
		case 's':
			size_bytes = (size_t)strtoul(optarg, NULL, 0) * 1024 * 1024;
			break;
		case 'i':
			iters = atoi(optarg);
			break;
		case 'c':
			cpu = atoi(optarg);
			pin = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return 1;
		}
	}

#if defined(__i386__) || defined(__x86_64__)
	if (!pin)
		pin = 1;
#endif

	if (pin) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) != 0) {
			fprintf(stderr, "sched_setaffinity cpu=%d failed: %s\n", cpu, strerror(errno));
			return 1;
		}
		printf("pinned to cpu %d\n", cpu);
	}

	uc_fence_init();

	//bench_one("/dev/memcache_wb", size_bytes, iters);
	//bench_one("/dev/memcache_uc", size_bytes/8, iters/4);
	//bench_one("/dev/memcache_wc", size_bytes, iters);
	run_test_without_fence();
	return g_verify_failures ? 1 : 0;
}
