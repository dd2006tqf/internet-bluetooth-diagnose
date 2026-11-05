// SPDX-License-Identifier: GPL-2.0
/*
 * weaknet_softirq.c - WeakNet 实验性软中断计数采集内核模块
 *
 * 定位：B1 实验路径，与 weaknet_cpu_stat 同构。
 *
 * 数据通路：
 *   hrtimer(1s) -> 逐 CPU 读取 kstat_softirqs_cpu 到私有缓冲区
 *              -> seqlock 保护下 memcpy 到共享缓冲区
 *              -> 用户态 mmap 只读访问
 *
 * 相比之下少一个 tickless 细节：软中断计数是纯递增次数，
 * 没有 idle / iowait 那种需要走 tick 子系统的特殊路径。
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
#include <linux/interrupt.h>
#include <linux/cpumask.h>
#include <linux/version.h>
#include <linux/capability.h>
#include <linux/sched.h>

#include "weaknet_kmod_abi.h"

#define DEVICE_NAME "weaknet_softirq"
#define CLASS_NAME  DEVICE_NAME

MODULE_LICENSE("GPL");
MODULE_AUTHOR("tanqf");
MODULE_DESCRIPTION("WeakNet experimental softirq counter collector via mmap");
MODULE_VERSION("0.1.0");

static dev_t g_dev;
static struct cdev g_cdev;
static struct class *g_class;
static struct device *g_device;

static struct wn_softirq_snapshot *g_shared;
static struct wn_softirq_snapshot *g_priv;
static size_t g_map_size;

static struct hrtimer g_timer;
static ktime_t g_interval;

static void wn_collect(struct wn_softirq_snapshot *priv)
{
	int cpu, n = 0;

	priv->hdr.abi_version = WEAKNET_KMOD_ABI_VERSION;
	priv->hdr.sample_ns = ktime_get_ns();

	for_each_possible_cpu(cpu) {
		struct wn_softirq_stat *r;
		int i;

		if (n >= (int)WEAKNET_KMOD_MAX_CPUS)
			break;

		r = &priv->cpu[n++];
		r->cpu_id = (u32)cpu;
		r->online = cpu_online(cpu) ? 1u : 0u;

		for (i = 0; i < (int)WEAKNET_KMOD_NR_SOFTIRQ; i++)
			r->cnt[i] = kstat_softirqs_cpu(i, cpu);
	}

	priv->hdr.nr_cpus = (u32)n;
}

static void wn_publish(struct wn_softirq_snapshot *dst,
		       const struct wn_softirq_snapshot *src)
{
	u64 s = READ_ONCE(dst->hdr.seq);
	size_t bytes = src->hdr.nr_cpus * sizeof(struct wn_softirq_stat);

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

	if (vma->vm_pgoff)
		return -EINVAL;

	if (size > g_map_size) {
		pr_err("%s: mmap size %lu exceeds %zu\n",
		       DEVICE_NAME, size, g_map_size);
		return -EINVAL;
	}

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

	BUILD_BUG_ON(NR_SOFTIRQS != WEAKNET_KMOD_NR_SOFTIRQ);
	BUILD_BUG_ON(sizeof(struct wn_softirq_stat) % 8);

	g_map_size = PAGE_ALIGN(sizeof(struct wn_softirq_snapshot));

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
