// SPDX-License-Identifier: GPL-2.0
/*
 * weaknet_wdt.c - WeakNet 硬件看门狗接管模块（Allwinner A733 / sunxi-wdt）
 *
 * 定位：本模块**不是**观测模块，它会写硬件寄存器并改变系统行为，
 *       与 weaknet_cpu_stat / weaknet_softirq 的无害观测定位完全不同。
 *       因此它**不随发布包开机无条件加载**，由 weaknet-health-guard
 *       在确认自身可用后按需加载（见 openspec/tasks/ 下的任务记录）。
 *
 * 为什么需要它：
 *   /dev/watchdog0 由内建的 sunxi_wdt 持有，而该驱动把超时固定为 16 秒
 *   且 WDIOC_SETTIMEOUT 返回 EINVAL。要让「系统健康」而非「进程活着」
 *   成为喂狗条件，就必须把喂狗权拿到用户态可控的设备上。
 *
 * 为什么不直接 /dev/mem 写魔数：
 *   能力上完全等价（本模块的核心就是一行 writel），但 /dev/mem 方案里
 *   **任何 root 进程**都能静默关掉这层保护，无法审计也无法互斥。
 *   注册成标准 watchdog 设备后，/dev/weaknet_wdt 是 0600 且单一持有者，
 *   语义也从「有人在敲一个魔数」变成「watchdog 被喂了」。
 *
 * 硬件事实（2026-11-14 由运行中内核的指令与 .rodata 双向确认）：
 *   寄存器窗口 0x02050000 + 0x20（DT 声明值，硬件只解码低 5 位）
 *     +0x0C  喂狗：写魔数 0x14AF
 *     +0x10  使能位：只读回 1
 *     +0x14  配置：使能 0xB1 / 关闭 0x00
 *     +0x18  模式：恒 0x1F，本模块不触碰
 *
 *   对应 sunxi_wdt_ping 的真实指令：
 *     ldrb w2, [regs]      ; w2 = 0x0C
 *     mov  w1, #0x14af
 *     str  w1, [base, w2]
 *   注意魔数是 0x14AF 而**不是**写 1 —— 写 1 会被硬件静默丢弃，
 *   这是本项目两次实验各复位一次后才查清的事实。
 *
 * 硬编码假设（本模块不通用，改板必改这里）：
 *   物理基址 0x02050000 是 A733 的 watchdog 地址，直接从 DT 抄来。
 *   本模块不匹配 DT 节点（因为 sunxi_wdt 已经绑定了它），而是手工
 *   创建 platform device 后直接 ioremap，因此拿不到 of 资源。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>
#include <linux/version.h>

#define DRIVER_NAME "weaknet_wdt"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("tanqf");
MODULE_DESCRIPTION("WeakNet watchdog takeover for Allwinner A733 (sunxi-wdt)");
MODULE_VERSION("0.1.0");

/* ---- 硬件常量：全部来自板端实测，见文件头注释 ---- */
#define SUNXI_WDT_PHYS_BASE 0x02050000UL
#define SUNXI_WDT_WINDOW    0x20

#define REG_PING   0x0C
#define REG_ENABLE 0x10
#define REG_CFG    0x14
#define REG_MODE   0x18

/*
 * ping 是裸写魔数，enable/cfg 必须带高半字使能键 0x16AA0000。
 * 这是从 sunxi_wdt_start 的真实指令读出来的：
 *     ldr w3, [regs, #8]      ; 0x16AA0000
 *     orr w0, w0, w3          ; 目标值 |= 0x16AA0000
 *     str w0, [base, #0x10]
 * 硬件消费掉高半字后只保留低半字，因此回读得到的是 0x1 / 0xb1。
 * 不带键的写会被静默丢弃 —— 本模块第一版就踩了这个坑。
 */
#define MAGIC_PING    0x14AF
#define ENABLE_KEY    0x16AA0000

/* timeout 位偏移 4、共 4 位；16 秒对应编码 0xB，故超时字段为 0xB0 */
#define CFG_TIMEOUT_SHIFT 4
#define CFG_TIMEOUT_MASK  0xF0
#define CFG_TIMEOUT_CODE  0xB0
#define CFG_ENABLE_BIT    0x01

