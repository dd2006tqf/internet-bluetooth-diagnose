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
