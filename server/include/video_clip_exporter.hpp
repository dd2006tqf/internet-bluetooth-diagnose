/**
 * @file video_clip_exporter.hpp
 * @brief 视频留证上行器：把编码器产出的 H.264 码流按「清单 + 分片」送到云端
 *
 * ============================================================================
 * 它在链路里的位置
 * ============================================================================
 *
 *   V4l2FrameSource ──▶ VideoFrameHub ──┬──▶ Qt 面板（渲染）
 *   （唯一所有者）       （租约交付）    ├──▶ VideoEncoder（编码）
 *                                       └──▶ **VideoClipExporter**（本文件）
 *                                                │ 清单 + base64 分片
 *                                                ▼
 *                                    云端 POST /edge/video-clips
 *
 * ============================================================================
 * 为什么这里只吃「一段已编码的字节」，不认识编码器也不认识中枢
 * ============================================================================
 *
 * 与 `IVideoEncoder` 同一个理由：只依赖 `const uint8_t*` + `size`，于是整条
 * 上行逻辑（分片、签名、重试、幂等、收齐判定）可以在**没有摄像头、没有 VE、
 * 没有开发板**的机器上用假码流单测。码流从哪来与「怎么把它送上去」无关。
 *
 * 唯一的额外输入是 `VideoClipMeta`：宽高、时间窗、可选事故关联。这些是
 * **清单字段**，云端靠它们建立索引；它们同样不由本类推导，由调用方按事实填。
 *
 * ============================================================================
 * 为什么是「清单 + 分片」两个端点，而不是一个大报文
 * ============================================================================
 *
 * 云端 `video_contracts.py` 已经钉死这个形状，理由在那里：所有 edge 上行
 * 共享 1 MB 原始体上限，而片段是几十 MB 量级。把上限抬高会同时抬掉**每个**
 * edge 端点的内存与解析代价。所以本类必须：
 *
 *   1. 先 `POST /edge/video-clips` 登记清单（几百字节）；
 *   2. 再逐片 `POST /edge/video-clips/{clip_id}/chunks/{index}`。
 *
 * 顺序不可颠倒：分片先到等于让服务端猜它属于哪个片段，云端会直接 404。
 *
 * ============================================================================
 * 分片边界：按字节切，不按 GOP 切
 * ============================================================================
 *
 * 一个自然的想法是「在 IDR 边界切分片」，这样每片都能独立解码。但云端契约
 * 里的 `declared_content_sha256` 与 `chunk_index` 都只描述**字节区间**，
 * 收齐后按 index 顺序拼接即可还原原始码流——播放端拿到的是完整 Annex-B
 * 码流，解码器自己会找起始码。按字节切不会破坏它，反而让分片大小可预测
 * （固定 640 KiB，云端就永远不需要处理「这个分片特别大」的边界情况）。
 *
 * 按 GOP 切还会引入一个隐性缺陷：码流长度由编码器决定，我们无法保证 GOP
 * 恰好落在分片上限之内；若某个 GOP 超过上限，就必须再切，于是「按 GOP 切」
 * 与「按字节切」同时存在，两套边界在收齐后对不上。
 *
 * 结论：**只按字节切**，`MAX_CHUNK_BYTES` 与云端 `MAX_VIDEO_CHUNK_BYTES`
 * 同源（640 KiB）。
 *
 * ============================================================================
 * 幂等：clip_id 是唯一的重试锚点
 * ============================================================================
 *
 * 板端网络是这条链路里最不可靠的一段，重试是**常态而非异常**。云端的幂等
 * 契约（见 `video_contracts.py` docstring）是：
 *
 *   - 清单：同 clip_id 同内容 → duplicate；同 clip_id 异内容 → 409 拒绝；
 *   - 分片：同 (clip_id, index) 同内容 → duplicate；异内容 → 409 拒绝。
 *
 * 因此本类的重试策略必须是「**用完全相同的字节重发**」。任何一次重试前
 * 重新序列化都会有风险：JSON 字段顺序或浮点格式漂移会让服务端把它判成
 * 「内容冲突」，于是一个本来只是丢包的片段永久卡死。所以：
 *
 *   `buildManifestBody()` / `buildChunkBody()` 的产出在**一次 upload 内**
 *   必须被缓存复用，绝不重算。这也是为什么它们返回完整报文字节而不是
 *   让调用方拼装。
 *
 * `clip_id` 由调用方给定（必须跨重试稳定）。本类提供 `makeClipId()` 生成
 * 一个合规的默认值，但它**只应被调用一次**并存进待上传队列，不能每次重试
 * 都重新生成——那会让每个重试都变成一个全新片段。
 *
 * ============================================================================
 * 为什么需要队列，而不是同步上传
 * ============================================================================
 *
 * 编码消费者线程必须持续消费帧中枢（否则中枢租约会堆积、面板会卡）。一次
 * 22 MB 片段在跨公网 RTT 234ms 的链路上要传几十秒，同步做会让编码线程
 * 几十秒不取帧。所以上行必须**另一个线程**，编码线程只做「拷走码流 + 入队」。
 *
 * 队列有上限：留证是事件触发的偶发行为，但异常期间可能连续触发。无界队列
 * 在长时间断网时会把内存吃光（22 MB/片段）。满了怎么办见 `enqueue()` 的
 * 注释——**丢最旧的**，并计入 `dropped_clips`，因为最新片段与当前事故更相关。
 *
 * ============================================================================
 * 已知边界（不假装没有）
 * ============================================================================
 *
 * 1. **队列与重试状态只在内存**。进程重启后未传完的片段丢失。持久化需要
 *    一个与数据库生命周期耦合的落盘队列，本类刻意不做——先把通道跑通，
 *    再决定是否值得引入（见 openspec/tasks 记录）。`dropped_clips` 会如实
 *    计数，不会静默。
 * 2. **不做整段校验**。`declared_content_sha256` 由本类计算并声明，但云端
 *    在请求路径上不回读全部对象（几十 MB 同步 IO）。整段校验属于离线 worker。
 *    本类只保证**声明值与实际上传字节一致**（同一份缓冲算出）。
 * 3. **不做带宽自适应**。分片大小固定，重试是指数退避但不是拥塞控制。跨公网
 *    上传会与既有遥测上行争抢带宽——这是真实代价，`stats()` 里能看到字节数。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "edge_snapshot_upload.hpp"
#include "weaknet_config.hpp"
#include "video_clip_muxer.hpp"

namespace weaknet {

/// 单片裸字节上限（base64 前）。与云端 `MAX_VIDEO_CHUNK_BYTES` **必须同源**。
///
/// 640 KiB 的推导见云端契约：base64 后约 873 KiB，加 JSON 外壳仍在 1 MB
/// 原始体上限内。改小只会增加请求数，改大则会被云端 413/422 拒绝。
constexpr size_t VIDEO_CLIP_MAX_CHUNK_BYTES = 640 * 1024;

/// 单片段裸字节上限。与云端 `MAX_VIDEO_CLIP_BYTES` 同源（256 MiB）。
///
/// 超过它的一律在入队前拒绝：让本地方就知道「这段永远不会被接受」，
/// 而不是传了几十 MB 之后在最后一片上拿到 422。
constexpr size_t VIDEO_CLIP_MAX_BYTES = 256ULL * 1024 * 1024;

/// 单片段分片数上限。与云端 `MAX_VIDEO_CLIP_CHUNKS` 同源（512）。
constexpr size_t VIDEO_CLIP_MAX_CHUNKS = 512;

/// 队列里最多同时等待的片段数。
///
/// 4 × 256 MiB 是理论上限，实际上限由 `max_queue_bytes` 兜底（默认 64 MiB）。
/// 两个上限都要有：片段数防「很多小片段」，字节数防「几个大片段」。
constexpr size_t VIDEO_CLIP_MAX_QUEUED_CLIPS = 4;

/// 上传器的运行统计。
struct VideoClipExporterStats {
    uint64_t clips_enqueued{0};      ///< 入队的片段数
    uint64_t clips_uploaded{0};      ///< **全部分片确认送达**的片段数
    uint64_t clips_failed{0};        ///< 重试耗尽后放弃的片段数
    uint64_t clips_dropped{0};       ///< 因队列满被丢弃的片段数（含入队时与运行中）

    uint64_t manifests_sent{0};      ///< 清单请求成功次数（含 duplicate）
    uint64_t chunks_sent{0};         ///< 分片请求成功次数（含 duplicate）
    uint64_t bytes_sent{0};          ///< 已确认送达的**裸字节**（不含 base64 膨胀）

    uint64_t send_failures{0};       ///< HTTP 层失败次数（含重试）

    /// MP4 封装失败次数。**不**计入 `clips_dropped`：失败时改传裸码流，
    /// 片段照样送达。之所以要单独有这一项，是因为它的处置与「传不上去」
    /// 完全不同——这一项涨说明板端缺 ffmpeg 或编码器产出不合规，
    /// 而 `clips_dropped` 涨才是留证真的丢了。
    uint64_t mux_failures{0};

    // ---- 静帧（与片段同一条上行通路，独立计数）----
    //
    // 为什么必须独立计数而不是并进 clips_*：一个静帧传不上去与一段码流传
    // 不上去，处置完全不同。片段的失败多半是带宽/体积问题，静帧的失败多半
    // 是契约字段问题（120 KB 在任何链路上都传得动）。混在一起计数会让
    // 「静帧全挂了但片段正常」表现为「总体成功率还行」。
    uint64_t snapshots_enqueued{0};  ///< 入队的静帧数
    uint64_t snapshots_uploaded{0};  ///< 确认送达的静帧数
    uint64_t snapshots_dropped{0};   ///< 被丢弃/放弃的静帧数
    uint64_t snapshot_bytes_sent{0}; ///< 静帧已送达的裸字节
};

/// 一个待上传片段的自述（清单字段）。
///
/// 全部字段都是**事实**：宽高来自驱动回报的几何，时间窗来自编码帧的时间戳，
/// `incident_id` 由调用方按当前关联事故填（无关联就留空）。本类不推导任何
/// 一项——用宽度反推高度、用当前时间代替帧时间都会让云端索引到错误的东西。
struct VideoClipMeta {
    /// 片段标识。必须跨重试稳定；不合法字符集会被云端 422 拒绝。
    std::string clip_id;

    std::string device_id;   ///< 与 edge.device_id 一致
    std::string site_id;     ///< 可空（云端留空表示回落 device_id）
    std::string gateway_id;  ///< 可空

    /// 关联的区域事故。空串 = 「没有关联事故」，不是「关联到空事故」。
    std::string incident_id;

    uint32_t width{0};
    uint32_t height{0};
    uint32_t fps{25};

    /// 片段内的帧数（含首帧与末帧）。
    ///
    /// 为什么清单里没有它、却仍然要有：云端不需要帧数（时长由时间窗给），
    /// 但**封装 MP4 需要**——容器里没有「帧率」这个独立的量，它是由
    /// 「帧数 ÷ 时长」反推出来的。少了帧数就只能硬编一个名义帧率，
    /// 于是容器时间轴与真实时间轴之间有一处恒定比例的漂移，
    /// 「事发那一刻在第几秒」会系统性偏掉。
    ///
    /// 由 `VideoClipRecorder::poll()` 按**实际拼进片段的帧数**填。
    uint32_t frame_count{0};

    /// 片段覆盖的时间窗（墙钟毫秒，UTC epoch）。`ended >= started`。
    uint64_t started_at_ms{0};
    uint64_t ended_at_ms{0};

    bool valid() const {
        return !clip_id.empty() && !device_id.empty() && width > 0 && height > 0 &&
               ended_at_ms >= started_at_ms;
    }
};

// ============================================================================
// 片段录制器：决定「留哪一段」，不负责送
// ============================================================================
//
// 为什么不把这段逻辑塞进 `server.cpp` 的编码线程里：它有三条容易写错、
// 且**错了不会报错**的规则（回溯窗口、必须从 IDR 起、时间窗换算），
// 写在线程 lambda 里就再也无法用假码流验证。
//
// 时间语义（最容易错的一点）：**窗口判定用单调时钟，清单用墙钟**。
//
// 编码帧的 `timestamp_ns` 来自 `v4l2_buffer.timestamp`，是内核**单调时钟**；
// 而事故（`SiteIncident`）与清单字段用的是**墙钟毫秒**。两者混用不会报错，
// 只会让片段在云端索引到错误的时间点——事故回看时「跳过去正好什么都没有」。
// 所以本类内部只认单调毫秒，墙钟只在 `poll()` 产出清单时按一次采样换算。

/// 录制窗口与身份配置。
struct VideoClipRecorderConfig {
    /// 触发前的回溯时长。
    ///
    /// 事故被关联器确认时，**最有价值的部分往往已经过去了**——等到确认再
    /// 开始录，拍到的全是事后。所以必须常驻一段预录。
    uint32_t pre_roll_ms{30 * 1000};

    /// 触发后的延续时长。事故是持续过程，只录到触发瞬间会丢掉后续恶化。
    uint32_t post_roll_ms{60 * 1000};

    /// 缓冲总字节上限。
    ///
    /// 2000 kbps 下 90 秒约 22 MB，48 MiB 留一倍余量。上限是硬性的：
    /// 异常期间可能连续触发，无界缓冲会把内存吃光（片段是 MB 级）。
    uint64_t max_bytes{48ULL * 1024 * 1024};

    /// 清单身份字段（与 `edge.*` 同源，由调用方按事实填）
    std::string device_id;
    std::string site_id;
    std::string gateway_id;

    /// 片段几何。必须来自编码器 configure 的**事实值**，不能推导。
    uint32_t width{0};
    uint32_t height{0};
};

/**
 * @brief 环形预录 + 事故触发的片段录制器。
 *
 * 用法（生产，编码线程内）：
 * ```cpp
 * recorder.push(ef.data, ef.size, ef.keyframe, mono_ms);
 * if (新事故) recorder.startClip(incident.incident_id);
 * if (auto meta = recorder.poll(mono_now, wall_now, &bytes)) {
 *     exporter.enqueue(*meta, std::move(bytes));
 * }
 * ```
 *
 * 线程模型：`push` / `startClip` / `poll` 都在**同一个**编码线程调用，
 * 但内部仍加锁——这样将来若把 `poll` 挪到别的线程也不会悄悄变成数据竞争。
 */
