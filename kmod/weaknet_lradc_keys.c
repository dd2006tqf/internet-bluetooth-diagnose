// SPDX-License-Identifier: GPL-2.0
/*
 * weaknet_lradc_keys.c - Allwinner A733 (sun60iw2p1) LRADC 按键驱动
 *
 * 定位：B1 实验路径的第二阶段——从「读内核统计量」升级到「真正操作硬件」。
 * 与 weaknet_cpu_stat / weaknet_softirq 不同，本驱动直接读写 MMIO 寄存器、
 * 注册中断、操作时钟与复位，是一个完整意义上的 platform 驱动。
 *
 * ============================================================================
 * 为什么不能直接复用 mainline 的 sun4i-lradc-keys.c
 * ============================================================================
 *
 * 1. compatible 不匹配
 *    mainline: "allwinner,sun4i-a10-lradc-keys" / "allwinner,sun8i-a83t-r-lradc"
 *    本板 DT : "allwinner,keyboard_1350mv"
 *    → of_match_table 永远匹配不上，驱动不会 probe。
 *
 * 2. 按键表（keymap）的 DT 表达形式完全不同
 *    mainline 用子节点：
 *        keys {
 *            key0 { channel = <0>; voltage = <210>; linux,code = <115>; };
 *        };
 *    本板用成对属性 + 计数：
 *        key0    = <210 115>;   （电压 210 mV，键码 115）
 *        key_cnt = <5>;
 *    → mainline 的 of_get_child_count() 返回 0，直接 -EINVAL。
 *
 * 3. 参考电压来源不同
 *    mainline: devm_regulator_get(dev, "vref") + variant 分压系数
 *    本板 DT : 没有 vref-supply，参考电压直接编码在 compatible 后缀
 *              "keyboard_1350mv" 里（1350 mV）。
 *    → regulator 拿不到，mainline 会 -EPROBE_DEFER 后失败。
 *
 * 4. 时钟与复位
 *    本板 DT 有 clocks=<0x12 0xc1> 与 resets=<0x12 0x47>，但 mainline 驱动
 *    完全不处理它们（A10/A83T 上这两路默认常开）。
 *    实测本板 lradc 时钟 enable_count=0，不显式 enable 就读不到正确值。
 *
 * 所以本驱动是**按本板 DT 契约重写**，不是移植。
 *
 * ============================================================================
 * 硬件事实（2026-11-05 板端实测）
 * ============================================================================
 *
 *   SoC        : sun60iw2p1 / allwinner,sun60i-a733
 *   板卡       : radxa,cubie-a7a
 *   内核       : 5.15.147-99btf1-a733 aarch64
 *   寄存器基址 : 0x02524000，长度 0x100
 *   中断       : DT <0 0x25 4> = SPI 0x25(37) 高电平触发，实测 /proc/interrupts 无占用
 *   时钟       : "lradc" 26 MHz，实测 enable_count=0（关闭）
 *   复位       : resets phandle 0x12/0x47
 *   初始寄存器 : CTRL=0x01000168 (ENABLE=0)，DATA0=DATA1=0x3f（6bit 满量程静默值）
 *   按键表     : key0..key4 = <电压mV, keycode>，key_cnt=5
 *                key0 <210,115 KEY_VOLUMEUP>   key1 <410,114 KEY_VOLUMEDOWN>
 *                key2 <590,139 KEY_MENU>       key3 <750, 28 KEY_ENTER>
 *                key4 <880,172 KEY_HOMEPAGE>
 *
 * ============================================================================
 * 分层实现进度
 * ============================================================================
 *
 *   L1 骨架    platform_driver + of_match + DT 解析 + ioremap + clk + reset   [本文件]
 *   L2 中断    request_irq + INTS 判源 + DATA0 读值 + 电压换算 + 写1清中断      [本文件]
 *   L3 子系统  input_dev 注册 + report_key + evdev open/close 省电              [本文件]
 *   L4 调试    debugfs 暴露寄存器与解码状态                                     [本文件]
 *
 * 四层已全部实现。对用户态的唯一正式出口是 /dev/input/eventN：
 * 用户态 open 该节点后 ADC 才开始转换，关闭后 ADC 停止（标准键盘省电语义）。
 * debugfs 里的 dev_info 与计数器仅供开发期诊断，不是产品接口。
 *
 * ============================================================================
 * 实测边界（务必保留，这是本驱动最重要的一条事实）
 * ============================================================================
 *
 * 本板（A733 BSP，内核 5.15.147-99btf1-a733）上，LRADC 的**模拟转换从未启动**：
 *
 *   CTRL 写入可读回（0x02000141 等组合全部精确回读）
 *   时钟 lradc 26 MHz，enable_count 0→1 ✓
 *   复位 deassert ✓
 *   INTC 写 0xffffffff 回读 0x00001f1f（5+5 位，与 mainline 位布局一致）✓
 *   已实现位扫描：CTRL 写全 1 回读 0xffcf7fff，实现位与 mainline sun4i 完全相同 ✓
 *   ── 但 ──
 *   INTS 恒 0x00000000，DATA0 恒 0x0000003f，IRQ 计数恒 0
 *   CHAN_SELECT 0/1/2/3、SAMPLE_RATE 0/1/2/3、KEY_MODE_SEL×LEVELB_VOL 全 16 组合、
 *   CONTINUE_TIME_SEL、FIRST_CONVERT_DLY、LEVELA_B_CNT、纯 ENABLE=1、
 *   vendor 复位值 0x01000168 + INTC=0x1f1f，全部零响应。
 *   排除 gpadc 争用（unbind sunxi-gpadc 后 LRADC 无变化）。
 *
 * 因此 L2 中断通路在本板**无法用物理按键验证**。应对方式：
 *   1. debugfs 的 selftest 会临时打开 CHAN0_DATA_IRQ 并等 100 ms，实测仍 0 次中断
 *      → 结论写入 dmesg「selftest FAILED: ISR path is dead」，这是正确结论，不是 bug；
 *   2. debugfs 的 raw 口允许直接注入一个 raw 值，走 wn_lradc_report_raw() 的完整
 *      「换算 → 匹配 → input 上报」链路，用来验证 L3 在没有硬件事件时依然正确。
 *
 * 换句话说：L1/L2/L4 的代码路径都能在本板跑，但只有 L3 可以被端到端证伪/证实。
 * 不夸大：**本驱动在 A733 上不能作为可用的按键输入设备**，它是技术栈练习与
 * 后续换平台（mainline sun4i 兼容 SoC）的适配基座。
 *
 * 寄存器与位定义来自 mainline drivers/input/keyboard/sun4i-lradc-keys.c
 * （GPL-2.0-or-later, Copyright (C) 2014 Hans de Goede），本板适配部分为本项目重写。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/string.h>	/* sysfs_streq() */

