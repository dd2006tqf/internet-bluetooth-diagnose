/**
 * @file snapshot_service.hpp
 * @brief 事故留证快照：按需从 VideoFrameHub 取一帧，编成 JPEG 落盘
 *
 * ============================================================================
 * 为什么是「按需」而不是「持续」
 * ============================================================================
 *
 * 采集线程每帧要面对 1640x1232x1.5 = 3 MB、30fps 的流量。如果为了「随时能
 * 拍照」而在 sink 里持续深拷贝或持续编码，代价是：
 *
 *   - 持续深拷贝：90 MB/s 的 memcpy，纯浪费；
 *   - 持续编码：JPEG 实测 10~12 ms/张，等于给 33 ms 的帧预算再砍掉三分之一。
 *
 * 而快照的用途是「事故发生时留一张现场图」——它天然是低频事件。所以正确
 * 的形状是：**平时什么都不做，事故来了才去 hub 取当前那一帧**。
 *
 * `VideoFrameHub::acquire()` 正好提供这个能力：它返回一个 dup 出来的
 * dma-buf fd，我们 mmap 后读一帧、编一张、释放租约。既不需要改采集线程，
 * 也不需要额外缓冲。
 *
 * ============================================================================
 * 线程模型
 * ============================================================================
 *
 * ```
 *   采集线程 ──publish()──> VideoFrameHub ──acquire()──> 快照线程
 *   (已有路径，零改动)                        (本模块)
 * ```
 *
 * 本类保证：
 *   - `request()` **不阻塞**，只是把事故 ID 放进待办队列；
 *   - 所有编码与落盘发生在自己起的线程里；
 *   - 队列有上限，满了丢最旧的并计数——事故高峰期宁可少几张图，也不能让
 *     内存无界增长。
 *
 * 为什么不在事故线程里直接编：`ISnapshotEncoder` 明确**不是线程安全的**，
 * 而事故巡检线程与采集线程是两个线程。所有 `capture()` 调用集中在单一
 * 工作线程里，是这个类存在的核心理由之一。
 *
 * ============================================================================
 * 落盘布局
 * ============================================================================
 *
 * ```
 * <dir>/<incident_id>/<unix_ms>_<sequence>.jpg
 * ```
 *
 * 为什么按事故建子目录：一段事故可能连拍多张（后续可扩展），放进同一目录
 * 便于整段取证拷走。为什么文件名带 sequence：同一毫秒内可能拍两张，只靠
 * 时间戳会覆盖。
 *
 * 写盘使用「先写临时文件再 rename」：rename 在同一文件系统内是原子的，
 * 断电或进程被杀不会留下半张图——而半张 JPEG 会被下游当成真图。
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "edge_snapshot_upload.hpp"
#include "snapshot_encoder.hpp"

namespace weaknet {

/// 取一帧原始 NV12 的能力（由 `VideoFrameHub` 适配而来）
///
/// 为什么抽这个接口而不是直接依赖 `VideoFrameHub`：单测要能在没有 dma-heap
/// 的机器上跑，而 `VideoFrameHub::acquire()` 需要真实分配。用一个只含
/// 「借我一帧」语义的窄接口，测试可以注入一块普通内存。
class ISnapshotFrameSource {
public:
    virtual ~ISnapshotFrameSource() = default;
    /**
     * @brief 取当前最新一帧，必须调用 `release()`
     *
     * @param out 成功时填入可读内存指针与布局事实值
     * @return false 表示当前没有可用帧（未 configure / 从未发布）
     */
    struct Frame {
        const void* data{nullptr};
        size_t size{0};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t stride{0};
        uint32_t uv_offset{0};
        uint64_t sequence{0};
        uint64_t timestamp_ns{0};
        uint32_t slot{0};   ///< 传给 release()
    };
    virtual bool acquire(Frame& out) = 0;
    virtual void release(uint32_t slot) = 0;

    /// 用于 describe()，说明这条通路到底走的是哪条内存后端
    virtual std::string backend() const = 0;
};

class VideoFrameHub;

/**
 * @brief 把 `VideoFrameHub` 适配成 `ISnapshotFrameSource`
 *
 * hub 本身不满足这个接口（它交付的是 fd + slot，不是内存指针），
 * 所以需要一层很薄的适配：acquire 时 mmap，release 时 munmap + close + 归还槽位。
 *
 * @param hub 生命周期必须长于返回的对象
 */