class VideoClipRecorder {
public:
    explicit VideoClipRecorder(VideoClipRecorderConfig cfg);

    /// 喂一帧已编码字节。`mono_ms` 必须是**单调时钟**毫秒。
    ///
    /// 拷贝是必然的：`EncodedFrame.data` 只在下一次 `encode()` 之前有效，
    /// 而预录窗口要跨越几十秒。
    void push(const uint8_t* data, size_t size, bool keyframe, uint64_t mono_ms);

    /// 触发录制（事故确认时调用）。
    ///
    /// @return false 表示**已经在录**，本次触发被忽略。一次事故只产出一个
    ///         片段：重复触发若各自开一段，云端会收到若干互相重叠的碎片。
    bool startClip(const std::string& incident_id);

    bool isRecording() const;
    std::string currentIncident() const;

    /// 若已到后沿，产出片段（清单元数据 + 拼接后的码流字节）。
    ///
    /// @param mono_now_ms 当前单调时钟毫秒（判后沿用）
    /// @param wall_now_ms 当前墙钟毫秒（与 `mono_now_ms` 同时采样）
    /// @param out_bytes   产出码流；调用方接管
    /// @return 产出的清单元数据；无产出时 nullopt
    std::optional<VideoClipMeta> poll(uint64_t mono_now_ms, uint64_t wall_now_ms,
                                      std::vector<uint8_t>* out_bytes);