#define CFG_ENABLED  (CFG_TIMEOUT_CODE | CFG_ENABLE_BIT)   /* 0xB1 */
#define CFG_DISABLED 0x00

#define ENABLE_MASK 0x03
#define ENABLE_VAL  0x01

#define WN_TIMEOUT_SEC 16

struct wn_wdt {
	void __iomem *base;
	struct watchdog_device wdd;
};

static struct platform_device *g_pdev;
static struct wn_wdt *g_wn;

/* ---- 寄存器操作 ---- */

static inline void wn_hw_ping(struct wn_wdt *wn)
{
	/*
	 * ping 是唯一**裸写**的操作：不需要使能键，也不需要读-改-写。
	 * sunxi_wdt_ping 在写之前执行 dmb oshst（实测指令 bf3203d5）。
	 * writel() 内部有 __iowmb()，显式再声明一次以避免以后有人换成
	 * writel_relaxed 时无声丢掉屏障。
	 */
	wmb();
	writel(MAGIC_PING, wn->base + REG_PING);
}

static inline void wn_hw_enable(struct wn_wdt *wn)
{
	u32 cur;

	/* 读-改-写：保留 reset mask 之外的位，置使能位 */
	cur = readl(wn->base + REG_ENABLE);
	writel((cur & ~ENABLE_MASK) | ENABLE_VAL | ENABLE_KEY,
	       wn->base + REG_ENABLE);

	cur = readl(wn->base + REG_CFG);
	writel(cur | CFG_TIMEOUT_CODE | CFG_ENABLE_BIT | ENABLE_KEY,
	       wn->base + REG_CFG);
	wmb();
}

static inline void wn_hw_disable(struct wn_wdt *wn)
{
	/*
	 * sunxi_wdt_stop 用 regs[8]（=0x16AA0000，即 CFG_DISABLED 配合键）
	 * 直接整字覆盖 cfg，不做读-改-写。照抄同一行为。
	 */
	wmb();
	writel(CFG_DISABLED | ENABLE_KEY, wn->base + REG_CFG);
	wmb();
}

/* ---- watchdog 回调 ---- */

static int wn_wdt_start(struct watchdog_device *wdd)
{
	struct wn_wdt *wn = watchdog_get_drvdata(wdd);

	wn_hw_enable(wn);
	wn_hw_ping(wn);
	set_bit(WDOG_HW_RUNNING, &wdd->status);
	return 0;
}

static int wn_wdt_stop(struct watchdog_device *wdd)
{
	struct wn_wdt *wn = watchdog_get_drvdata(wdd);

	wn_hw_disable(wn);
	clear_bit(WDOG_HW_RUNNING, &wdd->status);
	return 0;
}

static int wn_wdt_ping(struct watchdog_device *wdd)
{
	struct wn_wdt *wn = watchdog_get_drvdata(wdd);

	wn_hw_ping(wn);
	return 0;
}

static int wn_wdt_set_timeout(struct watchdog_device *wdd, unsigned int t)
{
	/*
	 * 与 sunxi_wdt 行为保持一致：只接受 16 秒，其余一律 EINVAL。
	 * 硬件本身的超时编码无法在 0x14 的 cfg 里安全改写（本轮实验
	 * 未验证其他超时值），因此不做超出已知范围的承诺。
	 */
	if (t != WN_TIMEOUT_SEC)
		return -EINVAL;

	wdd->timeout = WN_TIMEOUT_SEC;
	return 0;
}

static int wn_wdt_restart(struct watchdog_device *wdd,
			  unsigned long action, void *data)
{
	struct wn_wdt *wn = watchdog_get_drvdata(wdd);

	wn_hw_enable(wn);
	wn_hw_ping(wn);
	/* sunxi_wdt_restart 是死循环等待硬件复位，这里给硬件响应时间 */
	mdelay(100);
	return 0;
}

static const struct watchdog_info wn_wdt_info = {
	.options  = WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE | WDIOF_SETTIMEOUT,
	.identity = "weaknet-wdt",
};