std::unique_ptr<ISnapshotFrameSource> createHubFrameSource(VideoFrameHub* hub);

struct SnapshotServiceConfig {
    /// 落盘根目录。空表示**只编不落盘**（用于纯验证）。
    std::string output_dir;

    /// 待办队列上限。事故高峰时宁可丢，也不能让内存无界增长。
    size_t max_pending{8};

    /// 启动时是否要求硬件后端。false 用于「链路验证」场景。
    bool prefer_hardware{true};

    /// `output_dir` 的总字节上限。0 = 不回收（默认）。
    ///
    /// 为什么默认关闭，而云端保留策略默认也是「只报告不删除」：这条路径删的是
    /// **本机上唯一的那份证据**（板子离线时云端还没有）。一个没人复核过的
    /// 数字不该开始删它。生产启用时由 `server.cpp` 按配置打开。
    ///
    /// 为什么是「总字节」而不是「保留天数」：板子没有 RTC，重启后墙钟可能是
    /// 1970 或任何值，而文件名用的是墙钟毫秒。按天数判会让时钟跳错的板子把
    /// 所有图一次性判成过期。详见 `snapshot_retention.hpp`。
    uint64_t max_total_bytes{0};

    /// 单次回收最多删多少个文件（防止上限误配成 1 字节时一次扫空）。
    size_t max_prune_files_per_run{32};
};
/**
 * @brief 单个事故的拍证进度
 *
 * 为什么不能只用「是否拍过」：失败要允许重试（事故刚触发时帧源可能还没
 * 发布），但必须有界（硬件坏了不能无限敲）。为什么不能只用「试了几次」：
 * 拍成功了就该永远挡住，而不是等到试满上限才挡——那样同一次事故还会被
 * 多拍几张。两个标志各管一件事。
 */
struct SnapshotAttempt {
    int tries{0};     ///< 已尝试次数（含成功那次）
    bool done{false}; ///< 是否已经拍到并留存
};

/**
 * @brief 记忆表触顶时的裁剪（供单测直接验证）
 *
 * 这段逻辑**无法靠阅读证明正确**，而它错了不会报错：表无界增长会缓慢吃掉
 * 内存，误删正在处理的那条会让失败的图无限重试。所以把它拿出来单测，
 * 而不是埋在 `run()` 里靠人眼看。
 *
 * 三条不变量：
 *   1. 裁完不超过 `cap`；
 *   2. `current` 必须留在表里且 `tries` 不丢——它正被处理；
 *   3. `done=true` 的记录优先保留（它们才是去重的依据）。
 *
 * @param attempts 记忆表（就地修改）
 * @param current  当前正在处理的事故 ID（可以为空，表示无在途）
 * @param cap      条目上限
 */
void snapshotPruneAttempts(std::map<std::string, SnapshotAttempt>& attempts,
                           const std::string& current, size_t cap);

/**
 * @brief 快照服务
 *
 * 生命周期：`start()` → 若干 `request()` → `stop()`。
 * 未 `start()` 时 `request()` 是空操作（并计入 dropped），不会崩。
 */
class SnapshotService {
public:
    SnapshotService(std::unique_ptr<ISnapshotFrameSource> frames,
                    std::unique_ptr<ISnapshotEncoder> encoder,
                    SnapshotServiceConfig config);
    ~SnapshotService();

    SnapshotService(const SnapshotService&) = delete;
    SnapshotService& operator=(const SnapshotService&) = delete;

    /// 起工作线程。返回 false 表示线程不可用（非致命，能力退化为不拍）。
    bool start();
    void stop();

    /**
     * @brief 接上云端上行（可选）
     *
     * 非拥有指针：生命周期必须覆盖本对象。生产路径由 `server.cpp` 保证——
     * 停机序列里静帧服务**先于** exporter 停止并销毁（见 stop 序列注释），
     * 所以工作线程读这个指针时它一定还活着。
     *
     * 为什么用 setter 而不是构造参数：现有 10 多项单测都用三参数构造，而
     * 它们验的是「落盘是否原子」「租约是否归还」——与上传毫无关系。把上传
     * 塞进构造函数会迫使每一处测试都去构造一个假上传器，那种噪音会掩盖
     * 真正要断言的东西。
     *
     * 未设置时行为完全不变（只落盘），因此这是一个**纯增量**能力。
     */
    void setUploader(ISnapshotUploader* uploader) { m_uploader = uploader; }