    /// 放弃当前录制（缓冲区超限 / 找不到起始 IDR）。
    void abort();

    size_t bufferedBytes() const;
    size_t bufferedFrames() const;
    uint64_t clipsProduced() const;   ///< 成功产出的片数
    uint64_t clipsAborted() const;    ///< 放弃的片数（含超限与无 IDR）

private:
    struct Frame {
        std::vector<uint8_t> bytes;
        bool keyframe{false};
        uint64_t mono_ms{0};
    };

    void abortLocked();

    VideoClipRecorderConfig cfg_;

    mutable std::mutex mutex_;
    std::deque<Frame> frames_;
    size_t buffered_bytes_{0};
    /// 已 push 的最新单调时刻。窗口以它为准，而不是以 poll 的 now 为准——
    /// 采集停掉之后 now 会继续走，用现在的时间裁窗口会把整段预录裁光。
    uint64_t newest_mono_ms_{0};

    bool recording_{false};
    std::string incident_id_;
    uint64_t trigger_mono_ms_{0};

    uint64_t clips_produced_{0};
    uint64_t clips_aborted_{0};
};

/**
 * @brief 视频留证上行器。
 *
 * 用法（生产）：
 * ```cpp
 * VideoClipExporter exporter(cfg, "http://.../edge/telemetry");
 * if (!exporter.start()) { ... }        // 配置不全时保持关闭，不出站
 * ...
 * VideoClipMeta meta = ...;              // clip_id 只生成一次
 * exporter.enqueue(meta, std::move(codec_bytes));
 * ...
 * exporter.stop();                       // 幂等，尽量传完在途
 * ```
 *
 * 用法（单测）：直接调 `runOnce()` 而不 `start()` 后台线程——与
 * `EdgeWirelessUplinkExporter` 同一可测性先例。
 */
