/**
 * @file edge_snapshot_upload.hpp
 * @brief 「把一张已编码的静帧送到云端」的窄接口
 *
 * ============================================================================
 * 为什么抽一个只有单个方法的接口，而不是让 SnapshotService 认识 exporter
 * ============================================================================
 *
 * `SnapshotService` 的职责是「按需取一帧 → 编码 → 原子落盘」，它与云端、
 * 凭据、重试、HTTP 全无关系。如果它直接 include `video_clip_exporter.hpp`，
 * 就同时继承了「ed25519 私钥怎么读」「libcurl 有没有编译进来」「退避多少秒」
 * 这一整套实现细节，而这些东西单测里**一个都不该出现**——否则
 * `test_snapshot_service_gtest` 会因为本机没装 OpenSSL 而红，而它要验的是
 * 「落盘是否原子」「租约是否归还」。
 *
 * 所以这里只留一个语义极窄的接口：**给定一份字节和它的自述，尽力送出去**。
 * 实现方（`VideoClipExporter`）负责其余全部细节；单测注入一个记录型替身即可。
 *
 * ============================================================================
 * 为什么不让 SnapshotService 自己另写一条 HTTP 通路
 * ============================================================================
 *
 * 板端已经有一条完整的 edge 上行（Ed25519 签名 + 四个 X-Edge-* 头 +
 * 超时 + 指数退避 + 队列上限）。再写一条会有两套凭据加载、两套超时默认值、
 * 两套重试语义，而它们**不会报错**——只会慢慢漂移，直到某天出现「遥测传得
 * 上去、静帧传不上去」这种谁也说不出原因的状态。
 *
 * 因此上传一律走同一条通路，本接口只是它的一个入口。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace weaknet {

/// 单张静帧裸字节上限（base64 前）。与云端 `MAX_SNAPSHOT_BYTES` **必须同源**。
///
/// 它们的共同来源是「edge 端点 1 MB 原始体 + base64 膨胀 33% + JSON 外壳」，
/// 与 `VIDEO_CLIP_MAX_CHUNK_BYTES` 是同一个数。之所以在两边各写一份而不是
/// 互相 include：云端那份在 Python 里，这份在 C++ 里，编译器没法替你校验；
/// 写在一起反而会让人以为它们有同一个定义点。真值由两侧的单测各自钉住。
constexpr size_t SNAPSHOT_MAX_BYTES = 640 * 1024;

/**
 * @brief 一张待上传的静帧自述
 *
 * 全部字段都是**事实**：几何来自驱动回报的帧布局，`captured_at_ms` 是编码
 * 成功那一刻的墙钟，`incident_id` 由事故巡检线程给出。本结构不推导任何一项。
 *
 * `bytes` 是**内存里的那一份**，不是文件路径。落盘与上传共享同一份字节，
 * 而不是让上传端按路径重读一遍——后者会在文件被清理/改名的瞬间静默失败，
 * 而失败时刻恰好是事后最难复盘的时刻。
 */
struct EdgeSnapshotUpload {
    /// 幂等键。必须跨重试稳定；云端按它判 duplicate / 409。
    std::string image_id;

    /// 关联事故。空串 = 「没有关联事故」。
    std::string incident_id;

    uint32_t width{0};
    uint32_t height{0};

    /// 拍摄时刻（墙钟毫秒 UTC epoch）。是**时刻**不是窗口。
    uint64_t captured_at_ms{0};

    /// JPEG 字节（move 语义由实现方决定，接口按值收下）。
    std::vector<uint8_t> bytes;

    bool valid() const {
        return !image_id.empty() && width > 0 && height > 0 && !bytes.empty();
    }
};

/**
 * @brief 静帧上行入口
 *
 * 实现必须满足两条与 `VideoClipExporter::enqueue` 相同的纪律：
 *
 *   - **不阻塞**：调用方是快照工作线程，它刚做完 10 ms 的 VE 编码，不能被
 *     一次跨公网 RTT 拖住（否则后续事故的请求会在队列里排长队）。
 *   - **拒绝可观测**：返回 false 必须同时让某个计数器涨，绝不静默丢弃。
 */
class ISnapshotUploader {
public:
    virtual ~ISnapshotUploader() = default;

    /// @return false 表示未接收（未启动 / 队列满 / 自述不合法 / 超出上限）
    virtual bool enqueueSnapshot(EdgeSnapshotUpload shot) = 0;
};

}  // namespace weaknet
