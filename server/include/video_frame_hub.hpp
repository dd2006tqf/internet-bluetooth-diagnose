/**
 * @file video_frame_hub.hpp
 * @brief 帧中枢：把采集到的像素以**句柄**形式交给别的进程（面板 / 编码器）
 *
 * ============================================================================
 * 为什么需要这一层，而不是让消费者直接打开摄像头
 * ============================================================================
 *
 * 本板实测（2026-11-13，`/dev/video0`）：
 *
 *   A 仅 open（零 ioctl）→ OK
 *   B 紧随其后 open     → EBUSY
 *   只读 open 也是独占   → EBUSY
 *   video1/4/5 可同时打开 → 说明是**按节点**独占，不是全局
 *
 * 也就是说独占发生在 `open()`，不是「硬件 CSI 链路独占」。
 * 结论同样硬：**面板不能自己开 /dev/video0**。摄像头必须只有一个所有者，
 * 由它把画面分发给所有消费者。
 *
 * ============================================================================
 * 为什么不直接把 V4L2 capture buffer 的 dma-buf fd 交出去
 * ============================================================================
 *
 * 那是零拷贝的最短路径，但有一个致命副作用：**消费者持有越久，采集越卡**。
 * 采集线程 DQBUF 之后必须立刻 QBUF 归还，否则 4 个 buffer 很快耗尽，
 * 驱动开始丢帧——而那会被本项目的 `FrameStats` 记成「链路丢了帧」，
 * 运维会去查一条完好的线。
 *
 * 本项目已经反复踩过同一个坑（见 `frame_source.hpp` 里
 * `snapshot_interval_ms` 与 `observationGapMs()` 的整套注释）：**观测缺陷
 * 绝不能栽给被观测对象**。所以这里选择：
 *
 *   - 消费者拿到的是一份**我们自己的快照缓冲**（一次 memcpy）；
 *   - 采集 buffer 立刻归还，采集节奏完全不受是否有人在看影响；
 *   - 代价是每交付一帧一次拷贝，但交付是被**限速**的（默认 10fps），
 *     而采集是 18fps——只拷贝真正要给人看的那几帧。
 *
 * 换句话说：为了「帧统计的真实性」牺牲一次拷贝。这个取舍与本项目其它
 * 所有取舍同向。
 *
 * ============================================================================
 * 租约协议（必须遵守，否则内存会被覆写）
 * ============================================================================
 *
 *   acquire() → 拿到 fd + slot 号 → 消费 → release(slot) → close(fd)
 *
 * - `fd` 与 `slot` 的生命周期**互相独立**：fd 是 dma-buf 句柄（可以被 dup、
 *   可以在别的进程里 mmap），slot 是「这块内存现在归你」的标记。
 * - 只要 slot 没被 release，中枢就**不会**往里写新帧，哪怕一直跳过发布。
 *   跳过是可观测的（`stats().skipped_busy`），而不是静默覆写。
 * - 消费者崩溃没 release：超过 `lease_timeout_ms` 强制回收并计入
 *   `stats().reclaimed`。宁可记一条异常，也不能让发布永久停摆。
 *
 * ============================================================================
 * 为什么它不依赖 V4L2
 * ============================================================================
 *
 * 本文件不 include 任何 `<linux/videodev2.h>`，输入只是「一块内存 + 几何」。
 * 于是整条租约/限速/回收逻辑可以在**没有摄像头**的机器上单测：
 * 灌假数据、验证非零、验证覆写保护、验证超时回收。
 * 摄像头只在最后一步把指针接进来。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace weaknet {

/// 一帧的交付句柄
struct VideoFrameLease {
    /// dma-buf（或 memfd）句柄。调用方负责 `close()`
    ///
    /// 拿到后可以直接 `mmap(size, PROT_READ, MAP_SHARED, fd, 0)`，
    /// 也可以再通过 SCM_RIGHTS 转给另一个进程。
    int fd{-1};

    uint32_t width{0};
    uint32_t height{0};

    /// Y 平面每行字节数。**不等于** width：驱动可能按 16/32 字节对齐。
    /// 消费方若用 width 当 stride 会得到逐行斜切的画面。
    uint32_t stride{0};

    /// UV 平面相对帧起始的**字节偏移**。
    ///
    /// 为什么不能有个「UV 紧跟 stride*height」的隐含约定：本板 sunxi-vin
    /// 实测（2026-11-14，1640x1232 NV12）真实布局是
    ///
    /// ```
    /// [0,        2020480)  Y   stride=1640 × 1232
    /// [2020480,  2030336)  对齐空洞 9856 字节（全零）
    /// [2030336,  3040576)  UV  stride=1640 × 616
    /// [3040576,  3045504)  尾部空洞 4928 字节（全零）
    /// ```
    ///
    /// 即 UV 偏移 = 2030336 ≠ stride*height = 2020480。差的那 9856 字节
    /// 由驱动的 `ALIGN(w,16)*ALIGN(h,16)` 造成。消费方按 stride*height 定位
    /// UV 会落进全零空洞（U=V=0），画面明显偏色。
    ///
    /// 0 表示「等于 stride*height」（标准紧凑 NV12）——这样不关心布局的
    /// 调用方不必填它，而生产者只要知道真实偏移就填。
    uint32_t uv_offset{0};

    /// V4L2 fourcc（如 `V4L2_PIX_FMT_NV12`）。用整数而非枚举，避免本头
    /// 拖入内核头；消费方按需对照。
    uint32_t fourcc{0};

    uint64_t sequence{0};
    uint64_t timestamp_ns{0};
    size_t size{0};

    /// 归还时必须原样传回 `release()`
    uint32_t slot{UINT32_MAX};

    bool valid() const { return fd >= 0 && size > 0; }
};

struct VideoFrameHubConfig {
    /// 快照槽位数量。3 个足够「一帧在交付、一帧刚发布、一帧在写」
    uint32_t slot_count{3};

    /// 两帧发布之间的最小间隔（毫秒）。默认 100 = 上限 10fps
    ///
    /// 为什么限速：消费者（面板 10fps 刷新、编码器 15~25fps）都不需要
    /// 采集端的满帧率。不限速就是白拷 3MB × 18 = 54 MB/s。
    uint32_t min_publish_interval_ms{100};

    /// 租约超时（毫秒）。消费者崩溃没释放时强制回收
    uint32_t lease_timeout_ms{2000};

    /// 优先用 `/dev/dma_heap/system` 分配（真 dma-buf，可交给编码器/显示器）
    ///
    /// 失败自动回落 `memfd_create`：跨进程 mmap 同样可行，但**不是 dma-buf**，
    /// 无法零拷贝交给硬件编码器。这个差别会写进 `describe()`，不静默。
    bool use_dma_heap{true};
};

/**
 * @brief 帧中枢：限速发布 + 租约式交付
 *
 * 线程模型：`publish()` 由采集线程调用，`acquire()`/`release()` 由任意
 * D-Bus 派发线程调用。内部有一把锁，临界区只做指针与计数操作，
 * **memcpy 在锁外**（否则 3MB 拷贝会让 acquire 阻塞）。
 */
