/*
 * 文件: src/bpf/bt_events.bpf.c
 * 功能: 蓝牙设备事件采集（Phase 1: 断连原因 / Canonical Device Event 的底层事实源）
 *
 * 与 a2dp_media.bpf.c 的区别：
 *   - a2dp_media 用 BPF_MAP 累计"流量统计"（周期性读走）
 *   - 本程序用 RINGBUF 上报"事件"（发生即上报，事件语义）
 *
 * 设计约束（与 wireless_event.hpp 的领域模型一一对应）：
 *   1. 本程序只产出 RawBtObservation（原始观测），**不产出业务事件**。
 *      同一现实故障被多个 hook 观测到时，由用户态 BtEventNormalizer 合并成
 *      一个 canonical 事件。在这里各 hook 自产事件会导致重复计数。
 *   2. 原始事实无损上报：reason code 原值、来源 hook 名、地址类型一并带上。
 *   3. 不做任何诊断推断（不填 suspected_cause）。
 *
 * ---- 挂点选择依据（2026-09-29 在目标板实测，内核 5.15.147-99btf1-a733）----
 *
 *   目标内核 **没有** bluetooth tracepoint：
 *     /sys/kernel/debug/tracing/events/bluetooth/ 不存在
 *   蓝牙是模块（CONFIG_BT=m），hci_conn/l2cap_chan 等类型不在 vmlinux BTF 中，
 *   因此 **不能用 CO-RE 读蓝牙结构体**，只能自定义最小结构 + probe_read。
 *
 *   函数签名由板端 /sys/kernel/btf/bluetooth（配合 base BTF）实锤：
 *     mgmt_device_disconnected(hdev, bdaddr, link_type, addr_type,
 *                              reason, mgmt_connected)          vlen=6
 *     hci_disconnect(conn, reason)                              vlen=2
 *     hci_conn_del(conn)                                        vlen=1
 *   结构偏移（BTF 实锤，非猜测）：
 *     hci_conn.dst    = 20   (bdaddr_t, 6 字节)
 *     hci_conn.type   = 55
 *     hci_conn.hdev   = 1400 (hci_dev*)
 *     hci_dev.id      = 64   (u16)
 *
 *   挂点优先级：mgmt_device_disconnected 是内核 mgmt 层通知用户态断连的统一
 *   出口（bluetoothd 消费的就是它），参数直接携带 reason，比在 HCI 包里反解
 *   稳得多，因此作为首选事实来源。
 *
 *   **注意 reason 的取值域**：mgmt 的 reason 是内核 MGMT_DEV_DISCONN_* 枚举
 *   （0=UNKNOWN,1=TIMEOUT,2=LOCAL_HOST,3=REMOTE,4=AUTH_FAILURE,5=SUSPEND），
 *   **不是** HCI error code —— 内核 hci_to_mgmt_reason() 已做过一次转换。
 *   用户态必须按 source 选择映射函数，见 wireless_event.hpp。
 *
 *   注意：本文件**不挂** hci_connect_acl / hci_connect_le —— "开始建链" 与
 *   "已经连上" 不是同一个产品事件，未经验证不得命名为 LinkConnected。
 *
 *   同理，**从未建立的连接不产出任何观测**：基于 hci_conn 的 hook 一律先读
 *   conn->handle（偏移 50），handle==0 表示从未收到 Connection Complete，
 *   直接丢弃——否则每次失败的连接尝试都会被 hci_conn_del 记成一条
 *   "LinkDisconnected"（板端实测连不上 5 次就刷了 5 条假断连）。
 *   失败的连接尝试属于 ConnectionAttempt 语义域，Phase 1 范围外。
 */

#define __TARGET_ARCH_arm64
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

// =============================================================================
// 常量
// =============================================================================

