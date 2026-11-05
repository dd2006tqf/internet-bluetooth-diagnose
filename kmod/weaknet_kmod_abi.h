/* SPDX-License-Identifier: GPL-2.0 */
/*
 * weaknet_kmod_abi.h - WeakNet 实验性内核模块与用户态共享的 ABI 定义
 *
 * 本头文件同时被内核模块和用户态工具包含，因此必须保持纯数据结构，
 * 不得引入任一侧专有的头文件。
 *
 * ABI 版本变更规则：只要下面的结构体布局发生变化，必须递增
 * WEAKNET_KMOD_ABI_VERSION，用户态工具据此拒绝读取不兼容的模块。
 */
#ifndef WEAKNET_KMOD_ABI_H
#define WEAKNET_KMOD_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#define WN_U64 __u64
#define WN_U32 __u32
#else
#include <stdint.h>
/*
 * 用户态不引入 <linux/types.h>：它可能与 <sys/stat.h> 间接引入的
 * asm-generic/int-ll64.h 产生 typedef 冲突。统一使用定宽类型。
 */
#define WN_U64 uint64_t
#define WN_U32 uint32_t
#endif

#define WEAKNET_KMOD_ABI_VERSION 2u
#define WEAKNET_KMOD_MAX_CPUS    256u
#define WEAKNET_KMOD_NR_SOFTIRQ  10u

/*
 * 快照头。seq 为 seqlock 代次号：
 *   奇数 = 内核正在写入，读者必须重试
 *   偶数 = 内容一致，可以采信
 *
 * 读者流程：
 *   判断 seq 为偶数 -> 拷贝整份快照 -> 再读一次 seq -> 两次相同才采信
 *
 * 内核侧的写入窗口被压缩到一次 memcpy（8 核约 700 字节），
 * 因此用户态重试概率极低。
 */
struct wn_kmod_hdr {
	WN_U64 seq;
	WN_U64 sample_ns;   /* 本次采集的 CLOCK_MONOTONIC 时间戳（纳秒） */
	WN_U32 abi_version;
	WN_U32 nr_cpus;     /* 有效记录条数，<= WEAKNET_KMOD_MAX_CPUS */
};

/*
 * 单 CPU 累计时间，单位是纳秒（不是 jiffies）。
 *
 * 语义与 /proc/stat 一致：user 已包含 guest，nice 已包含 guest_nice。
 * 计算使用率时应当为：
 *
 *   busy  = user + nice + system + irq + softirq + steal
 *   total = busy + idle + iowait
 *
 * 不要把 guest / guest_nice 再加进 busy，否则会重复计入。
 *
 * 【online 与 valid 的区别 —— 做区间差值前必须看 valid】
 *
 *   online  当前这一刻该 CPU 是否在线。
 *   valid   本记录能否与上一份快照做差值比较。
 *
 * CPU 下线时内核会在 tick_cancel_sched_timer() 里
 * memset(per_cpu(tick_cpu_sched, cpu), 0, ...)，把 idle/iowait 的累计
 * 清零；重新上线后这些计数从 0 重新累积。于是快照会出现
 * 「累计值突然变小」的跳变，直接相减会得到负数或荒唐的使用率。
 *
 * 因此：只要该 CPU 本轮与上一轮的 online 状态不同（下线或上线），
 * valid 就被置 0，表示这一对快照不可相减。用户态必须跳过，不能
 * 自行用 a > b 之类的判断去猜。
 */
struct wn_cpu_stat {
	WN_U32 cpu_id;
	WN_U32 online;
	WN_U32 valid;       /* 0 = 累计值发生过重置，本轮不可做差值 */
	WN_U32 _pad;        /* 显式补齐，保持后续 u64 的 8 字节对齐 */
	WN_U64 user;
	WN_U64 nice;
	WN_U64 system;
	WN_U64 idle;
	WN_U64 iowait;
	WN_U64 irq;
	WN_U64 softirq;
	WN_U64 steal;
	WN_U64 guest;
	WN_U64 guest_nice;
};

struct wn_cpu_snapshot {
	struct wn_kmod_hdr hdr;
	struct wn_cpu_stat cpu[WEAKNET_KMOD_MAX_CPUS];
};

/*
 * 单 CPU 软中断累计触发次数。cnt[] 索引与内核 enum 一致：
 *   0 HI, 1 TIMER, 2 NET_TX, 3 NET_RX, 4 BLOCK,
 *   5 IRQ_POLL, 6 TASKLET, 7 SCHED, 8 HRTIMER, 9 RCU
 */
struct wn_softirq_stat {
	WN_U32 cpu_id;
	WN_U32 online;
	WN_U64 cnt[WEAKNET_KMOD_NR_SOFTIRQ];
};

struct wn_softirq_snapshot {
	struct wn_kmod_hdr hdr;
	struct wn_softirq_stat cpu[WEAKNET_KMOD_MAX_CPUS];
};

#endif /* WEAKNET_KMOD_ABI_H */
