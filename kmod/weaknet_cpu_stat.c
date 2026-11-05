// SPDX-License-Identifier: GPL-2.0
/*
 * weaknet_cpu_stat.c - WeakNet 实验性 per-CPU 统计采集内核模块
 *
 * 定位：B1 实验路径。不进 tools/ci.sh、不进 OTA manifest、不注册
 * IMonitorPlugin、不新增 D-Bus 方法。仅供板端手工 make / insmod 验证。
 *
 * 数据通路：
 *   hrtimer(1s) -> 逐 CPU 读取 kcpustat / idle / iowait 到私有缓冲区
 *              -> seqlock 保护下 memcpy 到共享缓冲区
 *              -> 用户态 mmap 只读访问
 *
 * 与参考实现（monitor_system worker/src/kmod/cpu_stat_collector.c）的差异：
 *   D1  私有缓冲区 + seqlock 代次号，消除跨字段撕裂读
 *   D2  vmalloc_user + remap_vmalloc_range，去掉错误的 pgprot_noncached
 *   D3  输出纳秒而非 jiffies。实测澄清两点（本板 CONFIG_HZ=250、
 *       CONFIG_TICK_CPU_ACCOUNTING=y）：
 *         a) kcpustat 的 busy 类字段本就按 TICK_NSEC(1e9/HZ=4ms) 累加，
 *            源实现的 nsec_to_jiffies() 除以 4e6 是**无损**的，
 *            所以「转 jiffies 引入量化误差」对 busy 类不成立。
 *         b) 真正的精度收益在 idle / iowait：它们来自
 *            get_cpu_idle_time_us() 的**微秒**值，源实现接着用
 *            usecs_to_jiffies() 又压回 4ms；本实现保留微秒精度。
 *       guest / guest_nice 原样导出，由用户态按 /proc/stat 口径排除在
 *       busy 之外（user 已含 guest、nice 已含 guest_nice）。
 *   D4  头部携带 nr_cpus，用户态不再硬编码上限
 *   D5  每条记录带 online 标记，可区分离线 CPU
 *   D6  open 校验 CAP_SYS_ADMIN
 *   D7  Makefile 的 KDIR 可覆盖，支持交叉编译
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/kernel_stat.h>
#include <linux/cpumask.h>
#include <linux/tick.h>
#include <linux/version.h>
#include <linux/capability.h>
#include <linux/sched.h>

#include "weaknet_kmod_abi.h"

#define DEVICE_NAME "weaknet_cpu_stat"
#define CLASS_NAME  DEVICE_NAME

#define NSEC_PER_USEC_LOCAL 1000ULL

MODULE_LICENSE("GPL");
MODULE_AUTHOR("tanqf");
MODULE_DESCRIPTION("WeakNet experimental per-CPU stats collector via mmap");
MODULE_VERSION("0.1.0");

static dev_t g_dev;
static struct cdev g_cdev;
static struct class *g_class;
static struct device *g_device;

static struct wn_cpu_snapshot *g_shared;  /* 用户态可见的映射区 */
static struct wn_cpu_snapshot *g_priv;    /* 内核私有工作区 */
static size_t g_map_size;

static struct hrtimer g_timer;
static ktime_t g_interval;

/*
 * tickless 感知的空闲时间。NO_HZ_IDLE 下 kcpustat 的 idle 不会随 tick
 * 推进，必须优先取 tick 子系统的精确值，失败才回落累计值。
 *
 * 【离线 CPU 必须绕开 tick 路径】
 *
 * get_cpu_idle_time_us() 内部只判 tick_nohz_active，**不判 cpu_online()**，
 * 它读的是 per_cpu(tick_cpu_sched, cpu) 的 idle_sleeptime / idle_entrytime。
 * 而 CPU 下线时 tick_cancel_sched_timer() 会执行
 *
 *     memset(ts, 0, sizeof(*ts));
 *
 * 把这两个字段连同 idle_active 一起清零。清零后本函数走 else 分支：
 * idle = 0 + (ktime_get() - 0)，也就是**返回自开机以来的绝对时间**，
 * 且每次采样都在增长。实测（11-05，CPU6/CPU7 离线）该值从 1533 s 跳到
 * 1.58 s，之后每 2 秒 +2.00 s——它跟的是 wall clock，不是 CPU 空闲。
 *
 * 危害：用户态用 (idle_a - idle_b) 算区间增量，会得到负数或远超 100%
 * 的离谱使用率。而且这个错误值不会返回 -1，所以下面的回落分支根本不触发。
 *
 * 已实测确认（11-05）：CPU 离线期间 kcpustat 的纯累计字段
 * （user/system/softirq 等）是完全冻结的。因此对离线 CPU 直接读
 * kcpustat 既安全又语义正确——报的是该 CPU 下线前的最终累计值。
 */