    /**
     * @brief 请求为某个事故拍一张
     *
     * **不阻塞**。同一 incident_id 在队列里已有时不重复入队（避免事故巡检
     * 每 2 秒轮询一次就拍一张，那会变成「每 2 秒写一张 600KB 的图」）。
     *
     * @return true 已入队；false 未入队（未启动 / 队列满 / 重复）
     */
    bool request(const std::string& incident_id);

    struct Stats {
        uint64_t requested{0};   ///< request() 被调用次数
        uint64_t queued{0};      ///< 真正入队的次数
        uint64_t duplicate{0};   ///< 因同 incident 已在队列而跳过
        uint64_t dropped_full{0};///< 队列满丢弃
        uint64_t captured{0};    ///< 成功编码
        uint64_t written{0};     ///< 成功落盘
        uint64_t failed{0};      ///< 编码或写盘失败
        uint64_t no_frame{0};    ///< 取不到帧
        /// 交给上传器的张数（不等于送达数——送达与否由上传器自己计数）。
        /// 两者必须分开：「发出去了」与「送到了」混成一个数，会让上行链路
        /// 故障表现为「静帧还在拍」，而那正是最需要它工作的时候。
        uint64_t uploaded_handoff{0};
        /// 被回收策略删掉的张数（磁盘容量回收，不是上传失败）。
        /// 与 `failed` 分开：混在一起会让「磁盘满了在删旧图」看起来像
        /// 「拍照在失败」，排查方向完全相反。
        uint64_t pruned{0};
        /// 回收删掉的字节数
        uint64_t pruned_bytes{0};
        /// 回收尝试过的次数（含无事可做的空转）
        uint64_t prune_runs{0};
        /// 回收后仍超上限的次数。持续增长说明上限配得比单张还小，
        /// 或磁盘已经装不下当前证据量了。
        uint64_t prune_still_over{0};
    };
    Stats stats() const;

    /// 人类可读状态（写进日志与证据）
    std::string describe() const;

private:
    void run();
    bool handleOne(const std::string& incident_id);

    /**
     * @brief 容量回收：删最旧的事故图，直到总字节回到上限内
     *
     * 只从工作线程调用（`handleOne` 里），因此不需要额外加锁——目录操作与
     * `m_pending` / `m_attempts` 无关，而文件本身只有本线程写。
     *
     * @param just_written 刚写成功的文件路径，**一定不会被删**
     */
    void pruneSnapshots(const std::string& just_written);

    std::unique_ptr<ISnapshotFrameSource> m_frames;
    std::unique_ptr<ISnapshotEncoder> m_encoder;
    SnapshotServiceConfig m_config;

    /// 上行通道（可为空）。见 setUploader() 的所有权说明。
    ISnapshotUploader* m_uploader{nullptr};

    /// 每张图的幂等键。与片段 `makeClipId` 同形，但**在这里生成一次**并
    /// 随上传请求一起走：中途重新生成会让每次重试都变成一张新图，
    /// 云端按 image_id 判幂等，于是同一现场会存下若干张重复图。
    std::string makeImageId(uint64_t wall_ms, uint64_t sequence) const;

    mutable std::mutex m_mutex;
    std::deque<std::string> m_pending;

    /// 每个事故的拍证进度（去重与重试上限的依据），见 `SnapshotAttempt`
    std::map<std::string, SnapshotAttempt> m_attempts;
    Stats m_stats{};

    std::thread m_thread;
    bool m_running{false};
    std::atomic<bool> m_stop{false};

    // 串行化的状态文本，避免 describe() 里反复拼字符串
    std::string m_last_error;

public:
    /// 编码后的张数与字节数（供日志/证据对账）
    struct OutputStats {
        uint64_t images{0};
        uint64_t bytes{0};
        uint64_t over_chunk_limit{0};  ///< 超过云端单分片上限的张数
    };
    OutputStats outputStats() const;

private:
    OutputStats m_out{};
};

}  // namespace weaknet