/// 单条观测最多携带的证据备注长度（保持 ringbuf 记录紧凑）。
/// 必须 >= 25：最长 hook 名 "mgmt_device_disconnected" 含结尾 NUL 是 25 字节。
/// 曾用 24 导致存进 DB 的 source_detail 被截断成 "mgmt_device_disconnecte"，
/// 追溯证据时按 hook 名匹配会失败。
#define BT_EVENT_DETAIL_MAX 32

/// 观测来源枚举 —— 必须与 wireless_event.hpp 的 EvidenceSource 保持一致
enum bt_evidence_source {
    SRC_UNKNOWN          = 0,
    SRC_KERNEL_MGMT      = 1,
    SRC_KERNEL_HCI       = 2,
    SRC_KERNEL_HCI_TIMEOUT = 3,
    SRC_BLUEZ_DBUS       = 4,
    SRC_DERIVED          = 5,
};

/// 事件类型枚举 —— 必须与 wireless_event.hpp 的 DeviceEventType 保持一致
enum bt_event_type {
    EVT_DEVICE_APPEARED    = 0,
    EVT_DEVICE_LOST        = 1,
    EVT_LINK_CONNECTED     = 2,
    EVT_LINK_DISCONNECTED  = 3,
    EVT_LINK_DEGRADED      = 4,
    EVT_LINK_RECOVERED     = 5,
    EVT_CONNECTION_ATTEMPT = 6,
};

// =============================================================================
// 上报结构（ringbuf 记录）
// =============================================================================

/*
 * 一条原始观测。字段顺序刻意与用户态 RawBtObservation 对齐，便于对照阅读。
 *
 * 注意 has_reason 标志：内核行为差异导致某些 hook 不携带 reason，
 * 此时 reason 字段无意义。用显式标志表达"未提供"而不是用 0 冒充
 * （0x00 是合法的 HCI status，用 0 冒充会与真实值混淆）。
 */
struct bt_observation {
    __u64 timestamp_ns;          // bpf_ktime_get_ns()
    __u32 hci_index;             // 来自 hdev->id
    __u8  bdaddr[6];             // BDADDR，6 字节
    __u8  addr_type;             // HCI 域 ADDR_LE_DEV_*（0=public,1=random,...）
    __u8  event_type;            // enum bt_event_type
    __u8  source;                // enum bt_evidence_source（决定 reason 的域）
    __u8  has_reason;            // 1 = reason 字段有效
    __u8  reason;                // 原始原因码，**域取决于 source**：
                                 //   mgmt 来源 = MGMT_DEV_DISCONN_* 枚举
                                 //   其余来源  = HCI error code
    __u8  link_type;             // 内核 link_type（ACL_LINK=0x01 / LE_LINK=0x80）
    __u8  source_detail[BT_EVENT_DETAIL_MAX];  // hook 名，如 "mgmt_device_disconnected"
};

// =============================================================================
// RingBuf
// =============================================================================

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 18);   // 256 KiB，事件稀疏，足够
} bt_events SEC(".maps");

/*
 * 全局开关。用户态在 BtEventMonitor::start() 成功后置 1；
 * 未启用时所有 hook 立即返回，避免无观测需求时的开销。
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} bt_events_cfg SEC(".maps");

// =============================================================================
// 辅助函数
// =============================================================================

static __always_inline int is_enabled(void)
{
    __u32 key = 0;
    __u32 *v = bpf_map_lookup_elem(&bt_events_cfg, &key);
    return v ? (int)*v : 0;
}

/*
 * 预留并填充一条观测记录。
 * 返回 NULL 表示 ringbuf 满（丢弃该条观测）——事件稀疏场景下可接受，
 * 用户态会从 trace 里看到丢包计数。
 */
static __always_inline struct bt_observation *reserve_observation(void)
{
    struct bt_observation *obs = bpf_ringbuf_reserve(&bt_events, sizeof(*obs), 0);
    if (!obs) return NULL;
    // ringbuf_reserve 返回未初始化内存，显式清零避免把内核栈数据带出去
    __builtin_memset(obs, 0, sizeof(*obs));
    obs->timestamp_ns = bpf_ktime_get_ns();
    return obs;
}

