/**
 * @file video_encoder.hpp
 * @brief 视频编码器抽象：把 VideoFrameHub 的帧转成 H.264 码流
 *
 * ============================================================================
 * 它在链路里的位置
 * ============================================================================
 *
 *   V4l2FrameSource ──▶ VideoFrameHub ──┬──▶ Qt 面板（UNIX_FD，渲染）
 *   （唯一所有者）       （租约交付）    └──▶ **VideoEncoder**（本文件）──▶ 推流
 *
 * 编码器是帧中枢的**第二个消费者**。两个消费者拿的都是同一条 `acquire()` /
 * `release()` 通路，中枢不知道也不关心谁是谁。
 *
 * ============================================================================
 * 为什么接口只吃「内存 + 几何」，不认识 V4L2 也不认识 dma-buf
 * ============================================================================
 *
 * 与 `VideoFrameHub` 同一个理由：**不 include 任何内核头**，于是整条编码逻辑
 * 可以在**没有摄像头、没有板端硬件**的机器上单测（灌假帧、验码流、验计数）。
 * 后端差异（本板 cedar VE / 通用软件编码）由实现类吸收，调用方看不见。
 *
 * 唯一的例外是 `VideoFrameView::dmabuf_fd`：它是 `int`，不是 `#include
 * <linux/dma-buf.h>`。传 -1 就退化成纯内存路径，不需要任何内核头。
 *
 * ============================================================================
 * 本板实测（2026-11-14，Radxa Cubie A7A，sunxi-vin + cedar VE）
 * ============================================================================
 *
 * 硬件编码器**可用且远快于需要**：
 *
 * | 输入 | 帧数 | 耗时 | 吞吐 |
 * | --- | --- | --- | --- |
 * | `demoVencoder` 1920x1080 NV12 | 30 | 0.37s | ~81 fps |
 * | `demoVencoder` 1640x1232 NV12（有内容） | 60 | 0.686s | ~87 fps |
 *
 * 采集侧真实 18.5fps → **4.7 倍余量**，且 CPU 侧几乎为零（`sys` 0.21s / 60 帧，
 * 编码在 VE 硬件里做）。SPS 实测声明 `1640x1232`（coded 1648 + crop right=4，
 * 非 16 对齐宽度的处理完全正确），Annex-B，NAL 直方图
 * `{1:29, 7:1, 8:1, 5:1}` 与输入帧数严格对齐。
 *
 * 因此本接口的**默认策略是硬件优先**，软件仅作无 VE 环境的后备。
 *
 * ============================================================================
 * 零拷贝是可能的，而且不是巧合（实测的字节账，务必别改坏）
 * ============================================================================
 *
 * 真实 V4L2 帧（1640x1232 NV12，`sizeimage=3045504`）的实际布局：
 *
 *   Y   [0,        2030336)   1648 行 × 1232      ← **不是** 1640×1232=2020480
 *   空洞 [2030336, 2030336+9856)                   ← 对齐填充，全零
 *   UV  [2030336, 3045504)   1648 列 × 616 行
 *
 * 而编码器 `AllocInputBuffer` 要的正是 `sizeY=2030336 sizeC=1015168`：
 *
 *   2030336 = 1648 × 1232   （coded 宽 16 对齐后的 Y 平面）
 *   1015168 = 2030336 / 2
 *   2030336 + 1015168 = 3045504 = 真实 sizeimage  ← 严丝合缝
 *
 * 也就是说 Y 平面「就地」满足对齐要求，UV 起点正好落在编码器期望的位置。
 * 于是 `nShareBufFd` 导入能**零拷贝**吃掉整块帧，不需要中间缓冲。
 * 实测两条路径产出同级码流（首帧 681989 B vs 680097 B），
 * 但零拷贝省掉每帧 3.0 MB 的 memcpy（18.5fps 下约 56 MB/s）。
 *
 * 这个吻合依赖 `uv_offset` 是**驱动回报的事实值**。任何用 `width*height`
 * 推算 UV 起点的写法都会在此处错位 9856 字节——整帧偏色且不报错。
 *
 * ============================================================================
 * 统计口径（与 FrameStats 逐条同构，不是巧合）
 * ============================================================================
 *
 * 编码统计要和采集统计、网络指标做同窗对比，否则归因必然误判
 * （见 `video_qoe_attribution.hpp` 头注释里那条「窗口必须对齐」）。
 * 所以这里同样有：
 *
 *   - `window_ms`：这段统计**事实**覆盖了多久（不是配置值）；
 *   - `valid`：没有有效输入时**必须为 false**，全零统计会被上游读成「零丢帧」。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace weaknet {

/// 一份编码统计快照
///
/// 语义与 `FrameStats` 逐条对应，便于 `PushQoeAttribution` 做同窗比较。
struct EncoderStats {
    uint64_t frames_in{0};    ///< 送进编码器的帧
    uint64_t frames_out{0};   ///< 吐出的码流帧（有延迟，稳态下 == frames_in - delay）
    uint64_t bytes_out{0};    ///< 码流字节数
    uint64_t keyframes{0};    ///< IDR 数量

    /// 因「队列满」被**主动丢弃**的帧
    ///
    /// 与「编码失败」必须分开计：前者是我们自己选择降级（丢帧保实时），
    /// 后者是编码器坏了。归因结论完全不同，混在一起就再也分不开。
    uint64_t dropped{0};

    /// 编码失败次数（后端返回错误）
    uint64_t failed{0};

    /// 单帧编码耗时（毫秒）：从送进来到码流可取走
    ///
    /// 硬件编码是异步的（送进去立刻返回、稍后取码流），所以这个值衡量的是
    /// 「排队 + 硬件处理」的总延迟，不是 CPU 时间。这是**用户真正感受到的**
    /// 那个量。
    uint32_t encode_ms_p50{0};
    uint32_t encode_ms_p95{0};

    /// 当前待编码帧数（送进去但码流还没取走的）
    ///
    /// **这是区分「编码器跟不上」与「网络差」的关键判据**：
    /// 编码队列涨 + 发送队列空 = 编码器瓶颈；两者都涨 = 网络瓶颈。
    uint32_t queue_depth{0};

    /// 本统计窗口的**事实**时长（毫秒）。0 表示未声明，归因层会拒绝归因。
    uint32_t window_ms{0};

    /// 是否真的编过帧。false 时其余字段无意义（不是「全零的健康」）。
    bool valid{false};

    /// 实际码率（kbps），由 bytes_out / window_ms 推导
    ///
    /// 放在这里而不是让调用方自己算：窗口口径必须与 bytes_out 同源，
    /// 两个地方各算一次迟早会漂移。
    uint32_t bitrate_kbps() const {
        if (window_ms == 0) return 0;
        return static_cast<uint32_t>(
            (bytes_out * 8ULL) / static_cast<uint64_t>(window_ms));
    }
};

/// 一块码流数据
///
/// **生命周期只在回调/返回语句内**：实现可以把内部缓冲直接指过来，避免每次
/// 编码都拷贝一份。需要留存就必须自己拷走。
struct EncodedFrame {
    const uint8_t* data{nullptr};
    size_t size{0};

    /// 是否关键帧（IDR）。分片封装时需要在此切 GOP。
    bool keyframe{false};

    /// 与输入帧对应的序号（原样透传 `VideoFrameView::sequence`）
    ///
    /// 让下游能把「哪一帧编出来的码流」与「采集侧的那一帧」对上。没有它，
    /// 端到端延迟只能靠时间戳猜。
    uint64_t sequence{0};

    uint64_t timestamp_ns{0};

    bool valid() const { return data != nullptr && size > 0; }
};

/// 待编码的一帧（视图，不持有内存）
///
/// 从 `VideoFrameHub::acquire()` 拿到的租约可以直接填进来：`data` 是
/// 映射地址、`dmabuf_fd` 是租约的 fd。填完交给 `encode()`，**不要**在这里
/// 拷贝像素。
struct VideoFrameView {
    /// Y 平面起始地址。零拷贝导入时也建议填（实现可能用它做 CPU 侧回读）。
    const void* data{nullptr};

    /// 可读字节数（边界检查用，不是「必须读完」）
    size_t size{0};

    /// dma-buf fd。>= 0 且后端支持时走**零拷贝导入**；-1 走内存拷贝。
    ///
    /// 调用方仍然持有这个 fd——编码器**不关闭它**，只借用。
    int dmabuf_fd{-1};

    uint64_t sequence{0};
    uint64_t timestamp_ns{0};
};

/// 编码器配置
struct VideoEncoderConfig {
    uint32_t width{0};
    uint32_t height{0};

    /// 输入 Y 平面每行字节数。**必须是驱动回报的事实值**，不能用 width 推。
    uint32_t stride{0};

    /// UV 平面相对帧起始的字节偏移。0 表示「等于 stride * height」。
    ///
    /// 本板实测 UV 偏移 = 2030336 而 stride*height = 2020480，中间 9856 字节
    /// 是对齐空洞。**硬件编码器同样需要这个事实值**：喂错位置会整帧偏色，
    /// 而且不报任何错。
    uint32_t uv_offset{0};

    /// 输入 fourcc（`V4L2_PIX_FMT_NV12` 等）。当前只支持 NV12。
    uint32_t fourcc{0};

    /// 一帧的总字节数（`sizeimage` 的事实值）。0 表示按 uv_offset*3/2 推算。
    uint32_t frame_bytes{0};

    uint32_t fps{25};          ///< 目标帧率（写进 SPS/VUI，也用于码率控制）
    uint32_t bitrate_kbps{2000};

    /// 关键帧间隔（帧数）。0 表示按 fps 推算（= 1 秒一个 IDR）
    uint32_t gop_size{0};

    /// 编码队列上限。超过时**主动丢最旧的帧**并计入 `dropped`。
    ///
    /// 为什么是丢旧而不是阻塞：采集线程绝不能被编码器拖住（它还要供帧统计）。
    /// 丢帧是可观测的降级，阻塞是不可观测的故障。
    uint32_t max_queue_depth{4};

    /**
     * @brief 是否允许 dma-buf 零拷贝导入（**默认关闭**）
     *
     * 本板几何条件满足零拷贝（`uv_offset == align16(w)*h`，实测严丝合缝），
     * 实测两条路径产出同级码流，而零拷贝省掉每帧 3.0 MB 的 memcpy。
     *
     * 但它有一个**调用方必须配合**的生命周期约束，所以默认关闭：
     *
     *   硬件编码是异步的。`encode()` 返回时硬件可能仍在读这块 dma-buf。
     *   而帧中枢的租约协议是「release(slot) 之后中枢就可以覆写这块内存」。
     *   于是调用方若在 `encode()` 返回后立刻 release，中枢下一次发布就会
     *   把新帧写进硬件正在读的内存——**画面错乱且不报任何错**。
     *
     * 开启本项后，调用方必须：**持有该租约直到下一次 encode() 返回或
     * flush() 完成**（届时上一帧的硬件读取必然已结束）。做不到就保持关闭，
     * 走 memcpy 路径——它多一次 3 MB 拷贝，但没有生命周期约束。
     *
     * 默认关闭是刻意的：默认值必须让调用方什么都不做也是对的。
     */
    bool allow_zero_copy{false};
};

