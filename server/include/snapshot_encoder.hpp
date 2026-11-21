/**
 * @file snapshot_encoder.hpp
 * @brief 单帧快照编码器：把一帧 NV12 编成一张 JPEG
 *
 * ============================================================================
 * 为什么**不**复用 IVideoEncoder
 * ============================================================================
 *
 * 看起来「都是把 NV12 变成压缩字节，应该复用」，但那套接口处处是 H.264
 * 的语义，硬塞 JPEG 会同时污染两边：
 *
 * | IVideoEncoder 的成员 | 在 H.264 里的含义 | 在 JPEG 里是什么 |
 * | --- | --- | --- |
 * | `EncodedFrame::keyframe` | 是否 IDR | **无意义**（每张都是完整图） |
 * | `VideoEncoderConfig::gop_size` | 关键帧间隔 | **无意义** |
 * | `VideoEncoderConfig::bitrate_kbps` | 码率控制 | **无意义**（质量参数取代它） |
 * | `flush()` | 冲出 pipeline 延迟帧 | 常为空操作 |
 * | `encode()` 返回 false | 异步延迟，不是错误 | **就是失败** |
 *
 * 更硬的约束在下游：`VideoClipRecorder` 依赖 `keyframe` 做 GOP 切分，
 * 保证片段从 IDR 起——那是 H.264 独有的可解码性条件。让 JPEG 帧流进
 * 那条路径会产生「每帧都是关键帧」的荒谬判定。
 *
 * 所以这里是**独立接口**，语义只有一条：**给我一帧，给你一张图，或者
 * 明确失败**。没有异步、没有延迟、没有 GOP。
 *
 * ============================================================================
 * 为什么后端要做成可注入的
 * ============================================================================
 *
 * 本板 cedar VE 的 JPEG 路径已实测可用（见下），但：
 *
 *   - x86 开发机上没有 libvencoder.so，整条逻辑必须能在无硬件时单测；
 *   - 「硬件不可用」必须是**可观测的降级**，而不是崩溃或静默产坏图。
 *
 * 因此工厂 `createSnapshotEncoder()` 与 `createVideoEncoder()` 同构：
 * 硬件优先、失败回落、**保证返回非空对象**，能力缺失体现在
 * `configure()` 的返回值与 `describe()` 里。
 *
 * ============================================================================
 * 本板实测（2026-11-20，Radxa Cubie A7A，A733 / cedar VE）
 * ============================================================================
 *
 * 用与生产完全相同的 API 序列在板端编码一帧 640x480 合成 NV12：
 *
 *   VideoEncCreate(VENC_CODEC_JPEG=1)   → 非空
 *   VideoEncSetParameter(0x200, 85)     → rc=0      （JPEG quality 1~100）
 *   VideoEncInit                        → rc=0
 *   AllocInputBuffer                    → rc=0
 *   AddOneInputBuffer                   → rc=0
 *   VideoEncodeOneFrame                 → rc=0
 *   GetOneBitstreamFrame                → rc=0  size0=10263
 *
 * 产物 `file(1)` 判定：`JPEG image data, JFIF standard 1.01, baseline,
 * precision 8, 640x480, components 3`；段结构 SOI/APP0/DQT×2/SOF0/DHT×4/
 * SOS/EOI 全部齐备。**这是真 JPEG，不是"能返回成功但内容是垃圾"。**
 *
 * 注意 `VideoEncCreate(1)` 返回非空**本身不足以证明可用**——它只说明对象
 * 建出来了。判据必须是端到端取到以 `FF D8 FF` 开头、以 `FF D9` 结尾的
 * 字节流。板厂同时提供了独立的 `libvenc_jpeg.so`（含 `JpegEncOpen` /
 * `JpegEncEncode` 等另一套 API），但 `libvencoder.so` 的 DT_NEEDED 里
 * **没有**它，JPEG 走的是主库内置的 `AWJpecEnc` 分支。所以不要试图
 * dlopen 那个 so 去找 `VideoEncCreate`——它没有。
 *
 * ============================================================================
 * 质量参数：为什么默认 70 而不是 85 或 100
 * ============================================================================
 *
 * 留证快照的用途是「让人看清事发那一刻发生了什么」，不是交付印刷品。
 *
 * **本板 1640x1232 实测（2026-11-20，合成梯度+棋盘纹理，6 张/档）**：
 *
 *   quality=60  → 569 KB/张  （余量 86 KB）
 *   quality=70  → 624 KB/张  （余量 31 KB）  ← 默认
 *   quality=75  → 634 KB/张  （余量 21 KB）
 *   quality=85  → 686 KB/张  （**超出** 30 KB）
 *   quality=100 → 1095 KB/张 （**超出** 440 KB）
 *
 * 云端 `video_contracts.py` 的 `MAX_VIDEO_CHUNK_BYTES = 640 KiB = 655360`，
 * 那是**单个分片原始体（base64 前）**的上限。所以 not just「越大越慢」——
 * 超过 640 KiB 的图必须分片，而分片意味着两块 base64、两次请求、
 * 两个失败点。默认值选 70 是为了让**绝大多数快照都是单片上传**。
 *
 * ⚠️ 早期版本这里写的是「quality=85 → 约 120 KB/张」。那是 **640x480**
 * 下的测量值，被错误地套用到 1640x1232（面积 6.6 倍）。按面积折算
 * 120KB × 6.6 ≈ 790KB，实测 686KB 本就自洽——**数字没错，是被搬错了
 * 分辨率的上下文**。这类错误不会报错，只会让「一张图」悄悄变成
 * 「一次分片上传」，所以把实测表写进类型注释而不是留在提交信息里。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace weaknet {

/// 快照编码配置
struct SnapshotEncoderConfig {
    uint32_t width{0};
    uint32_t height{0};

    /// 输入 Y 平面每行字节数。**必须是驱动回报的事实值**，不能用 width 推。
    /// 本板实测 1640 宽下 stride=1640，但其它分辨率未必 16 对齐后仍等于 width。
    uint32_t stride{0};

    /// UV 平面相对帧起始的字节偏移。0 表示「等于 stride * height」。
    ///
    /// 本板实测 UV 偏移 = 2030336 而 stride*height = 2020480，中间 9856 字节
    /// 是对齐空洞。喂错位置会整帧偏色**且不报任何错**——这是本模块最贵的
    /// 一个隐性前提，写在类型里而不是让调用方自己记。
    uint32_t uv_offset{0};

    /// 一帧的总字节数（`sizeimage` 的事实值）。0 表示按 uv_offset*3/2 推算。
    uint32_t frame_bytes{0};

    /// JPEG 质量，1~100。0 表示用默认值 85（理由见头注释）。
    int quality{0};

    // 这里**故意没有** `timeout_ms`。
    //
    // 曾经写过一个，后来删掉了：硬件编码调用（`VideoEncodeOneFrame` /
    // `GetOneBitstreamFrame`）是库内部的同步阻塞调用，没有任何取消句柄，
    // 在调用线程里做超时只能是「另起线程去等、超时就把结果丢掉」——
    // 而那块输入描述符此时仍归硬件所有，丢结果等于泄漏，下一次 capture()
    // 直接卡在描述符池耗尽上。留一个设了却不起作用的旋钮，比没有更危险：
    // 调用方会以为「最坏情况有上限」。
    //
    // 真正的超时必须在**上层**做：调用方在自己的线程里 capture()，
    // 另一个线程按 deadline 判定「这张图我不要了」，超时就放弃整条链路
    // （进程内重建编码器），而不是指望这一次调用返回。
};

/// 冻结的常量：云端其余分片上限都从这里派生
///
/// 与 `video_contracts.py` 里的 `MAX_VIDEO_CHUNK_BYTES` 是**同一朵约束的
/// 两端**。板端先自查一遍，是为了把「图太大」这个事实在**编码完成后立刻**
/// 暴露出来，而不是等一次 HTTP 422 再回头猜是哪个字段不合规。
constexpr uint32_t kMaxSnapshotChunkBytes = 640 * 1024;  // 655360

/// 一张编码好的快照
///
/// 与 `EncodedFrame` 的关键区别：**没有 keyframe，没有 sequence 对齐问题**。
/// `data` 的所有权在本对象里（`std::vector`），调用方可以直接 move 走，
/// 不存在「只在本次调用内有效」的陷阱——快照是一次性产物，生命周期
/// 和「一帧接一帧的码流」根本不是一回事。
struct SnapshotImage {
    std::vector<uint8_t> bytes;

    uint32_t width{0};
    uint32_t height{0};

    /// 采集侧那一帧的序号与时间戳，原样透传，用于把图和时刻对上
    uint64_t sequence{0};
    uint64_t timestamp_ns{0};

    /// 编码耗时（毫秒）。快照是低频操作，这个值主要用于发现 VE 被抢
    int encode_ms{0};

    bool valid() const { return !bytes.empty(); }

    /// 是否超过云端单分片上限（需分片上传，或降低质量重编）。
    ///
    /// 为什么要单独一个方法而不是让调用方自己比大小：640 KiB 这个数字
    /// 在云端契约、板端默认质量、分片规划三处出现，任何一处单独改都会
    /// 失配。这里至少把它固化成一个**可被单测直接打中**的判断。
    bool fitsSingleChunk() const {
        return bytes.size() <= kMaxSnapshotChunkBytes;
    }

    /// 是否为合法 JPEG 头（`FF D8 FF`）。
    ///
    /// 提供它是因为「后端说成功」和「产出真的是 JPEG」是两件事：
    /// 板厂库在错误配置下可能返回 0 却给出一段非 JPEG 内存。调用方
    /// （尤其落盘/上传路径）应该用它做最后一道守门，而不是信任返回值。
    bool looksLikeJpeg() const {
        return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 &&
               bytes[2] == 0xFF;
    }
};

/// 待编码的一帧（视图，不持有内存）
///
/// 与 `VideoFrameView` 同构但**刻意不复用**：那个类型带着 `dmabuf_fd`
/// 与「零拷贝生命周期契约」，而快照是一次性同步操作，每次都会拷进
/// 编码器缓冲后立刻取走，不存在「硬件还在读」的窗口。引入 fd 字段
/// 只会让调用方以为快照也支持零拷贝，进而做出错误的租约管理。
struct SnapshotFrameView {
    const void* data{nullptr};
    size_t size{0};

    uint64_t sequence{0};
    uint64_t timestamp_ns{0};
};

/**
 * @brief 快照编码器接口
 *
 * 线程模型：**不保证线程安全**。一个实例一次只编一张。
 *
 * 之所以可以是同步的：JPEG 编码器内部没有 pipeline 深度（每帧独立，
 * 不像 H.264 需要参考帧），`encode()` 返回时码流已经可取走。
 */