static u64 wn_idle_ns(int cpu)
{
	u64 us;

	if (!cpu_online(cpu))
		return kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];

	us = get_cpu_idle_time_us(cpu, NULL);
	if (us != (u64)-1)
		return us * NSEC_PER_USEC_LOCAL;
	return kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
}

static u64 wn_iowait_ns(int cpu)
{
	u64 us;

	if (!cpu_online(cpu))
		return kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];

	us = get_cpu_iowait_time_us(cpu, NULL);
	if (us != (u64)-1)
		return us * NSEC_PER_USEC_LOCAL;
	return kcpustat_cpu(cpu).cpustat[CPUTIME_IOWAIT];
}

/*
 * 上一轮各 CPU 的在线状态，用于判定累计值是否发生重置。
 *
 * 为什么要记：CPU 下线 → 上线时，内核在 tick_cancel_sched_timer() 里
 * memset 掉 tick_cpu_sched，idle/iowait 累计被清零后重新累加。
 * 快照表现为「累计值突然变小」。只要 online 状态与上一轮不同，
 * 就把 valid 置 0，让用户态跳过这一对快照。
 */
static u8 g_prev_online[WEAKNET_KMOD_MAX_CPUS];
static bool g_have_prev;

/* 采集到私有缓冲区，不触碰共享区，避免长时间中断读者。 */
static void wn_collect(struct wn_cpu_snapshot *priv)
{
	int cpu, n = 0;

	priv->hdr.abi_version = WEAKNET_KMOD_ABI_VERSION;
	priv->hdr.sample_ns = ktime_get_ns();

	for_each_possible_cpu(cpu) {
		struct wn_cpu_stat *r;
		struct kernel_cpustat kcs;
		u32 online;

		if (n >= (int)WEAKNET_KMOD_MAX_CPUS)
			break;

		/*
		 * 未开 CONFIG_VIRT_CPU_ACCOUNTING_GEN 时该函数是
		 * *dst = kcpustat_cpu(cpu) 的内联展开。
		 */
		kcpustat_cpu_fetch(&kcs, cpu);

		online = cpu_online(cpu) ? 1u : 0u;

		r = &priv->cpu[n++];
		r->cpu_id = (u32)cpu;
		r->online = online;

		/*
		 * 首轮没有可比对象，标 valid；此后只要在线状态发生变化
		 * （下线或上线）就标 0，因为内核此时已经重置了累计值。
		 */
		if (!g_have_prev || g_prev_online[cpu] == online)
			r->valid = 1u;
		else
			r->valid = 0u;
		g_prev_online[cpu] = (u8)online;

		r->user = kcs.cpustat[CPUTIME_USER];
		r->nice = kcs.cpustat[CPUTIME_NICE];
		r->system = kcs.cpustat[CPUTIME_SYSTEM];
		r->idle = wn_idle_ns(cpu);
		r->iowait = wn_iowait_ns(cpu);
		r->irq = kcs.cpustat[CPUTIME_IRQ];
		r->softirq = kcs.cpustat[CPUTIME_SOFTIRQ];
		r->steal = kcs.cpustat[CPUTIME_STEAL];
		r->guest = kcs.cpustat[CPUTIME_GUEST];
		r->guest_nice = kcs.cpustat[CPUTIME_GUEST_NICE];
	}

	g_have_prev = true;
	priv->hdr.nr_cpus = (u32)n;
}

/*
 * seqlock 风格的发布：写窗口只覆盖一次 memcpy。
 * 奇数代次表示写入进行中，用户态读到即重试。
 */
static void wn_publish(struct wn_cpu_snapshot *dst,
		       const struct wn_cpu_snapshot *src)
{
	u64 s = READ_ONCE(dst->hdr.seq);
	size_t bytes = src->hdr.nr_cpus * sizeof(struct wn_cpu_stat);

	WRITE_ONCE(dst->hdr.seq, s + 1);
	smp_wmb();

	dst->hdr.abi_version = src->hdr.abi_version;
	dst->hdr.sample_ns = src->hdr.sample_ns;
	dst->hdr.nr_cpus = src->hdr.nr_cpus;
	memcpy(dst->cpu, src->cpu, bytes);

	smp_wmb();
	WRITE_ONCE(dst->hdr.seq, s + 2);
}