/* ------------------------------------------------------------------ */
/* 寄存器布局（偏移相对 0x02524000）                                    */
/* ------------------------------------------------------------------ */
#define LRADC_CTRL		0x00
#define LRADC_INTC		0x04
#define LRADC_INTS		0x08
#define LRADC_DATA0		0x0c
#define LRADC_DATA1		0x10

/* LRADC_CTRL 位域 */
#define FIRST_CONVERT_DLY(x)	((x) << 24)	/* 8 bits */
#define CHAN_SELECT(x)		((x) << 22)	/* 2 bits */
#define CONTINUE_TIME_SEL(x)	((x) << 16)	/* 4 bits */
#define KEY_MODE_SEL(x)		((x) << 12)	/* 2 bits */
#define LEVELA_B_CNT(x)		((x) << 8)	/* 4 bits */
#define HOLD_KEY_EN(x)		((x) << 7)
#define HOLD_EN(x)		((x) << 6)
#define LEVELB_VOL(x)		((x) << 4)	/* 2 bits */
#define SAMPLE_RATE(x)		((x) << 2)	/* 2 bits */
#define ENABLE(x)		((x) << 0)

/* LRADC_INTC / LRADC_INTS 位域（本驱动只用通道 0） */
#define CHAN0_KEYUP_IRQ		BIT(4)
#define CHAN0_ALRDY_HOLD_IRQ	BIT(3)
#define CHAN0_HOLD_IRQ		BIT(2)
#define CHAN0_KEYDOWN_IRQ	BIT(1)
#define CHAN0_DATA_IRQ		BIT(0)

/* 6 bit ADC，满量程 63 */
#define LRADC_DATA_MASK		0x3f
#define LRADC_FULL_SCALE	63

/* 本板参考电压：compatible 后缀 "keyboard_1350mv" */
#define LRADC_DEFAULT_VREF_MV	1350

/* 采样周期：SAMPLE_RATE(0) = 250 Hz / 4 ms，与 mainline 一致 */
#define LRADC_SAMPLE_RATE_VAL	0

/* DT 里 key 属性的个数上限（key0..keyN） */
#define LRADC_MAX_KEYS		16

/**
 * struct wn_lradc_key - 一个按键的 DT 描述
 * @voltage_mv: DT 里写的标称电压（毫伏）
 * @keycode:    linux,input-event-codes.h 里的 KEY_* 码
 * @raw:        由 voltage_mv 换算出的 6bit 期望码值（四舍五入，便于日志对照）
 */
struct wn_lradc_key {
	u32 voltage_mv;
	u32 keycode;
	u32 raw;
};

/**
 * struct wn_lradc - 驱动私有状态
 * @dev:        设备指针
 * @base:       ioremap 后的寄存器基址
 * @clk:        lradc 时钟
 * @rst:        复位控制（可选）
 * @irq:        中断号
 * @vref_mv:    参考电压
 * @keys:       按键表
 * @nr_keys:    按键个数
 * @lock:       保护 @cur_keycode 与统计计数（中断与 debugfs 并发访问）
 * @cur_keycode: 当前按下的键码，0 表示无键按下
 * @is_open:    ADC 是否已使能
 * @stat_irq:       中断总次数
 * @stat_keydown:   键按下事件次数
 * @stat_keyup:     键抬起事件次数
 * @stat_unknown:   中断里判不出源的次数
 * @stat_unmatched: 有电压但匹配不到任何键的次数
 * @last_raw:   最近一次读到的 DATA0 原始值
 * @last_mv:    最近一次换算出的电压
 * @dbg_dir:    debugfs 目录
 */
struct wn_lradc {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	struct reset_control *rst;
	int irq;
	u32 vref_mv;

	/*
	 * input 子系统设备。这是本驱动对用户态唯一的正式出口：
	 * 注册后 /dev/input/eventN 出现，事件经 evdev 交给用户态。
	 *
	 * 注意 @is_open 的语义由它决定：input->open 回调里才使能 ADC，
	 * input->close 回调里关闭。没有进程打开 evdev 时 ADC 不转换，
	 * 这是键盘类驱动的标准省电模式（mainline 也是这么写的）。
	 */
	struct input_dev *input;

	struct wn_lradc_key keys[LRADC_MAX_KEYS];
	u32 nr_keys;

	spinlock_t lock;
	u32 cur_keycode;

	bool is_open;

	u64 stat_irq;
	u64 stat_keydown;
	u64 stat_keyup;
	u64 stat_unknown;
	u64 stat_unmatched;
	u32 last_raw;
	u32 last_mv;

	/* 最近一次「中断通路自检」的结果，见 wn_lradc_dbg_selftest_write() */
	u64 selftest_irqs;
	u32 selftest_raw;
	bool selftest_done;

	struct dentry *dbg_dir;
};

/* ------------------------------------------------------------------ */
/* 寄存器访问                                                          */
/* ------------------------------------------------------------------ */

static inline u32 wn_lradc_read(struct wn_lradc *lradc, u32 reg)
{
	return readl(lradc->base + reg);
}

static inline void wn_lradc_write(struct wn_lradc *lradc, u32 reg, u32 val)
{
	writel(val, lradc->base + reg);
}

/*
 * 6bit 原始码 → 毫伏。
 * 与 mainline 的 val * vref / 63 等价，只是把顺序改成先乘后除以免整数截断。
 */
static u32 wn_lradc_raw_to_mv(struct wn_lradc *lradc, u32 raw)
{
	return raw * lradc->vref_mv / LRADC_FULL_SCALE;
}

/*
 * 在按键表里找与实测电压最接近的一项。
 * 返回键索引，找不到返回 -ENOENT。
 *
 * 语义与 mainline 一致：取绝对差最小者，而不是区间判定。
 * 这样 DT 里标的电压有偏差也不会整键失效。
 */
static int wn_lradc_match_key(struct wn_lradc *lradc, u32 mv, u32 *out_keycode)
{
	u32 i, best_diff = U32_MAX;
	int best = -ENOENT;

	for (i = 0; i < lradc->nr_keys; i++) {
		u32 diff = abs((int)lradc->keys[i].voltage_mv - (int)mv);

		if (diff < best_diff) {
			best_diff = diff;
			best = (int)i;
		}
	}

	if (best >= 0 && out_keycode)
		*out_keycode = lradc->keys[best].keycode;

	return best;
}

/* ------------------------------------------------------------------ */
/* 按键上报                                                            */
/* ------------------------------------------------------------------ */

/*
 * 把一个 6bit 原始值走完整的「换算 → 匹配 → 上报」链路。
 *
 * 抽成独立函数有两个目的：
 *   1. 中断路径与 debugfs 注入路径共用同一份判定逻辑，行为不会分叉；
 *   2. 让「硬件的 raw 值」与「上报语义」解耦——注入 raw 不改 DATA0 寄存器，
 *      所以即使 ADC 不转换，也能验证匹配与上报链路本身是否正确。
 *
 * 返回匹配到的键码（0 表示没匹配上）。
 *
 * 上下文约束：调用者必须已持有 @lradc->lock（本函数会改 @cur_keycode
 * 与统计计数），且必须能睡眠无关——本函数不睡眠，可从原子上下文调用。
 * 但 input_report_key() 内部会拿 input 设备自己的锁，故不能在持
 * @lradc->lock 时调用，因此这里采用「锁内算、锁外报」的分段写法。
 */