static const struct watchdog_ops wn_wdt_ops = {
	.owner       = THIS_MODULE,
	.start       = wn_wdt_start,
	.stop        = wn_wdt_stop,
	.ping        = wn_wdt_ping,
	.set_timeout = wn_wdt_set_timeout,
	.restart     = wn_wdt_restart,
};

/* ---- platform driver ---- */

static int wn_wdt_probe(struct platform_device *pdev)
{
	struct wn_wdt *wn;
	int ret;

	wn = devm_kzalloc(&pdev->dev, sizeof(*wn), GFP_KERNEL);
	if (!wn)
		return -ENOMEM;

	wn->base = devm_ioremap(&pdev->dev, SUNXI_WDT_PHYS_BASE,
				SUNXI_WDT_WINDOW);
	if (!wn->base) {
		dev_err(&pdev->dev, "ioremap %#lx failed\n",
			(unsigned long)SUNXI_WDT_PHYS_BASE);
		return -ENOMEM;
	}

	/*
	 * 这里刻意**不做**任何 cfg 自检写。
	 *
	 * 探测阶段写 cfg 等于短暂改写看门狗配置：若此刻另一条路径正在喂狗，
	 * 会被我们的写打断；若探测时硬件本已使能，读-改-写还可能把超时
	 * 编码改错。开关看门狗只发生在 open()/start 与 close()/stop 时刻。
	 *
	 * 硬件可写性已在开发阶段用一次性自检确证（带键写 0x00 -> 回读 0xb1，
	 * 不带键写被静默丢弃），证据见 docs/ideas/喂狗条件检查-系统健康看门狗.md。
	 */

	wn->wdd.info        = &wn_wdt_info;
	wn->wdd.ops         = &wn_wdt_ops;
	wn->wdd.parent      = &pdev->dev;
	wn->wdd.timeout     = WN_TIMEOUT_SEC;
	wn->wdd.min_timeout = WN_TIMEOUT_SEC;
	wn->wdd.max_timeout = WN_TIMEOUT_SEC;

	watchdog_set_drvdata(&wn->wdd, wn);
	watchdog_set_restart_priority(&wn->wdd, 128);
	watchdog_stop_on_reboot(&wn->wdd);
	watchdog_stop_on_unregister(&wn->wdd);

	ret = devm_watchdog_register_device(&pdev->dev, &wn->wdd);
	if (ret) {
		dev_err(&pdev->dev, "watchdog_register_device failed: %d\n", ret);
		return ret;
	}

	g_wn = wn;

	dev_info(&pdev->dev,
		 "registered %s (phys=%#lx window=%#x ping=%#x magic=%#x)",
		 wn->wdd.info->identity,
		 (unsigned long)SUNXI_WDT_PHYS_BASE, SUNXI_WDT_WINDOW,
		 REG_PING, MAGIC_PING);

	return 0;
}

static int wn_wdt_remove(struct platform_device *pdev)
{
	g_wn = NULL;
	return 0;
}

static struct platform_driver wn_wdt_driver = {
	.probe  = wn_wdt_probe,
	.remove = wn_wdt_remove,
	.driver = {
		.name = DRIVER_NAME,
	},
};

static int __init weaknet_wdt_init(void)
{
	int ret;

	ret = platform_driver_register(&wn_wdt_driver);
	if (ret)
		return ret;

	/*
	 * 手工创建 platform device：sunxi_wdt 已经绑定了 DT 里的
	 * watchdog@2050000 节点，同一节点不可能再被第二个驱动 probe。
	 * 因此本模块走"无资源 device + 硬编码物理地址"的路径。
	 */
	g_pdev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
	if (IS_ERR(g_pdev)) {
		ret = PTR_ERR(g_pdev);
		g_pdev = NULL;
		platform_driver_unregister(&wn_wdt_driver);
		return ret;
	}

	return 0;
}

static void __exit weaknet_wdt_exit(void)
{
	if (g_pdev) {
		platform_device_unregister(g_pdev);
		g_pdev = NULL;
	}
	platform_driver_unregister(&wn_wdt_driver);
}

module_init(weaknet_wdt_init);
module_exit(weaknet_wdt_exit);
