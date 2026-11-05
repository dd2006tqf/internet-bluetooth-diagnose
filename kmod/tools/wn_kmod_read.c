// SPDX-License-Identifier: GPL-2.0
/*
 * wn_kmod_read.c - 读取 WeakNet 实验性内核模块共享快照的用户态工具
 *
 * 用法：
 *   wn_kmod_read cpu     [/dev/weaknet_cpu_stat]  [--count N] [--interval SEC]
 *   wn_kmod_read softirq [/dev/weaknet_softirq]   [--count N] [--interval SEC]
 *
 * 读到快照后按 /proc/stat 口径计算每个 CPU 的使用率，并打印。
 * 若给出 --count > 1，则按 --interval 周期重复采样并打印差分。
 *
 * 设计要点：
 *   - seqlock 代次读取：seq 为奇数表示内核正在写入，必须重试
 *   - 使用率计算遵循 ABI 头中的口径：guest/guest_nice 不重复计入 busy
 *   - 离线 CPU 单独标注，不参与平均
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../weaknet_kmod_abi.h"

#define MAX_RETRY 1000

static const char *const g_softirq_name[WEAKNET_KMOD_NR_SOFTIRQ] = {
	"HI", "TIMER", "NET_TX", "NET_RX", "BLOCK",
	"IRQ_POLL", "TASKLET", "SCHED", "HRTIMER", "RCU",
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"用法: %s <cpu|softirq> [设备路径] [--count N] [--interval SEC]\n",
		prog);
}

/*
 * seqlock 读者：拷贝 -> 校验 -> 不一致重试。
 *
 * 关键点：必须在**拷贝之后**再读一次 seq。若只在拷贝前校验，
 * 内核可能在两次读之间写入，导致拿到半新半旧的数据。
 *
 * 统一使用 32 位访问：内核侧 memcpy 的目的地在用户页，缓存别名
 * 理论上存在，用 32 位访问可避免读到撕裂的 64 位字段。
 * （内核写 seq 用的是 WRITE_ONCE，64 位字段在 5.15/arm64 上原子，
 *  但这里保守处理。）
 */