/// 记录 hook 名（截断到 BT_EVENT_DETAIL_MAX-1，保证 NUL 结尾）
static __always_inline void set_source_detail(struct bt_observation *obs, const char *name)
{
    // bpf_probe_read_kernel_str 在编译期需要常量长度，这里用固定长度读取
    bpf_probe_read_kernel_str(obs->source_detail, BT_EVENT_DETAIL_MAX, name);
}

/// 从 bdaddr_t*（6 字节）读取地址；失败返回 -1
static __always_inline int read_bdaddr(const void *ptr, __u8 out[6])
{
    if (!ptr) return -1;
    return bpf_probe_read_kernel(out, 6, ptr);
}

/// 从 hci_dev* 读取适配器序号；失败时保持 0
static __always_inline __u32 read_hci_index(const void *hdev)
{
    if (!hdev) return 0;
    __u16 id = 0;
    // hci_dev.id 偏移 64（板端 BTF 实锤）
    if (bpf_probe_read_kernel(&id, sizeof(id), (const char *)hdev + 64) < 0)
        return 0;
    return (__u32)id;
}

/*
 * 从 hci_conn* 读取 BDADDR、地址类型、链路类型与适配器序号。
 *
 * 地址类型必须连同链路类型一起读取：
 *   - `hci_conn.dst_type`（偏移 26）取值域是 **HCI 域 ADDR_LE_DEV_***
 *     （0=public, 1=random, 2=public_resolved, 3=random_resolved），
 *     **不是** mgmt API 的 BDADDR_*（BREDR=0/LE_PUBLIC=1/LE_RANDOM=2）。
 *     混淆会把 LE Random 错标成 LE Public（板端实测踩过）。
 *   - `hci_conn.type`（偏移 55）：ACL_LINK=0x01 / LE_LINK=0x80。
 *     不读它就无法区分 BR/EDR 与 LE —— ACL 连接的 dst_type 也常为 0，
 *     单看 dst_type 会误判。归类规则对照内核 mgmt.c 的 link_to_bdaddr()：
 *     type != LE_LINK 一律按 BR/EDR；LE_LINK 下只有 0x00 是 public，其余按 random。
 */
static __always_inline int read_conn_identity(const void *conn, __u8 out_bdaddr[6],
                                              __u8 *out_addr_type, __u8 *out_link_type,
                                              __u32 *out_hci_index, __u16 *out_handle)
{
    if (!conn) return -1;
    // hci_conn.handle 偏移 50（板端 BTF 实锤，u16）。
    // **只有链路真正建立（Connection Complete）后内核才会赋真实句柄**；
    // 连接尝试失败（page/create 超时、取消）的 conn 保持 kzalloc 初值 0。
    // 用它区分"真断连"与"从没连上"——否则每一次失败的连接尝试都会被
    // hci_conn_del 记成一条 LinkDisconnected（板端实测踩过：连不上 5 次
    // 就刷出 5 条"断连"事件）。Phase 1 只对已建立链路产出事件，
    // 失败的连接尝试属于 ConnectionAttempt 域，本 change 不产出。
    // （理论边界：句柄 0x0000 是合法句柄，但控制器实际分配几乎总是从
    //  非 0 开始；用它过滤掉海量失败尝试是净收益，见 docs 文档 11.x 记录。）
    *out_handle = 0;
    if (bpf_probe_read_kernel(out_handle, 2, (const char *)conn + 50) < 0)
        return -1;
    // hci_conn.dst 偏移 20（板端 BTF 实锤）
    if (bpf_probe_read_kernel(out_bdaddr, 6, (const char *)conn + 20) < 0)
        return -1;
    // hci_conn.dst_type 偏移 26（板端 BTF 实锤）
    // 取值域是 HCI 域 ADDR_LE_DEV_*（0=public,1=random,...），**不是** mgmt BDADDR_* 域
    if (bpf_probe_read_kernel(out_addr_type, 1, (const char *)conn + 26) < 0)
        return -1;
    // hci_conn.type 偏移 55（板端 BTF 实锤）：ACL_LINK=0x01 / LE_LINK=0x80。
    // 必须一并读：只看 addr_type 无法区分 BR/EDR 与 LE（ACL 连接该字段常为 0，
    // 会被误判成 LE public 或 BR/EDR）。mgmt 侧同样依赖 link_type 归类
    // （见内核 mgmt.c 的 link_to_bdaddr）。
    if (bpf_probe_read_kernel(out_link_type, 1, (const char *)conn + 55) < 0)
        return -1;
    // hci_conn.hdev 偏移 1400 -> hci_dev.id 偏移 64
    const void *hdev = NULL;
    if (bpf_probe_read_kernel(&hdev, sizeof(hdev), (const char *)conn + 1400) < 0)
        return -1;
    *out_hci_index = read_hci_index(hdev);
    return 0;
}