class VideoClipExporter : public ISnapshotUploader {
public:
    /// @param config     运行时配置（只读 edge.* 字段，与既有上行器同源）
    /// @param telemetry_url `edge.url`；本类从中派生 `/video-clips` 端点。
    ///        显式传入而不是从 config 再读一次：派生规则集中在一处，
    ///        避免两个上行器对同一个配置键给出不同解释。
    VideoClipExporter(const weaknet_dbus::WeakNetConfig& config,
                      std::string telemetry_url);
    ~VideoClipExporter();

    VideoClipExporter(const VideoClipExporter&) = delete;
    VideoClipExporter& operator=(const VideoClipExporter&) = delete;

    /// 配置校验 + 启动后台上传线程。false 表示未启动且无任何出站流量。
    bool start();
    /// 停止线程。**幂等**。返回时在途片段已尽力传完（或被计入 dropped）。
    void stop();

    bool isRunning() const { return running_.load(); }

    /// 队列满时是否接受（见实现注释：默认丢最旧）。
    void setMaxQueueBytes(size_t bytes) { max_queue_bytes_ = bytes; }

    /// 覆盖 ffmpeg 可执行文件路径（**仅用于测试**）。
    ///
    /// 生产值就是 `kDefaultFfmpegPath`。单测需要它，否则「封装成功」这条
    /// 用例在没装 ffmpeg 的 CI 机器上会直接失败，而那条用例要验的是
    /// 「产物是否被正确接管」，不是「本机装没装 ffmpeg」——两件事混在一起
    /// 会让测试结果随环境漂移，是最容易养出「反正就是红的」那种状态的做法。
    void setFfmpegPathForTest(const std::string& path) { ffmpeg_path_ = path; }

