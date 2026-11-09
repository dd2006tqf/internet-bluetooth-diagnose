/**
 * @file v4l2_frame_source.hpp
 * @brief V4L2 真机帧源（视频 QoE 的 L2 层实现之一）
 *
 * 与 `SyntheticFrameSource` 实现同一个 `IFrameSource` 接口，上层代码不需要
 * 知道帧从哪来。这是 §6.2.1 分层的全部意义：L3/L4 在无硬件时已测透，
 * 真机源到位后只是换一个实现。
 *
 * ============================================================================
 * 为什么把 V4L2 细节全部藏在 pimpl 后面
 * ============================================================================
 *
 * `linux/videodev2.h` 是个大而杂的内核头（超 5000 行，且版本间有差异）。
 * 若把它 include 进公共头，任何包含本文件的编译单元都会被拖慢，且 ARM64
 * sysroot 与宿主机头文件版本差异会直接变成编译错误。
 *
 * 所以本头**只暴露**：构造参数、`IFrameSource` 的五个方法、几个可观测的
 * 计数器。所有 `ioctl` / `mmap` / `struct v4l2_*` 都留在 .cpp 里。
 *
 * ============================================================================
 * 语义边界（与合成源逐条同构，不得越过）
 * ============================================================================
 *
 * - `start()` 失败**不是**错误状态，是「这个能力不可用」。上层报 unavailable，
 *   业务链路照常运行。这与 `IFrameSource::start()` 的契约一致。
 * - 设备不存在时构造仍然成功（不在构造函数里碰 /dev/video*），只有 `start()`
 *   才尝试打开。这样单测可以在没有摄像头的 CI 上构造对象并验证失败路径。
 * - `snapshotAndReset()` 的 `frames_expected` **必须**由真实帧率推导，不能
 *   拿 `frames_received` 回填——那会让丢帧率恒为 0，把「全丢」显示成「完美」。
 *
 * ============================================================================
 * 单测可行性
 * ============================================================================
 *
 * 真机路径无法在 CI 验证，但**失败路径**可以：把一个不存在的路径传进来，
 * `start()` 必须返回 false 且 `snapshotAndReset()` 返回 `valid()==false`。
 * 这是「缺数据不得伪装成健康」纪律在最底层的一次守护。
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "frame_source.hpp"

namespace weaknet {

/**
 * @brief V4L2 采集参数
 *
 * 默认值来自方案 §6.2.2.2 的取舍表，每一项都有实测/设计依据：
 *   - MMAP + QBUF/DQBUF：零拷贝，最少一次搬移
 *   - 4 个 buffer：vb2 的 dma-contig 单次分配不宜过大
 *   - select() 超时计入 max_gap_ms：检测卡顿的关键
 *   - 用 v4l2_buffer.timestamp：内核单调时钟，避免调度延迟污染帧间隔
 */
struct V4l2CaptureConfig {
    /// 设备路径。默认对应 8M 219（IMX219）在 sunxi-vin 上的节点
    ///
    /// 注意：真机上不一定是 `/dev/video0`——vin 会为每个 DMA 通道注册节点，
    /// 通常 ISP 主通路在 video0，但 RAW/DMA 通道可能占用更小编号。
    /// `describe()` 会输出实际打开的路径供证据溯源。
    std::string device{"/dev/video0"};

    /// 期望分辨率。默认 1280x720 而非 sensor 满分辨率 3280x2464
    ///
    /// 理由是「要的是帧统计，不是画质」：满分辨率会让 ISP/内存带宽成为瓶颈，
    /// 把「ISP 跟不上」与「网络丢了帧」混在一起，污染归因。720p@30 足以
    /// 制造可观测丢帧，且留出充足余量。
    uint32_t width{1280};
    uint32_t height{720};

    /// 期望帧率。用于推导 frames_expected
    uint32_t fps{30};

    /// buffer 数量
    uint32_t buffer_count{4};

    /// select() 超时（毫秒）。超时本身不是错误，但要计入 max_gap_ms
    uint32_t select_timeout_ms{2000};

    /// 是否尝试把驱动帧率设成 `fps`（`VIDIOC_S_PARM`）
    ///
    /// 默认 true，且**必须**默认 true：`frames_expected` 是按 `fps` 推的，
    /// 若驱动实际跑在别的帧率上，丢帧率会算成一个恒定的巨大假值。
    ///
    /// 实测证据（vivid 虚拟设备，另起进程把驱动设成 15fps，本进程配置 30fps
    /// 且 `set_frame_rate=false`）：
    ///   expected=30 received=15 lost=15 loss_ratio=0.5000  ← 纯属配置没生效
    /// 注意驱动帧率状态**跨进程保留**，所以这个坑很容易在「上一步设过帧率」
    /// 之后突然出现。真机同理：IMX219 的默认帧率不一定是 30。不设就是算错。
    ///
    /// 设为 false 只应在「驱动不支持 S_PARM 且已知实际帧率」时使用，
    /// 此时须自己保证 `fps` 与驱动实际一致。
    bool set_frame_rate{true};