static enum hrtimer_restart wn_timer_cb(struct hrtimer *t)
{
	wn_collect(g_priv);
	wn_publish(g_shared, g_priv);

	hrtimer_forward_now(t, g_interval);
	return HRTIMER_RESTART;
}

static int wn_open(struct inode *inode, struct file *file)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	return 0;
}

static void wn_vma_close(struct vm_area_struct *vma)
{
	module_put(THIS_MODULE);
}

static const struct vm_operations_struct wn_vm_ops = {
	.close = wn_vma_close,
};

static int wn_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	int ret;

	/*
	 * 只读映射：快照是内核单向发布的，用户态没有写它的理由。
	 *
	 * 缺了这一条时 PROT_WRITE 映射会被 remap_vmalloc_range 直接放行，
	 * 用户态可以改写 hdr.seq / sample_ns 等字段。更糟的是 seqlock 挡不住：
	 * 用户态自己就是写者，改完 seq 后读者照常采信，一致性保证被架空。
	 */
	if (vma->vm_flags & VM_WRITE)
		return -EACCES;

	if (vma->vm_pgoff)
		return -EINVAL;

	if (size > g_map_size) {
		pr_err("%s: mmap size %lu exceeds %zu\n",
		       DEVICE_NAME, size, g_map_size);
		return -EINVAL;
	}

	/* 映射存活期间禁止卸载，避免 vm_ops 回调进已释放的代码段。 */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	ret = remap_vmalloc_range(vma, g_shared, 0);
	if (ret) {
		module_put(THIS_MODULE);
		return ret;
	}

	vma->vm_ops = &wn_vm_ops;
	return 0;
}

static const struct file_operations wn_fops = {
	.owner  = THIS_MODULE,
	.open   = wn_open,
	.mmap   = wn_mmap,
	.llseek = no_llseek,
};

static int __init wn_init(void)
{
	int ret;

	BUILD_BUG_ON(NR_STATS < CPUTIME_GUEST_NICE + 1);
	BUILD_BUG_ON(sizeof(struct wn_cpu_stat) % 8);

	g_map_size = PAGE_ALIGN(sizeof(struct wn_cpu_snapshot));

	g_shared = vmalloc_user(g_map_size);
	if (!g_shared)
		return -ENOMEM;

	g_priv = vmalloc(g_map_size);
	if (!g_priv) {
		ret = -ENOMEM;
		goto err_free_shared;
	}

	memset(g_priv, 0, g_map_size);
	memset(g_shared, 0, g_map_size);

	/* 先采一次，保证用户态一打开就能读到有效数据。 */
	wn_collect(g_priv);
	wn_publish(g_shared, g_priv);

	ret = alloc_chrdev_region(&g_dev, 0, 1, DEVICE_NAME);
	if (ret)
		goto err_free_priv;

	cdev_init(&g_cdev, &wn_fops);
	g_cdev.owner = THIS_MODULE;

	ret = cdev_add(&g_cdev, g_dev, 1);
	if (ret)
		goto err_unregister;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	g_class = class_create(CLASS_NAME);
#else
	g_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
	if (IS_ERR(g_class)) {
		ret = PTR_ERR(g_class);
		goto err_cdev_del;
	}

	g_device = device_create(g_class, NULL, g_dev, NULL, DEVICE_NAME);
	if (IS_ERR(g_device)) {
		ret = PTR_ERR(g_device);
		goto err_class_destroy;
	}

	g_interval = ktime_set(1, 0);
	hrtimer_init(&g_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	g_timer.function = wn_timer_cb;
	hrtimer_start(&g_timer, g_interval, HRTIMER_MODE_REL);

	pr_info("%s: loaded, %d possible CPUs, map %zu bytes\n",
		DEVICE_NAME, num_possible_cpus(), g_map_size);
	return 0;

err_class_destroy:
	class_destroy(g_class);
err_cdev_del:
	cdev_del(&g_cdev);
err_unregister:
	unregister_chrdev_region(g_dev, 1);
err_free_priv:
	vfree(g_priv);
err_free_shared:
	vfree(g_shared);
	return ret;
}

static void __exit wn_exit(void)
{
	hrtimer_cancel(&g_timer);

	device_destroy(g_class, g_dev);
	class_destroy(g_class);
	cdev_del(&g_cdev);
	unregister_chrdev_region(g_dev, 1);

	vfree(g_priv);
	vfree(g_shared);

	pr_info("%s: unloaded\n", DEVICE_NAME);
}

module_init(wn_init);
module_exit(wn_exit);