    /// 重试策略（**仅用于测试**）。
    ///
    /// 默认值是生产值（8 次、1s 起指数退避到 60s 上限）。单测把它设成
    /// 「1 次、不退避」是为了让「失败路径」用例在毫秒级结束——否则一个
    /// 只断言失败计数的用例要真的等满 2 分钟退避，那既拖慢 CI，也让
    /// 失败原因（网络不可达）被淹没在退避日志里。
    ///
    /// 生产代码**不得**调用它：缩短重试轮次会把「临时抖动」直接变成
    /// 永久丢片段，而留证恰恰发生在网络最差的时候。
    struct RetryPolicy {
        uint32_t max_attempts{8};
        uint32_t base_backoff_ms{1000};
        uint32_t max_backoff_ms{60000};
    };
    void setRetryPolicyForTest(const RetryPolicy& policy) { retry_policy_ = policy; }

    /**
     * @brief 入队一个待上传片段。
     *
     * **接管 `codec_bytes` 的所有权**（move）：片段在队列里要等到真正开始
     * 上传才分片，而分片要求整段可读。这里多留一份完整码流是刻意的——
     * 重试必须复用完全相同的字节（见文件头「幂等」一节），不能从编码器
     * 再取一次。
     *
     * @return false 表示被拒绝（队列满 / 超上限 / 元数据不合法）。拒绝是
     *         **可观测的**：`stats().clips_dropped` 会涨。绝不静默丢弃。
     */
    bool enqueue(const VideoClipMeta& meta, std::vector<uint8_t> codec_bytes);