static u32 wn_lradc_report_raw(struct wn_lradc *lradc, u32 raw)
{
	unsigned long flags;
	u32 mv, keycode = 0;
	int idx;

	mv = wn_lradc_raw_to_mv(lradc, raw);
	idx = wn_lradc_match_key(lradc, mv, &keycode);

	spin_lock_irqsave(&lradc->lock, flags);
	lradc->last_raw = raw;
	lradc->last_mv = mv;
	if (idx < 0) {
		lradc->stat_unmatched++;
		keycode = 0;
	} else {
		lradc->cur_keycode = keycode;
		lradc->stat_keydown++;
	}
	spin_unlock_irqrestore(&lradc->lock, flags);

	if (idx < 0) {
		dev_info(lradc->dev,
			 "raw=%u mv=%u -> no matching key\n", raw, mv);
		return 0;
	}

	/* 锁外上报：input 子系统内部有自己的锁 */
	if (lradc->input) {
		input_report_key(lradc->input, keycode, 1);
		input_sync(lradc->input);
	}

	dev_info(lradc->dev, "key press code=%u (raw=%u mv=%u key#%d)\n",
		 keycode, raw, mv, idx);
	return keycode;
}

/*
 * 释放当前按下的键。返回被释放的键码（0 表示本来就没有键按下）。
 */
static u32 wn_lradc_release(struct wn_lradc *lradc)
{
	unsigned long flags;
	u32 keycode;

	spin_lock_irqsave(&lradc->lock, flags);
	keycode = lradc->cur_keycode;
	lradc->cur_keycode = 0;
	if (keycode)
		lradc->stat_keyup++;
	spin_unlock_irqrestore(&lradc->lock, flags);

	if (!keycode)
		return 0;

	if (lradc->input) {
		input_report_key(lradc->input, keycode, 0);
		input_sync(lradc->input);
	}
	dev_info(lradc->dev, "key release code=%u\n", keycode);
	return keycode;
}

/* ------------------------------------------------------------------ */
/* 中断处理                                                            */
/* ------------------------------------------------------------------ */

/*
 * 中断上下文约束（驱动开发必考点）：
 *   - 不能睡眠：不能调用 kmalloc(GFP_KERNEL)、mutex_lock、copy_to_user 等
 *   - 不能做长耗时操作
 *   本 handler 只做 readl/writel + 内存写，完全满足。
 *
 * 共享状态的保护：
 *   @cur_keycode 会被这里的「中断上下文」与 debugfs 的「进程上下文」同时读，
 *   所以用 spinlock 而非 mutex（mutex 不能用在中断里）。
 */
