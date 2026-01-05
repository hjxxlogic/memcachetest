#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <x86intrin.h>
#include <string.h>
#include <stdlib.h>

// 测试结构
#define BUFFER_SIZE (64 * 1024) // 64KB
#define ITERATIONS 1000
#define ALIGN_SIZE 4096
// 内存屏障宏
#define SFENCE() __asm__ volatile("sfence" ::: "memory")
#define MFENCE() __asm__ volatile("mfence" ::: "memory")
#define LFENCE() __asm__ volatile("lfence" ::: "memory")
// 时间测量
static inline uint64_t rdtsc() { unsigned int lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

static void run_one_variant(const char *name, int do_uc_marker, int do_sfence,
			    uint8_t *wc_buffer, uint8_t *uc_buffer, uint8_t *check_buffer)
{
	printf("%s\n", name);
	uint64_t total_time = 0;
	int failures = 0;
	for (int i = 0; i < ITERATIONS; i++) {
		// 重置检查缓冲区
		memset(check_buffer, 0, ALIGN_SIZE);
		SFENCE();
		// 使用MOVNT向WC缓冲区写入
		for (int j = 0; j < ALIGN_SIZE; j += 8) {
			_mm_stream_si64((long long*)(wc_buffer + j), 0x0123456789ABCDEF);
		}
		// UC marker
		if (do_uc_marker)
			*((volatile uint64_t*)uc_buffer) = 0xDEADBEEFCAFEBABE;
		if (do_sfence)
			SFENCE();
		// 立即从WC缓冲区读取数据到检查缓冲区
		uint64_t start = rdtsc();
		memcpy(check_buffer, wc_buffer, ALIGN_SIZE);
		uint64_t end = rdtsc();
		total_time += (end - start);
		// 检查数据一致性
		int valid = 1;
		for (int j = 0; j < ALIGN_SIZE; j += 8) {
			if (*((uint64_t*)(check_buffer + j)) != 0x0123456789ABCDEF) {
				valid = 0;
				break;
			}
		}
		if (!valid) {
			failures++;
		}
	}
	printf(" 平均读取延迟: %lu cycles\n", total_time / ITERATIONS);
	printf(" 数据不一致次数: %d/%d (%.1f%%)\n", failures, ITERATIONS, (failures * 100.0) / ITERATIONS);
	if (failures)
		printf(" 结论: 观察到数据不一致，可能存在排序/可见性问题\n\n");
	else
		printf(" 结论: 本次未观察到数据不一致\n\n");
}

void test_a(uint8_t* wc_buffer, uint8_t* uc_buffer, uint8_t* check_buffer)
{
	run_one_variant("测试A: baseline (no uc marker, no sfence)", 0, 0, wc_buffer, uc_buffer, check_buffer);
}

void test_b(uint8_t* wc_buffer, uint8_t* uc_buffer, uint8_t* check_buffer)
{
	run_one_variant("测试B: sfence after nt stores", 0, 1, wc_buffer, uc_buffer, check_buffer);
}

void test_c(uint8_t* wc_buffer, uint8_t* uc_buffer, uint8_t* check_buffer)
{
	run_one_variant("测试C: uc marker only (no sfence)", 1, 0, wc_buffer, uc_buffer, check_buffer);
}

void test_d(uint8_t* wc_buffer, uint8_t* uc_buffer, uint8_t* check_buffer)
{
	run_one_variant("测试D: uc marker + sfence", 1, 1, wc_buffer, uc_buffer, check_buffer);
}