// =============================================================================
// 主挂点：mgmt_device_disconnected
//
// 内核 mgmt 层向用户态（bluetoothd）通报设备断连的统一出口。
// 参数由板端 BTF 实锤：6 个，第 5 个即 reason（最权威的事实来源）。
//
// ARM64 kprobe 参数寄存器映射（PT_REGS_PARM1..N = x0..x7）：
//   PARM1 = hdev, PARM2 = bdaddr, PARM3 = link_type,
//   PARM4 = addr_type, PARM5 = reason, PARM6 = mgmt_connected
// =============================================================================

SEC("kprobe/mgmt_device_disconnected")
int BPF_KPROBE(bt_mgmt_device_disconnected, void *hdev, const void *bdaddr,
               __u8 link_type, __u8 addr_type, __u8 reason, __u8 mgmt_connected)
{
    if (!is_enabled()) return 0;

    struct bt_observation *obs = reserve_observation();
    if (!obs) return 0;

    if (read_bdaddr(bdaddr, obs->bdaddr) < 0) {
        bpf_ringbuf_discard(obs, 0);
        return 0;
    }

    obs->hci_index = read_hci_index(hdev);
    obs->addr_type = addr_type;
    obs->link_type = link_type;
    obs->event_type = EVT_LINK_DISCONNECTED;
    obs->source = SRC_KERNEL_MGMT;
    obs->has_reason = 1;
    obs->reason = reason;
    set_source_detail(obs, "mgmt_device_disconnected");

    bpf_ringbuf_submit(obs, 0);
    return 0;
}

// =============================================================================
// 补充证据 1：hci_disconnect(conn, reason)
//
// 本机主动发起断连。与 mgmt 挂点的区别：mgmt 是"最终通知"，本挂点是"发起点"。
// 两条都会进 raw_evidence；canonical reason 仍由 mgmt 的原始 code 定案
// （见 BtEventNormalizer 的原因定案优先级）。
// =============================================================================

SEC("kprobe/hci_disconnect")
int BPF_KPROBE(bt_hci_disconnect, const void *conn, __u8 reason)
{
    if (!is_enabled()) return 0;

    struct bt_observation *obs = reserve_observation();
    if (!obs) return 0;

    __u32 hci_index = 0;
    __u16 handle = 0;
    if (read_conn_identity(conn, obs->bdaddr, &obs->addr_type, &obs->link_type,
                           &hci_index, &handle) < 0 || handle == 0) {
        // handle==0：链路从未建立，不是"断连"，Phase 1 不产出业务观测
        bpf_ringbuf_discard(obs, 0);
        return 0;
    }

    obs->hci_index = hci_index;
    obs->event_type = EVT_LINK_DISCONNECTED;
    obs->source = SRC_KERNEL_HCI;
    obs->has_reason = 1;
    obs->reason = reason;
    set_source_detail(obs, "hci_disconnect");

    bpf_ringbuf_submit(obs, 0);
    return 0;
}