static irqreturn_t wn_lradc_irq(int irq, void *dev_id)
{
	struct wn_lradc *lradc = dev_id;
	unsigned long flags;
	u32 ints, raw;
	bool handled = false;

	/* 先读中断状态；判不出源就直接返回 IRQ_NONE，让内核统计 spurious */
	ints = wn_lradc_read(lradc, LRADC_INTS);
	if (!(ints & (CHAN0_KEYUP_IRQ | CHAN0_KEYDOWN_IRQ |
		      CHAN0_HOLD_IRQ | CHAN0_ALRDY_HOLD_IRQ |
		      CHAN0_DATA_IRQ))) {
		spin_lock_irqsave(&lradc->lock, flags);
		lradc->stat_unknown++;
		lradc->stat_irq++;
		spin_unlock_irqrestore(&lradc->lock, flags);
		return IRQ_NONE;
	}

	spin_lock_irqsave(&lradc->lock, flags);
	lradc->stat_irq++;
	spin_unlock_irqrestore(&lradc->lock, flags);

	/* 抬起：上报 release 并清掉缓存键码 */
	if (ints & CHAN0_KEYUP_IRQ) {
		wn_lradc_release(lradc);
		handled = true;
	}

	/*
	 * 转换完成中断：只在 debugfs 自检期间被使能（正常运行时 INTC 里没有这一位）。
	 * 它证明 ADC 真的在跑、ISR 真的被调用，是「无物理按键」环境下
	 * 验证 L2 中断通路的唯一手段。
	 */
	if (ints & CHAN0_DATA_IRQ) {
		raw = wn_lradc_read(lradc, LRADC_DATA0) & LRADC_DATA_MASK;

		spin_lock_irqsave(&lradc->lock, flags);
		lradc->last_raw = raw;
		lradc->last_mv = wn_lradc_raw_to_mv(lradc, raw);
		if (lradc->selftest_done)
			lradc->selftest_irqs++;
		lradc->selftest_raw = raw;
		spin_unlock_irqrestore(&lradc->lock, flags);

		handled = true;
	}

	/*
	 * 按下：只有在当前无键时才处理，避免重复触发把同一键反复上报。
	 * HOLD / ALRDY_HOLD 类中断走同一套语义——按键一直按着时硬件会重复
	 * 产生这两类中断，但因为 @cur_keycode 非 0，这里不会重复上报。
	 */
	if (ints & (CHAN0_KEYDOWN_IRQ | CHAN0_HOLD_IRQ | CHAN0_ALRDY_HOLD_IRQ)) {
		bool idle;

		spin_lock_irqsave(&lradc->lock, flags);
		idle = (lradc->cur_keycode == 0);
		spin_unlock_irqrestore(&lradc->lock, flags);

		if (idle) {
			raw = wn_lradc_read(lradc, LRADC_DATA0) & LRADC_DATA_MASK;
			wn_lradc_report_raw(lradc, raw);
		}
		handled = true;
	}

	/* 写 1 清中断状态位（W1C）。必须在处理完之后清，否则会丢事件 */
	wn_lradc_write(lradc, LRADC_INTS, ints);

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

/* ------------------------------------------------------------------ */
/* ADC 使能 / 关闭                                                     */
/* ------------------------------------------------------------------ */

/*
 * 使能 ADC。
 *
 * 寄存器取值与 mainline 一致：
 *   FIRST_CONVERT_DLY(2)  首次转换延时
 *   LEVELA_B_CNT(1)       电平判决次数
 *   HOLD_EN(1)            按住不放时重复上报
 *   SAMPLE_RATE(0)        250 Hz / 4 ms
 *   ENABLE(1)
 *
 * 注意：本板实测初始 CTRL=0x01000168，其中 FIRST_CONVERT_DLY 为 1 而非 2，
 * 且 ENABLE=0。这里按 mainline 的 2 写入，probe 后会把实测值打到日志里，
 * 便于对照「我确实改动了硬件」。
 */
static int wn_lradc_enable(struct wn_lradc *lradc)
{
	int ret;

	ret = clk_prepare_enable(lradc->clk);
	if (ret) {
		dev_err(lradc->dev, "clk_prepare_enable failed: %d\n", ret);
		return ret;
	}

	wn_lradc_write(lradc, LRADC_CTRL,
		       FIRST_CONVERT_DLY(2) | LEVELA_B_CNT(1) | HOLD_EN(1) |
		       SAMPLE_RATE(LRADC_SAMPLE_RATE_VAL) | ENABLE(1));
	wn_lradc_write(lradc, LRADC_INTC, CHAN0_KEYUP_IRQ | CHAN0_KEYDOWN_IRQ);

	lradc->is_open = true;
	return 0;
}

static void wn_lradc_disable(struct wn_lradc *lradc)
{
	/*
	 * 幂等：probe 失败路径上可能还没 enable 过就跳到这里，
	 * 直接 clk_disable_unprepare() 会触发内核的时钟引用计数告警。
	 */
	if (!lradc->is_open)
		return;

	/* 关中断使能，再关 ADC，最后关时钟。顺序反了会留下挂起中断 */
	wn_lradc_write(lradc, LRADC_INTC, 0);
	wn_lradc_write(lradc, LRADC_CTRL,
		       FIRST_CONVERT_DLY(2) | LEVELA_B_CNT(1) | HOLD_EN(1) |
		       SAMPLE_RATE(2));	/* ENABLE(0) */
	clk_disable_unprepare(lradc->clk);
	lradc->is_open = false;
}

/* ------------------------------------------------------------------ */
/* input 子系统（L3）                                                   */
/* ------------------------------------------------------------------ */

/*
 * @input->open / @input->close 是内核对「有没有用户态在听」这件事的
 * 唯一权威通知。evdev 被第一次 open 时调 open，最后一个 fd 关闭时调 close。
 *
 * 所以省电逻辑放在这里，而不是 probe/remove 里：
 *   probe   → 注册设备、申请中断，但 ADC 不转换（is_open = false）
 *   有人 open /dev/input/eventN → ADC 开始转换，中断通路激活
 *   所有人关闭           → ADC 停止，时钟关闭，板子回到零功耗
 *
 * 两个回调都在进程上下文执行（持 input->mutex），允许睡眠，
 * 因此可以直接 clk_prepare_enable()。
 */
static int wn_lradc_input_open(struct input_dev *input)
{
	struct wn_lradc *lradc = input_get_drvdata(input);
	int ret;

	ret = wn_lradc_enable(lradc);
	if (ret) {
		dev_err(lradc->dev, "enable on open failed: %d\n", ret);
		return ret;
	}

	dev_info(lradc->dev, "evdev opened: adc enabled\n");
	return 0;
}

static void wn_lradc_input_close(struct input_dev *input)
{
	struct wn_lradc *lradc = input_get_drvdata(input);

	wn_lradc_disable(lradc);
	dev_info(lradc->dev, "evdev closed: adc disabled\n");
}

/*
 * 分配并注册 input 设备。
 *
 * 用 devm_input_allocate_device() 而不是 input_allocate_device()：
 * 前者挂 devres，驱动解绑时自动 input_unregister_device()，
 * 不会漏掉 error path 上的释放（这类泄漏在真实驱动里很常见）。
 */
static int wn_lradc_input_init(struct wn_lradc *lradc)
{
	struct device *dev = lradc->dev;
	struct input_dev *input;
	u32 i;

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = "weaknet-lradc-keys";
	input->phys = "weaknet-lradc/input0";
	input->id.bustype = BUS_HOST;
	input->dev.parent = dev;

	/*
	 * 能力位：EV_KEY 表示「我能发按键」，EV_SYN 表示「我会发同步包」。
	 * 少了 EV_SYN，用户态 evdev 客户端会一直等不到事件边界，看起来像丢键。
	 */
	set_bit(EV_KEY, input->evbit);
	set_bit(EV_SYN, input->evbit);

	/* 每个解析出来的按键都要显式声明能力，否则内核拒绝上报该键码 */
	for (i = 0; i < lradc->nr_keys; i++) {
		set_bit(lradc->keys[i].keycode, input->keybit);
		dev_info(dev, "declare key%u: code %u at %u mV (raw ~%u)\n",
			 i, lradc->keys[i].keycode,
			 lradc->keys[i].voltage_mv, lradc->keys[i].raw);
	}

	input->open  = wn_lradc_input_open;
	input->close = wn_lradc_input_close;

	input_set_drvdata(input, lradc);

	/*
	 * input_register_device() 之后 /dev/input/eventN 就出现了，
	 * 但此时 ADC 还没开始转换——要等用户态 open 才启动。
	 */
	if (input_register_device(input)) {
		dev_err(dev, "input_register_device failed\n");
		return -ENODEV;
	}

	lradc->input = input;

	dev_info(dev, "input device registered: '%s' (%u keys)\n",
		 input->name, lradc->nr_keys);
	return 0;
}

/* ------------------------------------------------------------------ */
/* DT 解析                                                             */
/* ------------------------------------------------------------------ */

/*
 * 从 compatible 字符串里取参考电压，形如 "allwinner,keyboard_1350mv"。
 * 取不到就返回默认值。这是本板 BSP 的私有约定，mainline 没有。
 */
static u32 wn_lradc_parse_vref(struct device *dev)
{
	const char *compat;
	u32 mv = LRADC_DEFAULT_VREF_MV;
	int ret;

	ret = of_property_read_string(dev->of_node, "compatible", &compat);
	if (ret)
		return mv;

	/*
	 * 在字符串里找 "1350mv" 这样的片段。不用 sscanf 是因为
	 * compatible 可能带多个逗号分隔项，直接定位数字更稳。
	 */
	while (*compat) {
		if (*compat >= '0' && *compat <= '9') {
			u32 val = 0;
			const char *p = compat;

			while (*p >= '0' && *p <= '9') {
				val = val * 10 + (*p - '0');
				p++;
			}
			/* 紧跟 "mv" 才认，避免把 "a733" 之类误当电压 */
			if (p[0] == 'm' && p[1] == 'v') {
				if (val >= 500 && val <= 5000)
					mv = val;
				break;
			}
			compat = p;
			continue;
		}
		compat++;
	}

	return mv;
}

/*
 * 解析按键表。
 *
 * 本板 DT 格式（与 mainline 完全不同）：
 *     key_cnt = <5>;
 *     key0 = <210 115>;      电压 210 mV，键码 115
 *     key1 = <410 114>;
 *     ...
 *
 * 返回 0 表示成功；负值表示失败。
 */
static int wn_lradc_parse_keymap(struct wn_lradc *lradc)
{
	struct device *dev = lradc->dev;
	struct device_node *np = dev->of_node;
	u32 key_cnt = 0;
	u32 i;
	int ret;

	ret = of_property_read_u32(np, "key_cnt", &key_cnt);
	if (ret) {
		dev_err(dev, "no key_cnt property in DT\n");
		return -EINVAL;
	}
	if (key_cnt == 0) {
		dev_err(dev, "key_cnt is 0\n");
		return -EINVAL;
	}
	if (key_cnt > LRADC_MAX_KEYS) {
		dev_warn(dev, "key_cnt %u exceeds max %d, truncating\n",
			 key_cnt, LRADC_MAX_KEYS);
		key_cnt = LRADC_MAX_KEYS;
	}

	for (i = 0; i < key_cnt; i++) {
		char prop[16];
		u32 pair[2];

		snprintf(prop, sizeof(prop), "key%u", i);
		ret = of_property_read_u32_array(np, prop, pair, 2);
		if (ret) {
			dev_warn(dev, "%s missing or malformed, skipping\n", prop);
			continue;
		}

		lradc->keys[lradc->nr_keys].voltage_mv = pair[0];
		lradc->keys[lradc->nr_keys].keycode = pair[1];
		/* 换算成 6bit 期望码值，仅用于日志对照，不参与判定 */
		lradc->keys[lradc->nr_keys].raw =
			(pair[0] * LRADC_FULL_SCALE + lradc->vref_mv / 2) /
			lradc->vref_mv;
		lradc->nr_keys++;
	}

	if (lradc->nr_keys == 0) {
		dev_err(dev, "no valid key parsed\n");
		return -EINVAL;
	}

	dev_info(dev, "parsed %u keys (vref=%u mV):\n", lradc->nr_keys,
		 lradc->vref_mv);
	for (i = 0; i < lradc->nr_keys; i++)
		dev_info(dev, "  key%u: %u mV -> KEY code %u (expect raw ~%u)\n",
			 i, lradc->keys[i].voltage_mv, lradc->keys[i].keycode,
			 lradc->keys[i].raw);

	return 0;
}

/* ------------------------------------------------------------------ */
/* debugfs                                                             */
/* ------------------------------------------------------------------ */

static int wn_lradc_dbg_regs_show(struct seq_file *s, void *unused)
{
	struct wn_lradc *lradc = s->private;
	unsigned long flags;
	u32 ctrl, intc, ints, d0, d1;
	u64 n_irq, n_down, n_up, n_unk, n_unm;
	u32 cur, lraw, lmv;

	/* 寄存器是硬件状态，直接读，不需要锁 */
	ctrl = wn_lradc_read(lradc, LRADC_CTRL);
	intc = wn_lradc_read(lradc, LRADC_INTC);
	ints = wn_lradc_read(lradc, LRADC_INTS);
	d0   = wn_lradc_read(lradc, LRADC_DATA0);
	d1   = wn_lradc_read(lradc, LRADC_DATA1);

	spin_lock_irqsave(&lradc->lock, flags);
	n_irq  = lradc->stat_irq;
	n_down = lradc->stat_keydown;
	n_up   = lradc->stat_keyup;
	n_unk  = lradc->stat_unknown;
	n_unm  = lradc->stat_unmatched;
	cur    = lradc->cur_keycode;
	lraw   = lradc->last_raw;
	lmv    = lradc->last_mv;
	spin_unlock_irqrestore(&lradc->lock, flags);

	seq_printf(s, "== registers ==\n");
	seq_printf(s, "  CTRL   [0x%02x] = 0x%08x  (ENABLE=%u, SAMPLE_RATE=%u)\n",
		   LRADC_CTRL, ctrl, ctrl & 1, (ctrl >> 2) & 0x3);
	seq_printf(s, "  INTC   [0x%02x] = 0x%08x\n", LRADC_INTC, intc);
	seq_printf(s, "  INTS   [0x%02x] = 0x%08x\n", LRADC_INTS, ints);
	seq_printf(s, "  DATA0  [0x%02x] = 0x%08x  (6bit raw=%u, %u mV)\n",
		   LRADC_DATA0, d0, d0 & LRADC_DATA_MASK,
		   wn_lradc_raw_to_mv(lradc, d0 & LRADC_DATA_MASK));
	seq_printf(s, "  DATA1  [0x%02x] = 0x%08x  (6bit raw=%u)\n",
		   LRADC_DATA1, d1, d1 & LRADC_DATA_MASK);

	seq_printf(s, "\n== state ==\n");
	seq_printf(s, "  adc_enabled   : %s\n", lradc->is_open ? "yes" : "no");
	seq_printf(s, "  cur_keycode   : %u%s\n", cur, cur ? "" : " (idle)");
	seq_printf(s, "  last_raw/mv   : %u / %u mV\n", lraw, lmv);
	seq_printf(s, "  vref          : %u mV\n", lradc->vref_mv);
	seq_printf(s, "  nr_keys       : %u\n", lradc->nr_keys);

	seq_printf(s, "\n== counters ==\n");
	seq_printf(s, "  irq           : %llu\n", n_irq);
	seq_printf(s, "  keydown       : %llu\n", n_down);
	seq_printf(s, "  keyup         : %llu\n", n_up);
	seq_printf(s, "  unknown_irq   : %llu\n", n_unk);
	seq_printf(s, "  unmatched_key : %llu\n", n_unm);

	seq_printf(s, "\n== keymap ==\n");
	{
		u32 i;

		for (i = 0; i < lradc->nr_keys; i++)
			seq_printf(s, "  key%-2u %4u mV -> code %-4u (expect raw ~%u)\n",
				   i, lradc->keys[i].voltage_mv,
				   lradc->keys[i].keycode,
				   lradc->keys[i].raw);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(wn_lradc_dbg_regs);

/*
 * 手动触发一次「无中断的直读采样」，用于验证 ADC 在没有按键按下时
 * 也能被读到值。写任意字符触发一次。
 */
static ssize_t wn_lradc_dbg_sample_write(struct file *file,
					 const char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct wn_lradc *lradc = s->private;
	unsigned long flags;
	u32 raw, mv;

	raw = wn_lradc_read(lradc, LRADC_DATA0) & LRADC_DATA_MASK;
	mv = wn_lradc_raw_to_mv(lradc, raw);

	spin_lock_irqsave(&lradc->lock, flags);
	lradc->last_raw = raw;
	lradc->last_mv = mv;
	spin_unlock_irqrestore(&lradc->lock, flags);

	dev_info(lradc->dev, "manual sample: raw=%u mv=%u\n", raw, mv);

	return count;
}

static int wn_lradc_dbg_sample_show(struct seq_file *s, void *unused)
{
	struct wn_lradc *lradc = s->private;
	u32 raw = wn_lradc_read(lradc, LRADC_DATA0) & LRADC_DATA_MASK;

	seq_printf(s, "raw=%u mv=%u\n", raw, wn_lradc_raw_to_mv(lradc, raw));
	return 0;
}

static int wn_lradc_dbg_sample_open(struct inode *inode, struct file *file)
{
	return single_open(file, wn_lradc_dbg_sample_show, inode->i_private);
}

/*
 * 中断通路自检。
 *
 * 背景：本板 DT 声明了 5 个按键，但 PCB 上不一定焊接了对应的物理按键。
 * 若没有物理按键，KEYDOWN/KEYUP 中断永远不会产生，L2 的「中断真的接通了」
 * 就没有直接证据（只有 request_irq 成功这种间接证据）。
 *
 * 做法：临时往 LRADC_INTC 里加上 CHAN0_DATA_IRQ。ADC 使能后每次转换完成
 * 都会置这一位，于是 ISR 必然被调用。等 100 ms 收集到足够的中断次数，
 * 再把它关掉，恢复成只有 KEYDOWN|KEYUP 的正常状态。
 *
 * 这不是「伪造」证据：中断是真的从中断控制器上打进来的，ISR 真的执行了，
 * 真的读了 DATA0。它只是让 ADC 在不按键时也持续产生中断而已。
 *
 * 写入任意内容即触发一次自检。
 */
static ssize_t wn_lradc_dbg_selftest_write(struct file *file,
					   const char __user *ubuf,
					   size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct wn_lradc *lradc = s->private;
	unsigned long flags;
	u64 before, after;
	bool was_open;
	u32 raw;
	int ret;

	/*
	 * L3 之后 ADC 默认不使能（等 evdev open）。但自检必须让 ADC 真的跑起来
	 * 才能验证中断通路，所以这里临时自己开——测完再恢复原状态。
	 * 若已有用户态在听（was_open=true），就沿用它的使能状态，不能替它关。
	 */
	was_open = lradc->is_open;
	if (!was_open) {
		ret = wn_lradc_enable(lradc);
		if (ret) {
			dev_err(lradc->dev, "selftest: enable adc failed: %d\n",
				ret);
			return ret;
		}
	}

	spin_lock_irqsave(&lradc->lock, flags);
	before = lradc->stat_irq;
	lradc->selftest_irqs = 0;
	lradc->selftest_done = true;
	spin_unlock_irqrestore(&lradc->lock, flags);

	/* 打开「转换完成」中断，让 ADC 无键也持续产生中断 */
	wn_lradc_write(lradc, LRADC_INTC,
		       CHAN0_KEYUP_IRQ | CHAN0_KEYDOWN_IRQ | CHAN0_DATA_IRQ);

	/* 250 Hz 下 100 ms 约 25 次转换，足够证明通路 */
	msleep(100);

	/* 关掉自检中断位，恢复正常运行时的掩码 */
	wn_lradc_write(lradc, LRADC_INTC, CHAN0_KEYUP_IRQ | CHAN0_KEYDOWN_IRQ);

	spin_lock_irqsave(&lradc->lock, flags);
	after = lradc->stat_irq;
	lradc->selftest_done = false;
	raw = lradc->selftest_raw;
	spin_unlock_irqrestore(&lradc->lock, flags);

	dev_info(lradc->dev,
		 "selftest: %llu irqs in 100ms (total %llu->%llu), last raw=%u\n",
		 after - before, before, after, raw);

	if (after == before)
		dev_err(lradc->dev,
			"selftest FAILED: no interrupt observed, ISR path is dead\n");
	else
		dev_info(lradc->dev, "selftest PASSED: ISR path is alive\n");

	/* 恢复：如果使能是我们自己加的，就自己关掉，别改变原有状态 */
	if (!was_open)
		wn_lradc_disable(lradc);

	return count;
}

static int wn_lradc_dbg_selftest_show(struct seq_file *s, void *unused)
{
	struct wn_lradc *lradc = s->private;
	unsigned long flags;
	u64 total, self, raw;
	bool done;

	spin_lock_irqsave(&lradc->lock, flags);
	total = lradc->stat_irq;
	self  = lradc->selftest_irqs;
	raw   = lradc->selftest_raw;
	done  = lradc->selftest_done;
	spin_unlock_irqrestore(&lradc->lock, flags);

	seq_printf(s, "adc_enabled : %s\n", lradc->is_open ? "yes" : "no");
	seq_printf(s, "irq_total   : %llu\n", total);
	seq_printf(s, "selftest_irqs: %llu\n", self);
	seq_printf(s, "selftest_raw : %llu\n", raw);
	seq_printf(s, "running     : %s\n", done ? "yes" : "no");
	seq_printf(s, "\nwrite anything to this file to run a 100ms self-test\n");
	return 0;
}

static int wn_lradc_dbg_selftest_open(struct inode *inode, struct file *file)
{
	return single_open(file, wn_lradc_dbg_selftest_show, inode->i_private);
}

static const struct file_operations wn_lradc_dbg_selftest_fops = {
	.owner   = THIS_MODULE,
	.open    = wn_lradc_dbg_selftest_open,
	.read    = seq_read,
	.write   = wn_lradc_dbg_selftest_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/*
 * 原始寄存器读写口（调试专用）。
 *
 * 写入格式："<偏移> <值>"，两个都是十六进制，例：
 *     echo "00 02000141" > /sys/kernel/debug/weaknet_lradc/raw
 *
 * 用途：本板的 LRADC 在 vendor BSP 里没有驱动，行为只能靠实测确定。
 * 有了这个口就能在板端直接扫 CTRL/INTC 的各种组合，不必反复改代码重编译。
 * 读回值会同时打到 dmesg，用于确认写入是否真的落到了硬件。
 */
static ssize_t wn_lradc_dbg_raw_write(struct file *file,
				      const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct wn_lradc *lradc = s->private;
	char buf[64];
	unsigned int reg, val;
	u32 readback;
	int n;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	n = sscanf(buf, "%x %x", &reg, &val);
	if (n != 2)
		return -EINVAL;
	/*
	 * 允许整个 0x100 窗口（4 字节对齐）。
	 *
	 * 最初只开放到 DATA1(0x10)，理由是「已知布局就这几个寄存器」。
	 * 但 A733 的 LRADC 与 mainline sun4i 的同名 IP 行为并不一致：
	 * CTRL 写入成功、时钟已开、复位已解，DATA0 却始终停在复位值 0x3f。
	 * 说明还有未知寄存器（或窗口外的模拟控制位）参与使能，
	 * 必须能扫描整段窗口才能定位。这是调试口存在的意义。
	 */
	if (reg > 0xfc || (reg & 0x3))
		return -EINVAL;

	wn_lradc_write(lradc, reg, val);
	readback = wn_lradc_read(lradc, reg);

	dev_info(lradc->dev, "raw: reg[0x%02x] <= 0x%08x, readback 0x%08x\n",
		 reg, val, readback);

	return count;
}

static int wn_lradc_dbg_raw_show(struct seq_file *s, void *unused)
{
	struct wn_lradc *lradc = s->private;

	u32 off, val;

	seq_printf(s, "usage: echo \"<offset_hex> <value_hex>\" > raw\n");
	seq_printf(s, "regs: 00 CTRL / 04 INTC / 08 INTS / 0c DATA0 / 10 DATA1\n\n");

	seq_printf(s, "CTRL   = 0x%08x\n", wn_lradc_read(lradc, LRADC_CTRL));
	seq_printf(s, "INTC   = 0x%08x\n", wn_lradc_read(lradc, LRADC_INTC));
	seq_printf(s, "INTS   = 0x%08x\n", wn_lradc_read(lradc, LRADC_INTS));
	seq_printf(s, "DATA0  = 0x%08x\n", wn_lradc_read(lradc, LRADC_DATA0));
	seq_printf(s, "DATA1  = 0x%08x\n", wn_lradc_read(lradc, LRADC_DATA1));

	/*
	 * 全窗口扫描：只打印非零项与已知布局项，避免刷屏。
	 * 已知项即使为 0 也打印，便于确认「确实读到了 0」而非越界。
	 */
	seq_printf(s, "\nfull window 0x00-0xfc (nonzero + known):\n");
	for (off = 0; off <= 0xfc; off += 4) {
		val = wn_lradc_read(lradc, off);
		if (val || off <= LRADC_DATA1)
			seq_printf(s, "  [0x%02x] = 0x%08x\n", off, val);
	}

	return 0;
}

static int wn_lradc_dbg_raw_open(struct inode *inode, struct file *file)
{
	return single_open(file, wn_lradc_dbg_raw_show, inode->i_private);
}

static const struct file_operations wn_lradc_dbg_raw_fops = {
	.owner   = THIS_MODULE,
	.open    = wn_lradc_dbg_raw_open,
	.read    = seq_read,
	.write   = wn_lradc_dbg_raw_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

/*
 * L3 注入口：直接喂一个 raw 值，走完整的「换算 → 匹配 → input 上报」链路。
 *
 * 存在理由（不是玩具）：本板 ADC 从不转换（见文件头「实测边界」），
 * 物理按键永远产生不了中断，L3 就永远拿不到端到端证据。
 * 这个口把硬件那一端短路掉，让上层的匹配、去重、input_report_key、
 * evdev 分发全部真实执行一次，从而可以证明：
 *   - keybit 声明正确（漏声明时内核会丢弃该键码，用户态什么都收不到）
 *   - input_report_key + input_sync 产生了合法的 evdev 事件
 *   - 去重逻辑（同一个键按住不重复上报）成立
 *
 * 写入格式：
 *   echo <raw 0-63> > inject     注入一次按下
 *   echo release    > inject     注入一次抬起
 */
static ssize_t wn_lradc_dbg_inject_write(struct file *file,
					 const char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct wn_lradc *lradc = s->private;
	char buf[32];
	unsigned int raw;
	bool busy;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (sysfs_streq(buf, "release")) {
		wn_lradc_release(lradc);
		return count;
	}

	if (sscanf(buf, "%u", &raw) != 1) {
		dev_warn(lradc->dev,
			 "inject: usage \"<raw 0-63>\" or \"release\"\n");
		return -EINVAL;
	}

	if (raw > LRADC_FULL_SCALE) {
		dev_warn(lradc->dev, "inject: raw %u out of range 0-%u\n",
			 raw, LRADC_FULL_SCALE);
		return -EINVAL;
	}

	/*
	 * 若已有键按着（cur_keycode 非 0），先抬起再按新键，
	 * 模拟真实的「手指从 A 键滑到 B 键」——用户态会看到 release+press 一对事件。
	 * 不做这一步的话第二次注入会被去重逻辑吞掉，看起来像「注入无效」。
	 */
	spin_lock_irq(&lradc->lock);
	busy = (lradc->cur_keycode != 0);
	spin_unlock_irq(&lradc->lock);

	if (busy)
		wn_lradc_release(lradc);

	wn_lradc_report_raw(lradc, raw);

	return count;
}

static int wn_lradc_dbg_inject_show(struct seq_file *s, void *unused)
{
	struct wn_lradc *lradc = s->private;
	unsigned long flags;
	u32 last_raw, cur;
	int i;

	spin_lock_irqsave(&lradc->lock, flags);
	last_raw = lradc->last_raw;
	cur = lradc->cur_keycode;
	spin_unlock_irqrestore(&lradc->lock, flags);

	seq_puts(s, "usage: echo \"<raw 0-63>\" > inject   (inject a key press)\n");
	seq_puts(s, "       echo release          > inject   (inject a key release)\n\n");
	seq_printf(s, "input device : %s\n",
		   lradc->input ? "registered" : "absent");
	seq_printf(s, "cur_keycode  : %u\n", cur);
	seq_printf(s, "last_raw     : %u (mv %u)\n", last_raw,
		   wn_lradc_raw_to_mv(lradc, last_raw));
	seq_puts(s, "\nraw -> voltage -> keycode table:\n");
	for (i = 0; i < lradc->nr_keys; i++)
		seq_printf(s, "  raw %2u = %4u mV -> code %u\n",
			   lradc->keys[i].raw,
			   lradc->keys[i].voltage_mv,
			   lradc->keys[i].keycode);
	return 0;
}

static int wn_lradc_dbg_inject_open(struct inode *inode, struct file *file)
{
	return single_open(file, wn_lradc_dbg_inject_show, inode->i_private);
}

static const struct file_operations wn_lradc_dbg_inject_fops = {
	.owner   = THIS_MODULE,
	.open    = wn_lradc_dbg_inject_open,
	.read    = seq_read,
	.write   = wn_lradc_dbg_inject_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static const struct file_operations wn_lradc_dbg_sample_fops = {
	.owner   = THIS_MODULE,
	.open    = wn_lradc_dbg_sample_open,
	.read    = seq_read,
	.write   = wn_lradc_dbg_sample_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

static void wn_lradc_debugfs_init(struct wn_lradc *lradc)
{
	lradc->dbg_dir = debugfs_create_dir("weaknet_lradc", NULL);
	if (IS_ERR_OR_NULL(lradc->dbg_dir)) {
		lradc->dbg_dir = NULL;
		dev_warn(lradc->dev, "debugfs unavailable, skip\n");
		return;
	}

	debugfs_create_file("regs", 0444, lradc->dbg_dir, lradc,
			    &wn_lradc_dbg_regs_fops);
	debugfs_create_file("sample", 0644, lradc->dbg_dir, lradc,
			    &wn_lradc_dbg_sample_fops);
	debugfs_create_file("selftest", 0644, lradc->dbg_dir, lradc,
			    &wn_lradc_dbg_selftest_fops);
	debugfs_create_file("raw", 0644, lradc->dbg_dir, lradc,
			    &wn_lradc_dbg_raw_fops);
	debugfs_create_file("inject", 0644, lradc->dbg_dir, lradc,
			    &wn_lradc_dbg_inject_fops);
}

/* ------------------------------------------------------------------ */
/* probe / remove                                                      */
/* ------------------------------------------------------------------ */

static int wn_lradc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wn_lradc *lradc;
	u32 ctrl_before;
	int ret;

	lradc = devm_kzalloc(dev, sizeof(*lradc), GFP_KERNEL);
	if (!lradc)
		return -ENOMEM;

	lradc->dev = dev;
	spin_lock_init(&lradc->lock);
	platform_set_drvdata(pdev, lradc);

	/* ---- 1. 映射寄存器 ---- */
	lradc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(lradc->base)) {
		ret = PTR_ERR(lradc->base);
		dev_err(dev, "ioremap failed: %d\n", ret);
		return ret;
	}
	dev_info(dev, "mapped registers at %pR\n",
		 platform_get_resource(pdev, IORESOURCE_MEM, 0));

	/* ---- 2. 参考电压 + 按键表（先解析，失败就不用申请资源了）---- */
	lradc->vref_mv = wn_lradc_parse_vref(dev);

	ret = wn_lradc_parse_keymap(lradc);
	if (ret)
		return ret;

	/* ---- 3. 时钟 ---- */
	/*
	 * DT 里没有 clock-names，只有一个 clocks 项，
	 * 所以 devm_clk_get(dev, NULL) 取第一个（也是唯一一个）。
	 */
	lradc->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(lradc->clk)) {
		ret = PTR_ERR(lradc->clk);
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_err(dev, "get clock failed: %d\n", ret);
		return ret;
	}
	dev_info(dev, "clock rate = %lu Hz\n", clk_get_rate(lradc->clk));

	/* ---- 4. 复位（可选，DT 里没有 reset-names）---- */
	lradc->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(lradc->rst)) {
		ret = PTR_ERR(lradc->rst);
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_warn(dev, "reset control unavailable: %d, continue\n", ret);
		lradc->rst = NULL;
	}
	if (lradc->rst) {
		ret = reset_control_reset(lradc->rst);
		if (ret) {
			ret = reset_control_deassert(lradc->rst);
			if (ret)
				dev_warn(dev, "reset deassert failed: %d\n", ret);
		}
		dev_info(dev, "reset controller deasserted\n");
	}

	/* ---- 5. 记录使能前的寄存器原值（作为「我真的动过硬件」的证据）---- */
	ctrl_before = wn_lradc_read(lradc, LRADC_CTRL);
	dev_info(dev, "CTRL before enable = 0x%08x\n", ctrl_before);

	/* ---- 6. 中断必须先注册 ---- */
	/*
	 * 顺序很重要：INTC 里的中断使能位一写下去，硬件就可能立刻拉高 IRQ 线。
	 * 如果先 enable(ADC) 再 request_irq()，中间这段窗口期的中断没有 handler，
	 * 内核会记成 spurious IRQ；中断线是电平触发时还会一直挂着直到有人清。
	 * 所以这里先注册 handler，再用 wn_lradc_enable() 去开硬件中断。
	 */
	lradc->irq = platform_get_irq(pdev, 0);
	if (lradc->irq < 0) {
		ret = lradc->irq;
		dev_err(dev, "get irq failed: %d\n", ret);
		goto err_disable;
	}

	ret = devm_request_irq(dev, lradc->irq, wn_lradc_irq, 0,
			       "weaknet-lradc", lradc);
	if (ret) {
		dev_err(dev, "request_irq(%d) failed: %d\n", lradc->irq, ret);
		goto err_disable;
	}
	dev_info(dev, "irq %d registered\n", lradc->irq);

	/* ---- 7. input 设备（L3）---- */
	/*
	 * 必须在 request_irq() 之后：input_register_device() 一返回，
	 * /dev/input/eventN 立刻可被 open，而 open 回调会使能 ADC 并打中断使能位。
	 * 如果那时 handler 还没注册，电平中断会一直挂着直到内核判定 spurious。
	 *
	 * 注意这里不调用 wn_lradc_enable()——ADC 保持关闭，
	 * 直到真有进程 open evdev。这是键盘类驱动的标准省电语义。
	 */
	ret = wn_lradc_input_init(lradc);
	if (ret) {
		dev_err(dev, "input init failed: %d\n", ret);
		goto err_disable;
	}

	/* ---- 8. debugfs ---- */
	wn_lradc_debugfs_init(lradc);

	dev_info(dev, "probe done: %u keys, vref %u mV, sample %u Hz\n",
		 lradc->nr_keys, lradc->vref_mv,
		 LRADC_SAMPLE_RATE_VAL == 0 ? 250 :
		 LRADC_SAMPLE_RATE_VAL == 1 ? 125 :
		 LRADC_SAMPLE_RATE_VAL == 2 ? 62 : 31);
	dev_info(dev, "CTRL after probe = 0x%08x (ENABLE=%u, adc idle until evdev open)\n",
		 wn_lradc_read(lradc, LRADC_CTRL),
		 wn_lradc_read(lradc, LRADC_CTRL) & 1);

	return 0;

err_disable:
	wn_lradc_disable(lradc);
	return ret;
}

static int wn_lradc_remove(struct platform_device *pdev)
{
	struct wn_lradc *lradc = platform_get_drvdata(pdev);

	/*
	 * devm 会自动回收 irq / ioremap / clk 引用，
	 * 但 ADC 使能与时钟 enable 计数是我们手动加的，必须手动关。
	 */
	wn_lradc_disable(lradc);

	debugfs_remove_recursive(lradc->dbg_dir);
	lradc->dbg_dir = NULL;

	dev_info(lradc->dev, "removed: irq=%llu keydown=%llu keyup=%llu\n",
		 lradc->stat_irq, lradc->stat_keydown, lradc->stat_keyup);

	return 0;
}

/* ------------------------------------------------------------------ */
/* 驱动注册                                                            */
/* ------------------------------------------------------------------ */

static const struct of_device_id wn_lradc_of_match[] = {
	/*
	 * 本板 DT：compatible = "allwinner,keyboard_1350mv";
	 * 这个字符串在 mainline 里不存在，是 Allwinner BSP 的私有约定。
	 */
	{ .compatible = "allwinner,keyboard_1350mv" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wn_lradc_of_match);

static struct platform_driver wn_lradc_driver = {
	.probe  = wn_lradc_probe,
	.remove = wn_lradc_remove,
	.driver = {
		.name           = "weaknet-lradc-keys",
		.of_match_table = wn_lradc_of_match,
	},
};
module_platform_driver(wn_lradc_driver);

MODULE_AUTHOR("tanqf");
MODULE_DESCRIPTION("WeakNet LRADC keypad driver for Allwinner A733 (sun60iw2p1)");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");