    /// 是否允许改用多平面 API（`V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`）
    ///
    /// 默认 true：驱动声明 MPLANE 能力时优先用它。原因是嵌入式 ISP 的 DMA
    /// 通道常按平面分离（Y 一个通道、UV 一个通道），单平面要靠驱动额外拼装，
    /// 部分 sunxi-vin 配置只出 NV12M 而不出 NV12。
    ///
    /// 探测顺序：先看 `cap.capabilities` 与 `cap.device_caps` 是否含
    /// `V4L2_CAP_VIDEO_CAPTURE_MPLANE`，有则用 MPLANE，否则回落到单平面。
    /// 两条路径产出的 `FrameStats` 语义完全一致（见单测）。
    ///
    /// 设为 false 是「驱动声称支持 MPLANE 但实际有问题」时的兜底。
    bool allow_multiplanar{true};

    /// buffer 内存来源：`V4L2_MEMORY_MMAP`（默认）还是 `V4L2_MEMORY_DMABUF`
    ///
    /// ========================================================================
    /// 先说清收益边界：本项目**不需要** DMABUF
    /// ========================================================================
    ///
    /// DMABUF 的用途是**把同一块像素内存零拷贝移交给另一个设备**（编码器 /
    /// 显示器 / 另一个 V4L2 节点）。本帧源只消费 `v4l2_buffer` 里的**元数据**
    /// （bytesused / sequence / timestamp），从不读取像素，因此：
    ///
    /// - MMAP 路径下那次 mmap 是**无收益的**：映射出来的内存从头到尾没被访问，
    ///   只在 closeDevice() 里被 munmap。它甚至不是必需的——实测 vivid 与
    ///   真机 vb2 都允许未 mmap 直接 QBUF（但这依赖驱动实现，**不能依赖**）。
    /// - 换成 DMABUF 也只省掉这一次映射，**不会减少任何像素拷贝**，因为
    ///   本来就没有拷贝 —— 所有统计都来自内核写好的 buf 结构体。
    ///
    /// 所以这个选项默认 **false**。它存在的唯一理由是：将来若要把帧交给
    /// 编码器/推流进程，DMABUF 是正确做法，届时预留这个开关比那时再改
    /// 内存模型成本低。**在那之前它是无消费者的可选路径，不是默认路径。**
    ///
    /// ========================================================================
    /// 两条 DMABUF 子路径：实测都能跑通，本实现**优先导入**
    /// ========================================================================
    ///
    /// 1. **导入**（`/dev/dma_heap/*` 分配后入队）：我们自己分配内存，把 fd
    ///    交给驱动填充。这是本实现的**首选**，因为它才是 DMABUF 唯一真正的
    ///    收益来源——内存由调用方掌控（位置、复用池、跨进程共享）。
    ///    代价是依赖 `/dev/dma_heap`，板上不一定有。
    /// 2. **导出**（`VIDIOC_EXPBUF`）：先用 MMAP 申请并从驱动侧导出 dma-buf fd，
    ///    再以 DMABUF 模式重新入队同一 fd。这是导入不可用时的**回落**。
    ///
    /// ========================================================================
    /// 为什么优先导入而不是导出（一处必须说清的取舍）
    /// ========================================================================
    ///
    /// 导出路径对本帧源而言**收益为零**：它要求先按 MMAP 申请（驱动分配），
    /// 导出 fd 后仍要拿回同一个 fd 自己消费，全程没有第二方接收这个 fd。
    /// 净效果是「和 MMAP 完全一样 + 多占 N 个 fd + 多走一趟 REQBUFS」。
    ///
    /// 导出路径有意义的场景是**把 fd 交给别人**（编码器 / 另一个 V4L2 节点），
    /// 而那正是本帧源当前不需要的事。保留它只作为导入不可用时的兜底：
    /// 至少能证明 DMABUF 通路本身是通的，而不是静默退回 MMAP。
    ///
    /// ========================================================================
    /// 实测依据（vivid，640x480 NV12，4 buffer）
    /// ========================================================================
    ///
    ///   导入：REQBUFS(DMABUF) → dma_heap 分配 4 个 460800B → QBUF 全成功
    ///         → STREAMON → DQBUF 5 帧，`bytesused=460800` 与 `timestamp`
    ///         均正常递增。
    ///   导出：REQBUFS(MMAP) → EXPBUF 4 个 fd → REQBUFS(0) 释放队列
    ///         → 导出 fd **仍可 mmap** → REQBUFS(DMABUF) → QBUF 成功 → 出帧。
    ///
    /// 第二项（`REQBUFS(0)` 后导出 fd 仍有效）依赖 vb2 的 `vb2_dc_put`
    /// 挂 release callback 等 dma-buf detach 才释放底层内存这一实现细节。
    /// 它在 6.8 内核上实测成立，但**不是 ABI 保证**，所以导入路径优先。
    ///
    /// 两条路径产出的 `FrameStats` 语义与 MMAP 完全一致：统计只来自
    /// `v4l2_buffer` 的元数据，与缓冲区如何分配无关（见上面的收益边界）。
    bool use_dmabuf{false};