    /**
     * @brief 入队一张事故静帧（`ISnapshotUploader` 的实现）
     *
     * 与 `enqueue()` 共用**同一条**上行通路、同一套凭据、同一个超时与重试
     * 策略——两条通路会让签名/超时/退避语义各说各话，而它们漂移时不会报错。
     *
     * 与片段的差别只有两处，都是语义上的：
     *   - 静帧**不分片**（单张 120 KB 量级，分片只会多一个失败点）；
     *   - 静帧**不做 MP4 封装**（它已经是最终交付形态的 JPEG）。
     *
     * @return false 表示未接收（未启动 / 队列满 / 自述不合法 / 超单张上限），
     *         拒绝一律计入 `stats().snapshots_dropped`，不静默。
     */
    bool enqueueSnapshot(EdgeSnapshotUpload shot) override;

    /// 执行一轮静帧上传：从静帧队列取一张并 POST。
    ///
    /// 公开以便单测同步驱动（与 `runOnce()` 同一可测性做法）。
    bool runSnapshotOnce(std::string* error);

    /// 执行一轮：从队列取一个片段并完整上传（清单 + 全部分片）。
    ///
    /// 公开以便单测同步驱动（不起线程）。生产路径由 `run()` 循环调用。
    /// @return true 表示本片段全部分片确认送达。
    bool runOnce(std::string* error);

    VideoClipExporterStats stats() const;

    /// 队列里还有多少个片段（含正在上传的那个）。
    size_t queueDepth() const;

    /// 从 telemetry 端点派生 video-clips 端点。
    ///
    /// 与 `EdgeWirelessUplinkExporter::uplinkUrl()` 同一形态：形态不符约定时
    /// **返回空串**而不是猜一个地址——把留证发到未知端点比失败更糟。
    static std::string deriveClipsUrl(const std::string& telemetry_url);

    /// 从 telemetry 端点派生「某片段某分片」的端点。
    static std::string deriveChunkUrl(const std::string& telemetry_url,
                                      const std::string& clip_id, size_t chunk_index);

    /// 从 telemetry 端点派生静帧端点（`/edge/snapshot-images`）。
    ///
    /// 与 `deriveClipsUrl` 同一纪律：形态不符时返回**空串**，绝不猜地址。
    static std::string deriveSnapshotsUrl(const std::string& telemetry_url);

    /// 构造静帧报文字节（`network.edge.snapshot-images.v1`）。
    ///
    /// 公开以便单测直接断言字段名与取值：这些是**对外契约**，字段名写错只有
    /// 云端 422 才会发现，而本地日志只会留下「上传失败」。
    ///
    /// `payload` 必须就是将要传输的那份字节——摘要与 base64 都从它算，
    /// 不接受「声明一个、发另一个」。
    static std::string buildSnapshotBody(const EdgeSnapshotUpload& shot,
                                         const std::string& device_id,
                                         const std::string& site_id,
                                         const std::string& gateway_id,
                                         std::string* error);

    /// 生成一个合规的 `clip_id`（`^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$`）。
    ///
    /// 形态：`<device>-<epoch_ms>-<rand6>`。前段可读、中段可按时间排序、
    /// 后段防同一毫秒内碰撞。**只应调用一次**并存进待上传队列。
    static std::string makeClipId(const std::string& device_id, uint64_t epoch_ms);

    /// 构造清单报文字节（`network.edge.video-clips.v1`）。
    ///
    /// 公开以便单测直接断言字段名与取值——这些是**对外契约**，改错了
    /// 云端 422 才会发现。
    static std::string buildManifestBody(const VideoClipMeta& meta, size_t total_bytes,
                                         size_t expected_chunks,
                                         const std::string& content_sha256_hex,
                                         std::string* error);

    /// 构造单个分片报文字节。`payload` 是本分片的裸字节区间。
    static std::string buildChunkBody(const std::string& device_id,
                                      const std::string& clip_id, size_t chunk_index,
                                      const uint8_t* payload, size_t payload_size,
                                      std::string* error);

    /// 分片规划：给定总字节数，返回分片数。空输入返回 0。
    static size_t planChunkCount(size_t total_bytes);

    /// sha256（小写 hex）。失败返回空串。
    static std::string sha256Hex(const uint8_t* data, size_t size);