// =============================================================================
// 补充证据 2：hci_conn_timeout(work)
//
// 链路超时的**根因**观测。注意：本 hook 不携带 reason code，
// 因此 has_reason=0，用户态据来源推断原因为 ConnectionTimeout。
//
// 它不产出独立的业务事件 —— 它是 LinkDisconnected 的一条证据。
// 如果在 2 秒合并窗口内 mgmt 也报了同一次断连，两者归入同一个事件。
//
// **参数是 work_struct*，不是 hci_conn***（2026-09-29 板端实测踩坑）：
//   内核源码 static void hci_conn_timeout(struct work_struct *work)
//   这是 delayed_work 回调，必须 container_of 拿到宿主 hci_conn。
//   板端 BTF 实锤：
//     hci_conn.disc_work (delayed_work) 偏移 = 1984 bit = 248 字节
//     delayed_work.work  (work_struct)  偏移 = 0
//   => conn = work - 248
//   直接把 work 当 conn 读偏移 20 会拿到垃圾地址（实测得到 7E:A0:FF:FF:00:00）。
// =============================================================================

/// hci_conn.disc_work 在 hci_conn 中的字节偏移（板端 BTF 实锤：1984 bit）
#define HCI_CONN_DISC_WORK_OFFSET 248

SEC("kprobe/hci_conn_timeout")
int BPF_KPROBE(bt_hci_conn_timeout, const void *work)
{
    if (!is_enabled()) return 0;

    // container_of(work, struct hci_conn, disc_work)
    const void *conn = (const char *)work - HCI_CONN_DISC_WORK_OFFSET;

    struct bt_observation *obs = reserve_observation();
    if (!obs) return 0;

    __u32 hci_index = 0;
    __u16 handle = 0;
    if (read_conn_identity(conn, obs->bdaddr, &obs->addr_type, &obs->link_type,
                           &hci_index, &handle) < 0 || handle == 0) {
        // handle==0：链路从未建立，不是"断连"，Phase 1 不产出业务观测
        bpf_ringbuf_discard(obs, 0);
        return 0;
    }

    obs->hci_index = hci_index;
    obs->event_type = EVT_LINK_DISCONNECTED;
    obs->source = SRC_KERNEL_HCI_TIMEOUT;
    obs->has_reason = 0;   // 本 hook 不携带 reason，不得用 0 冒充
    set_source_detail(obs, "hci_conn_timeout");

    bpf_ringbuf_submit(obs, 0);
    return 0;
}

// =============================================================================
// 补充证据 3：hci_conn_del(conn)
//
// 连接对象销毁，兜底观测。几乎所有异常路径最终都会走到这里，
// 因此它是"确实发生了断连"的有力证据，但本身不携带原因。
// =============================================================================

SEC("kprobe/hci_conn_del")
int BPF_KPROBE(bt_hci_conn_del, const void *conn)
{
    if (!is_enabled()) return 0;

    struct bt_observation *obs = reserve_observation();
    if (!obs) return 0;

    __u32 hci_index = 0;
    __u16 handle = 0;
    if (read_conn_identity(conn, obs->bdaddr, &obs->addr_type, &obs->link_type,
                           &hci_index, &handle) < 0 || handle == 0) {
        // handle==0：链路从未建立，不是"断连"，Phase 1 不产出业务观测
        bpf_ringbuf_discard(obs, 0);
        return 0;
    }

    obs->hci_index = hci_index;
    obs->event_type = EVT_LINK_DISCONNECTED;
    obs->source = SRC_KERNEL_HCI;
    obs->has_reason = 0;
    set_source_detail(obs, "hci_conn_del");

    bpf_ringbuf_submit(obs, 0);
    return 0;
}