    /// 是否在 start() 时解析 media controller 拓扑并记入 describe()
    ///
    /// ========================================================================
    /// 为什么需要它（真机上的实际问题）
    /// ========================================================================
    ///
    /// sunxi-vin 会为每个 DMA 通道注册独立的 video 节点。板上光看 `/dev/video*`
    /// 无法判断：哪个节点对应 ISP 主通路、哪个是 RAW 通道、摄像头是否真的
    /// 接在某条链路末端、链路有没有被 `media-ctl` 正确 enable。
    ///
    /// 这些信息只存在于 media controller 拓扑里（`/dev/media*` +
    /// `MEDIA_IOC_ENUM_ENTITIES` / `MEDIA_IOC_ENUM_LINKS`）。没有它，真机
    /// 阶段只能靠反复试 `/dev/video0..N` 猜哪个能出帧，而「打不开」和
    /// 「打开了但链路没通」在 V4L2 层报的是同一个 EINVAL。
    ///
    /// ========================================================================
    /// 只读，不改拓扑
    /// ========================================================================
    ///
    /// 本实现**只读**拓扑，绝不调用 `MEDIA_IOC_SETUP_LINK`。链路配置属于
    /// 部署动作（`media-ctl -l`），由运维在 overlay 演练阶段完成。一个采集
    /// 库在 start() 里偷偷改链路方向会让现场完全无法复现问题。
    ///
    /// 解析失败（无 /dev/media*、无权限、驱动不支持）**不是错误**：
    /// 拓扑信息是诊断增强，缺失时 describe() 少一段即可，start() 照常继续。
    /// 把「拿不到诊断信息」升级成「采集失败」是本末倒置。
    bool probe_media_topology{true};

    /// 单次 snapshotAndReset() 的处理时间上限（毫秒）
    ///
    /// 不是「采集多久」，而是「最多花多久把队列里已到的帧全部取走」。
    /// 取完后窗口时长按实际耗时记入 FrameStats.window_ms，与网络侧对齐。
    uint32_t window_ms{1000};
};

/**
 * @brief V4L2 真机帧源
 *
 * 典型用法：
 *
 *   V4l2FrameSource src;                 // 构造不碰设备
 *   if (!src.start()) {  // 报 unavailable，业务照常
 *   }
 *   auto stats = src.snapshotAndReset(); // 每秒一次
 *   src.stop();
 */
class V4l2FrameSource : public IFrameSource {
public:
    explicit V4l2FrameSource(V4l2CaptureConfig config = V4l2CaptureConfig{});

    /// 析构必须在 .cpp 中定义（pimpl：此处 Impl 尚不完整）
    ~V4l2FrameSource() override;

    /// 不可拷贝：持有 fd 与 mmap 映射，复制会导致 double-close
    V4l2FrameSource(const V4l2FrameSource&) = delete;
    V4l2FrameSource& operator=(const V4l2FrameSource&) = delete;

    bool start() override;
    void stop() override;
    FrameStats snapshotAndReset() override;
    std::string describe() const override;

    /// 最近一次失败的原因（人类可读）。未失败时为空
    ///
    /// 为什么要单独暴露而不是塞进 FrameStats：`FrameStats` 是**纯数据事实**，
    /// 不含任何诊断结论（见 frame_source.hpp 的设计纪律）。失败原因是运维
    /// 信息，属于实现侧，不该污染数据契约。
    const std::string& lastError() const;

    /// 是否已成功 start()（stop() 之后回到 false）
    bool isRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace weaknet