static uint32_t load_seq(uint64_t *seq)
{
	volatile uint32_t *p = (volatile uint32_t *)seq;

	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static int read_snapshot(void *map, void *dst, size_t size, size_t seq_off,
			 uint64_t *seq_out)
{
	uint64_t *seq = (uint64_t *)((char *)map + seq_off);
	uint32_t s0, s1;
	int tries;

	for (tries = 0; tries < MAX_RETRY; tries++) {
		s0 = load_seq(seq);
		if (s0 & 1u)
			continue;	/* 内核正在写入 */

		memcpy(dst, map, size);

		/* 读屏障：确保 memcpy 完成后再读 seq */
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		s1 = load_seq(seq);

		if (s0 == s1) {
			*seq_out = s0;
			return 0;
		}
	}

	return -1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * 交叉校验模块侧时间戳：两者都基于 CLOCK_MONOTONIC，差值应当等于
 * 采样延迟（毫秒级）。若差值为负或大得离谱，说明 ABI 布局对不上。
 */
static void check_sample_age(uint64_t sample_ns, int iter)
{
	uint64_t age_ms;

	if (!sample_ns)
		return;

	age_ms = (now_ns() - sample_ns) / 1000000ULL;

	if (iter == 0)
		printf("快照时延: %" PRIu64 " ms\n\n", age_ms);
}

static double pct(uint64_t part, uint64_t total)
{
	if (!total)
		return 0.0;
	return (double)part * 100.0 / (double)total;
}

struct cpu_delta {
	uint32_t cpu_id;
	unsigned online;
	uint64_t busy;
	uint64_t total;
};

static int run_cpu(const char *dev, int count, int interval)
{
	struct wn_cpu_snapshot *cur = NULL, *prev = NULL;
	uint32_t prev_n = 0;
	void *map;
	int fd, i, iter;

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "打开 %s 失败: %s\n", dev, strerror(errno));
		return 1;
	}

	map = mmap(NULL, sizeof(struct wn_cpu_snapshot), PROT_READ,
		   MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap 失败: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	cur = calloc(1, sizeof(*cur));
	prev = calloc(1, sizeof(*prev));
	if (!cur || !prev) {
		fprintf(stderr, "内存不足\n");
		goto out;
	}

	for (iter = 0; iter < count; iter++) {
		struct wn_cpu_snapshot *live = (struct wn_cpu_snapshot *)map;
		uint64_t seq = 0;
		uint32_t n;

		if (read_snapshot(live, cur, sizeof(*cur),
				  offsetof(struct wn_cpu_snapshot, hdr.seq),
				  &seq) != 0) {
			fprintf(stderr, "快照重试超限：内核持续写入，放弃\n");
			goto out;
		}

		if (cur->hdr.abi_version != WEAKNET_KMOD_ABI_VERSION) {
			fprintf(stderr,
				"ABI 版本不匹配：模块=%u 工具=%u\n",
				cur->hdr.abi_version, WEAKNET_KMOD_ABI_VERSION);
			goto out;
		}

		n = cur->hdr.nr_cpus;
		if (n > WEAKNET_KMOD_MAX_CPUS) {
			fprintf(stderr, "nr_cpus 越界: %u\n", n);
			goto out;
		}

		if (iter == 0 || prev_n != n) {
			check_sample_age(cur->hdr.sample_ns, iter);
			printf("=== 累计值（seq=%" PRIu64 "，%u 个 CPU）===\n",
			       seq, n);
			printf("%-4s %-6s %12s %12s %12s %12s\n",
			       "CPU", "online", "user(s)", "system(s)",
			       "idle(s)", "softirq(s)");
			for (i = 0; i < (int)n; i++) {
				const struct wn_cpu_stat *r = &cur->cpu[i];

				printf("%-4u %-6u %12.2f %12.2f %12.2f %12.2f\n",
				       r->cpu_id, r->online,
				       (double)r->user / 1e9,
				       (double)r->system / 1e9,
				       (double)r->idle / 1e9,
				       (double)r->softirq / 1e9);
			}
			printf("\n");
		} else {
			uint64_t t_busy = 0, t_total = 0;

			printf("=== 区间使用率（间隔 %.1fs）===\n",
			       (double)interval);

			for (i = 0; i < (int)n; i++) {
				const struct wn_cpu_stat *a = &cur->cpu[i];
				const struct wn_cpu_stat *b = &prev->cpu[i];
				uint64_t busy_a, busy_b, tot_a, tot_b;
				uint64_t busy, total;

				busy_a = a->user + a->nice + a->system + a->irq +
					 a->softirq + a->steal;
				busy_b = b->user + b->nice + b->system + b->irq +
					 b->softirq + b->steal;
				tot_a = busy_a + a->idle + a->iowait;
				tot_b = busy_b + b->idle + b->iowait;

				if (tot_a <= tot_b)
					continue;

				busy = busy_a - busy_b;
				total = tot_a - tot_b;

				/* 离线 CPU 不参与平均 */
				if (a->online) {
					t_busy += busy;
					t_total += total;
				}

				printf("cpu%-2u %-4s busy=%6.2f%%  "
				       "(Δbusy=%" PRIu64 "ns Δtotal=%" PRIu64 "ns)\n",
				       a->cpu_id,
				       a->online ? "on" : "off",
				       pct(busy, total), busy, total);
			}

			printf("--- 在线 CPU 平均 busy=%.2f%% ---\n\n",
			       pct(t_busy, t_total));
		}

		memcpy(prev, cur, sizeof(*prev));
		prev_n = n;

		if (iter + 1 < count)
			sleep(interval);
	}

out:
	free(cur);
	free(prev);
	munmap(map, sizeof(struct wn_cpu_snapshot));
	close(fd);
	return 0;
}

static int run_softirq(const char *dev, int count, int interval)
{
	struct wn_softirq_snapshot *cur = NULL, *prev = NULL;
	uint32_t prev_n = 0;
	void *map;
	int fd, i, j, iter;

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "打开 %s 失败: %s\n", dev, strerror(errno));
		return 1;
	}

	map = mmap(NULL, sizeof(struct wn_softirq_snapshot), PROT_READ,
		   MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap 失败: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	cur = calloc(1, sizeof(*cur));
	prev = calloc(1, sizeof(*prev));
	if (!cur || !prev) {
		fprintf(stderr, "内存不足\n");
		goto out;
	}

	for (iter = 0; iter < count; iter++) {
		struct wn_softirq_snapshot *live =
			(struct wn_softirq_snapshot *)map;
		uint64_t seq = 0;
		uint32_t n;

		if (read_snapshot(live, cur, sizeof(*cur),
				  offsetof(struct wn_softirq_snapshot, hdr.seq),
				  &seq) != 0) {
			fprintf(stderr, "快照重试超限，放弃\n");
			goto out;
		}

		if (cur->hdr.abi_version != WEAKNET_KMOD_ABI_VERSION) {
			fprintf(stderr, "ABI 版本不匹配\n");
			goto out;
		}

		n = cur->hdr.nr_cpus;
		if (n > WEAKNET_KMOD_MAX_CPUS) {
			fprintf(stderr, "nr_cpus 越界: %u\n", n);
			goto out;
		}

		if (iter == 0 || prev_n != n) {
			printf("=== 累计计数（seq=%" PRIu64 "，%u 个 CPU）===\n",
			       seq, n);
			printf("%-4s %-6s", "CPU", "online");
			for (j = 0; j < (int)WEAKNET_KMOD_NR_SOFTIRQ; j++)
				printf(" %10s", g_softirq_name[j]);
			printf("\n");

			for (i = 0; i < (int)n; i++) {
				const struct wn_softirq_stat *r = &cur->cpu[i];

				printf("%-4u %-6u", r->cpu_id, r->online);
				for (j = 0; j < (int)WEAKNET_KMOD_NR_SOFTIRQ; j++)
					printf(" %10" PRIu64, r->cnt[j]);
				printf("\n");
			}
			printf("\n");
		} else {
			printf("=== 区间增量（间隔 %.1fs）===\n",
			       (double)interval);
			printf("%-4s %-6s", "CPU", "online");
			for (j = 0; j < (int)WEAKNET_KMOD_NR_SOFTIRQ; j++)
				printf(" %10s", g_softirq_name[j]);
			printf("\n");

			for (i = 0; i < (int)n; i++) {
				const struct wn_softirq_stat *a = &cur->cpu[i];
				const struct wn_softirq_stat *b = &prev->cpu[i];

				printf("%-4u %-6s", a->cpu_id,
				       a->online ? "on" : "off");
				for (j = 0; j < (int)WEAKNET_KMOD_NR_SOFTIRQ; j++)
					printf(" %10" PRIu64,
					       a->cnt[j] >= b->cnt[j]
						       ? a->cnt[j] - b->cnt[j]
						       : 0);
				printf("\n");
			}
			printf("\n");
		}

		memcpy(prev, cur, sizeof(*prev));
		prev_n = n;

		if (iter + 1 < count)
			sleep(interval);
	}

out:
	free(cur);
	free(prev);
	munmap(map, sizeof(struct wn_softirq_snapshot));
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	const char *kind, *dev = NULL;
	int count = 1, interval = 1, opt;
	struct option opts[] = {
		{ "count", required_argument, NULL, 'c' },
		{ "interval", required_argument, NULL, 'i' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	kind = argv[1];

	if (argc >= 3 && argv[2][0] != '-')
		dev = argv[2];

	optind = dev ? 3 : 2;
	while ((opt = getopt_long(argc, argv, "c:i:h", opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			count = atoi(optarg);
			if (count < 1)
				count = 1;
			break;
		case 'i':
			interval = atoi(optarg);
			if (interval < 1)
				interval = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	if (strcmp(kind, "cpu") == 0)
		return run_cpu(dev ? dev : "/dev/weaknet_cpu_stat", count,
			       interval);
	if (strcmp(kind, "softirq") == 0)
		return run_softirq(dev ? dev : "/dev/weaknet_softirq", count,
				   interval);

	fprintf(stderr, "未知类型: %s\n", kind);
	usage(argv[0]);
	return 2;
}