class VideoFrameHub {
public:
    explicit VideoFrameHub(VideoFrameHubConfig config = VideoFrameHubConfig{});
    ~VideoFrameHub();

    VideoFrameHub(const VideoFrameHub&) = delete;
    VideoFrameHub& operator=(const VideoFrameHub&) = delete;

    /**
     * @brief 设定几何并按需（重新）分配槽位
     *
     * 分辨率/格式变化时调用：会释放旧槽位并重新分配。**已发出的租约会被
     * 视为失效**（旧内存被释放），所以调用方应只在实际换格式时调它。
     *
     * @return true 至少有一个槽位可用；false 全部分配失败（能力不可用）
     *
     * @param uv_offset UV 平面相对帧起始的字节偏移；0 表示「等于
     *                  `stride * height`」。取 `V4l2FrameSource::PixelFrame`
     *                  的同名字段原样传入，**不要自己去推导**——那个偏移由
     *                  驱动的对齐规则决定，见 `VideoFrameLease::uv_offset`。
     */
    bool configure(uint32_t width, uint32_t height, uint32_t stride,
                   uint32_t fourcc, size_t size, uint32_t uv_offset = 0);

    /**
     * @brief 发布一帧（由采集线程调用）
     *
     * 内部限速：距上次成功发布不足 `min_publish_interval_ms` 时直接返回
     * false 并计入 `skipped_rate`——这是**正常**的，不是错误。
     *
     * @param src      源像素指针（V4L2 MMAP 出来的地址）
     * @param src_size 源实际字节数（取 `min(src_size, size)` 拷贝）
     * @return true 本次真的写了一个槽位
     */
    bool publish(const void* src, size_t src_size,
                 uint64_t sequence, uint64_t timestamp_ns);

    /**
     * @brief 取最新一帧的句柄
     *
     * 返回的 fd 是新 dup 出来的，调用方 close 它；slot 号必须传给 `release()`。
     * 没有可交付帧（未 configure / 从未 publish）时返回 false。
     */
    bool acquire(VideoFrameLease& out);

    /// 归还槽位。重复归还或未知 slot 是安全的（幂等，不报错）
    void release(uint32_t slot);

    /// 实际使用的分配方式：`dma-heap` / `memfd` / `none`
    std::string backend() const;

    /// 人类可读状态（写进 evidence，说明这条通路到底有没有真的在工作）
    std::string describe() const;

    struct Stats {
        uint64_t published{0};        ///< 成功写入槽位的帧数
        uint64_t skipped_rate{0};     ///< 因限速跳过
        uint64_t skipped_busy{0};     ///< 因所有槽位都被租用而跳过
        uint64_t skipped_unconfigured{0}; ///< configure 之前/失败后的发布
        uint64_t acquired{0};         ///< 被取走的次数
        uint64_t released{0};         ///< 归还次数
        uint64_t reclaimed{0};        ///< 超时强制回收次数（消费者异常）
        uint64_t truncated{0};        ///< src_size < size（源比预期短）
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace weaknet