/**
 * @brief 视频编码器接口
 *
 * 线程模型：`encode()` 由**一个**消费者线程调用（本类不保证线程安全）；
 * 帧中枢本身是线程安全的，多消费者各自持一个编码器实例即可。
 */
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    /// 配置并初始化编码器。返回 false 时 `describe()` 说明原因。
    virtual bool configure(const VideoEncoderConfig& config) = 0;

    /**
     * @brief 编码一帧
     *
     * @param frame    帧视图（含可选的 dma-buf fd）
     * @param out      输出的码流（仅在返回 true 时有效，且只在本调用返回后
     *                 的实现内部缓冲生命周期内有效——需要留存请拷贝）
     *
     * @return true 本次产出了一一对应的码流；false 表示「没有产出一一对应
     *         的一块码流」，这**不一定是错误**：硬件编码有延迟，前几帧可能
     *         返回 false 而 `stats().dropped` / `failed` 都不涨。
     */
    virtual bool encode(const VideoFrameView& frame, EncodedFrame& out) = 0;

    /// 冲出编码器内部所有延迟帧
    ///
    /// 硬件编码器有 pipeline 延迟；停止推流前不 flush 会丢掉最后几帧。
    /// @param out 每次回调一块码流
    /// @return 冲出多少帧（0 表示没有更多了）
    virtual size_t flush(const std::function<void(const EncodedFrame&)>& out) = 0;

    /// 取走并清零本期统计
    virtual EncoderStats snapshotAndReset() = 0;

    /// 当前累计统计（不清零），用于日志/快照
    virtual EncoderStats peekStats() const = 0;

    /// 后端名字（`cedar-ve` / `fake`），写进 evidence 溯源
    virtual std::string backend() const = 0;

    /// 人类可读状态（含失败原因），用于日志与证据
    virtual std::string describe() const = 0;
};