    /// 标准 base64（带 padding）。
    static std::string base64Encode(const uint8_t* data, size_t size);

private:
    struct PendingClip {
        VideoClipMeta meta;
        std::vector<uint8_t> bytes;
        /// 重试计数（清单与分片各自独立推进，见 runOnce）。
        uint32_t attempts{0};
    };

    /// 一张待上传静帧。与 `PendingClip` 分开而不是加个 flag：
    /// 两者的上传流程不同（一张要走清单+分片+封装，一张只发一个 POST），
    /// 合在一个结构里会让 `runOnce` 里到处是 `if (is_snapshot)`，
    /// 而那种分支最容易被后来的改动漏掉一半。
    struct PendingSnapshot {
        EdgeSnapshotUpload shot;
        std::string body;            ///< **预序列化**的报文（重试必须复用同一份字节）
        std::string signature;       ///< 对 `body` 的签名，与 body 同寿命
        /// 裸 JPEG 字节数。`body` 是 base64 之后的大小（约 1.33 倍），
        /// 而计数要与片段的 `bytes_sent` 同一口径——否则「静帧只占了几百 KB」
        /// 这种结论会凭 base64 膨胀被高估三成。原始字节在 `body` 构造完就
        /// 不再需要，只留这个数字。
        size_t raw_bytes{0};
        uint32_t attempts{0};
    };

    void run();
    bool uploadManifestOnce(const PendingClip& clip, size_t total_bytes,
                            size_t expected_chunks, const std::string& content_sha,
                            std::string* error);
    bool uploadChunkOnce(const PendingClip& clip, size_t index, std::string* error);

    /// 发送已签名报文。`signature` 由调用方传入而不是内部再签一次：
    /// 签名必须覆盖**与发送完全相同的字节**，分两步会让「签了什么」和
    /// 「发了什么」有机会不一致。
    bool postSigned(const std::string& url, const std::string& body,
                    const std::string& signature, std::string* response,
                    std::string* error) const;
    std::string signBody(const std::string& body, std::string* error) const;

    /// 与既有 exporter 一致：**引用**而不拷贝。`WeakNetConfig` 含 mutex/atomic，
    /// 拷贝构造已被删除；更要紧的是配置必须与其它上行器看到**同一份**事实，
    /// 拷贝一份就等于允许两处对同一配置键给出不同解释。
    /// 调用方保证 config 生命周期覆盖本对象（生产路径里它是 ServerContext 成员）。
    const weaknet_dbus::WeakNetConfig& config_;
    std::string telemetry_url_;
    size_t max_queue_bytes_{64ULL * 1024 * 1024};
    RetryPolicy retry_policy_{};

    /// ffmpeg 可执行文件路径。默认见 `video_clip_muxer.hpp`。
    /// 做成成员而非编译期常量：单测要能指向替身（见 setFfmpegPathForTest）。
    std::string ffmpeg_path_{kDefaultFfmpegPath};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<PendingClip> queue_;
    size_t queued_bytes_{0};

    /// 静帧队列。**独立于片段队列**，理由有两条：
    ///   - 片段可能几十 MB，静帧只有 120 KB。共用一个字节配额时，一个待传
    ///     的 22 MB 片段会把配额吃光，于是事故现场那张唯一的图反而被丢；
    ///   - 两者的时效性不同：片段是「那几十秒怎么变化」，晚几分钟仍有价值；
    ///     静帧是「现场长什么样」，同一事故只要一张，丢一张就没有替代品。
    /// 所以静帧用**条数**上限（`kMaxQueuedSnapshots`）而不是字节上限。
    std::deque<PendingSnapshot> snapshot_queue_;
    /// 一次只排这么几张。事故是偶发的，正常不会同时有第二张；能排到 4 张
    /// 说明上行已经堵了一阵，此时再多排也只是把「几小时前的现场」往后堆。
    static constexpr size_t kMaxQueuedSnapshots = 4;

    /// 用 atomic 而不是 bool：`stop()` 与上传线程并发访问它。
    /// 普通 bool 在这里是数据竞争（UB），而它的失效表现是「停止信号
    /// 偶尔看不到」——那种 bug 在压力下才复现，且无法从日志定位。
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};    std::thread thread_;

    mutable std::mutex stats_mutex_;
    VideoClipExporterStats stats_;
};

}  // namespace weaknet