class ISnapshotEncoder {
public:
    virtual ~ISnapshotEncoder() = default;

    /// 配置并初始化。返回 false 时 `describe()` 说明原因。
    ///
    /// 幂等：已配置成功时重复调用返回 true（不重建）。
    virtual bool configure(const SnapshotEncoderConfig& config) = 0;

    /**
     * @brief 把一帧编成 JPEG
     *
     * @param frame 帧视图。`data` 必须可读 `uv_offset + chroma` 字节。
     * @param out   成功时填入完整 JPEG 字节
     *
     * @return true 表示**确实产出了一张完整 JPEG**（实现内部已校验
     *         `FF D8 FF` 开头）。false 表示这次没拿到图，原因看
     *         `describe()`——注意它与 `IVideoEncoder::encode()` 的语义
     *         不同：那里返回 false 可能只是异步延迟，这里返回 false 就是
     *         没有图。
     */
    virtual bool capture(const SnapshotFrameView& frame, SnapshotImage& out) = 0;

    /// 后端名字（`cedar-ve-jpeg` / `fake`），写进日志与证据
    virtual std::string backend() const = 0;

    /// 人类可读状态（含失败原因）
    virtual std::string describe() const = 0;

    /// 累计成功张数与失败次数
    struct Stats {
        uint64_t captured{0};
        uint64_t failed{0};
    };
    virtual Stats stats() const = 0;
};

/**
 * @brief 创建一个快照编码器（工厂）
 *
 * 与 `createVideoEncoder()` 同一策略：**硬件优先，失败回落**。
 *
 * 返回的对象**保证非空**：硬件不可用时返回一个「永远失败但能说清原因」
 * 的对象。理由与帧源一致——调用方不必写空指针分支，能力缺失应体现在
 * `configure()` 的返回值和 `describe()` 里。
 *
 * @param prefer_hardware false 时直接走桩实现（用于对照实验与无 VE 环境）
 */
std::unique_ptr<ISnapshotEncoder> createSnapshotEncoder(bool prefer_hardware = true);

/**
 * @brief 桩实现：不做真编码，产出固定内容的假字节
 *
 * 用途：在无 VE 的机器上跑通「取帧 → 编码 → 落盘 → 上传」整条消费者链路。
 * **绝不能进生产产物**——它产出的不是合法 JPEG，写出去就是坏文件。
 */
std::unique_ptr<ISnapshotEncoder> createFakeSnapshotEncoder();

}  // namespace weaknet