/**
 * @brief 创建一个编码器（工厂）
 *
 * 策略：**硬件优先，软件后备**。
 *
 * 顺序是实测定的，不是猜的：本板 cedar VE 实测 87fps（采集 18.5fps，4.7 倍
 * 余量）且 CPU 几乎零占用；软件 x264 需约 1.7 个 A55 核。有硬件却用软件，
 * 等于白占 21% 的 CPU 去换零收益。
 *
 * @param prefer_hardware false 时直接走桩实现（用于对照实验与无 VE 环境）
 *
 * 返回的对象**保证非空**：硬件不可用时自动回落，也不可用时返回一个
 * 「永远失败但能说清原因」的对象，而不是 nullptr。理由与帧源一致——
 * 调用方不必写空指针分支，能力缺失应体现在 `configure()` 的返回值和
 * `describe()` 里，而不是崩溃。
 */
std::unique_ptr<IVideoEncoder> createVideoEncoder(bool prefer_hardware = true);

/// 不用硬件、不用真编码器的桩实现：只做计数与几何校验，产出假码流
///
/// 用途：在无摄像头、无 VE 的机器上跑通整条消费者链路与全部归因单测。
/// **绝不能进生产产物**——它产出的字节不是合法 H.264，推出去就是垃圾数据。
std::unique_ptr<IVideoEncoder> createFakeVideoEncoder();

}  // namespace weaknet
