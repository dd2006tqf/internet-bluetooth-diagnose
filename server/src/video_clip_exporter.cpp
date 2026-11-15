/**
 * @file video_clip_exporter.cpp
 * @brief 视频留证上行器实现
 *
 * 关键实现点（与头文件约束一一对应）：
 *   1. buildManifestBody() / buildChunkBody() 是**唯一**序列化入口，
 *      重试复用同一段字节（云端按内容判幂等，重算即有被判冲突的风险）；
 *   2. 清单先于分片；分片 index 从 0 连续到 expected_chunks-1；
 *   3. 只在 HTTP 2xx 后推进进度，失败原地重试（指数退避，带上限）；
 *   4. 队列满时丢**最旧**并计 dropped——最新片段与当前事故更相关；
 *   5. 分片按字节切，不按 GOP 切（见头文件「分片边界」）。
 */

#include "video_clip_exporter.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

#ifdef WEAKNET_HAVE_CURL
#include <curl/curl.h>
#endif

#include "logger.hpp"
#include "utils/json_escape.hpp"
#include "video_clip_muxer.hpp"

#ifdef WEAKNET_HAVE_TLS
#include <openssl/evp.h>
#include <openssl/pem.h>
#endif

namespace weaknet {

namespace {

constexpr const char* kVideoClipsSchemaVersion = "network.edge.video-clips.v1";

// 重试轮次与退避参数已移入 `RetryPolicy`（见头文件）。编译期常量无法
// 被单测缩短，会导致「验证失败路径」的用例真的等满两分钟退避。

std::string jsonStr(const std::string& raw) {
    return "\"" + weaknet_utils::escapeJsonString(raw) + "\"";
}

#ifdef WEAKNET_HAVE_CURL
size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    static_cast<std::string*>(userdata)->append(ptr, total);
    return total;
}
#endif

/// 指数退避：1s, 2s, 4s … 上限 60s。第 0 次尝试不退避。
///
/// 参数取自实例的重试策略而不是编译期常量：单测需要「不退避、1 次」才能
/// 在毫秒级验完失败路径，而生产需要「8 次、指数退避」扛住断网抖动。
/// 把两者塞进同一个常量会迫使测试真的等满两分钟（实测 123 秒），
/// 那既拖慢 CI，也让失败原因被退避日志淹没。
uint32_t backoffMs(uint32_t attempt, const VideoClipExporter::RetryPolicy& policy) {
    if (attempt == 0) return 0;
    uint32_t ms = policy.base_backoff_ms;
    for (uint32_t i = 1; i < attempt && ms < policy.max_backoff_ms; ++i) {
        ms *= 2;
    }
    return std::min(ms, policy.max_backoff_ms);
}

}  // namespace

// ============================================================================
// 片段录制器
// ============================================================================

VideoClipRecorder::VideoClipRecorder(VideoClipRecorderConfig cfg) : cfg_(std::move(cfg)) {}

void VideoClipRecorder::push(const uint8_t* data, size_t size, bool keyframe,
                            uint64_t mono_ms) {
    if (data == nullptr || size == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // 时间倒流说明调用方混用了时钟源（唯一能产生这种输入的方式）。
    // 静默接受会让窗口裁出空段，还是明确拒绝更好——但**不丢帧**：
    // 丢帧会让录制中的片段出现空洞，而空洞在收齐后是看不出来的。
    if (mono_ms < newest_mono_ms_) {
        LOG_WARNING(weaknet_dbus::LogModule::VIDEO,
                    "视频留证收到时间倒流的帧（mono=" << mono_ms << " newest="
                    << newest_mono_ms_ << "），按原样入缓冲");
    }
    newest_mono_ms_ = std::max(newest_mono_ms_, mono_ms);

    Frame f;
    f.bytes.assign(data, data + size);
    f.keyframe = keyframe;
    f.mono_ms = mono_ms;

    buffered_bytes_ += f.bytes.size();
    frames_.push_back(std::move(f));

    // 裁剪：未录制时缓冲跟着最新帧滚动，只保留回溯窗口。
    uint64_t left = newest_mono_ms_ > cfg_.pre_roll_ms ? newest_mono_ms_ - cfg_.pre_roll_ms : 0;

    // 录制期间左沿必须**冻结**在「触发点 − 回溯窗口」。
    //
    // 这是一个真实踩过的坑：左沿若继续跟着 newest 前进，触发之后每 push 一帧
    // 就往前挪一点，等 post_roll 走完时左沿已经到了 trigger + (post_roll −
    // pre_roll)。生产参数是 pre=30s / post=60s，于是触发 60 秒后回溯内容
    // **全部被裁光**，片段只剩事后画面——事故最关键的前半段在最需要它的时候没了，
    // 而且整个过程中不报任何错，日志里只有一条「上传完成」。
    if (recording_) {
        const uint64_t clip_left = trigger_mono_ms_ > cfg_.pre_roll_ms
                                       ? trigger_mono_ms_ - cfg_.pre_roll_ms
                                       : 0;
        left = std::min(left, clip_left);
    }
    while (!frames_.empty() && frames_.front().mono_ms < left) {
        buffered_bytes_ -= frames_.front().bytes.size();
        frames_.pop_front();
    }

    // 硬上限：超出即放弃整段。留证是偶发行为，而内存是有限的。
    // 放弃而不是继续裁最旧——一旦裁掉的是起始 IDR，产出的片段就不可解码，
    // 那比明确丢弃更糟（云端会收到一个永远播不了的"已完成"片段）。
    //
    // 放弃之后还要把缓冲压回上限内：此时已经没有片段会产出，"保住起始 IDR"
    // 这条约束随之消失，继续留着超限的缓冲只会让**每一次** push 都再超限、
    // 再打一条同样的告警（永久刷屏），而根因是窗口内容本身超过了 max_bytes。
    if (buffered_bytes_ > cfg_.max_bytes) {
        LOG_WARNING(weaknet_dbus::LogModule::VIDEO,
                    "视频留证缓冲超上限（" << buffered_bytes_ << " > "
                    << cfg_.max_bytes << "），放弃当前录制");
        abortLocked();
        const uint64_t target = cfg_.max_bytes / 2;
        while (!frames_.empty() && buffered_bytes_ > target) {
            buffered_bytes_ -= frames_.front().bytes.size();
            frames_.pop_front();
        }
    }
}

bool VideoClipRecorder::startClip(const std::string& incident_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_) return false;

    recording_ = true;
    incident_id_ = incident_id;
    // 以**已入缓冲的最新帧**为触发点，而不是调用时刻：事故是在事件到达
    // 关联器之后才被确认的，用调用时刻会把触发点推到未来，后沿就白等了。
    trigger_mono_ms_ = newest_mono_ms_;
    return true;
}

bool VideoClipRecorder::isRecording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_;
}

std::string VideoClipRecorder::currentIncident() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return incident_id_;
}

std::optional<VideoClipMeta> VideoClipRecorder::poll(uint64_t mono_now_ms,
                                                     uint64_t wall_now_ms,
                                                     std::vector<uint8_t>* out_bytes) {
    if (out_bytes == nullptr) return std::nullopt;
    out_bytes->clear();

    // `mono_now_ms` 是**刻意**不参与判定的（见下）。它留在签名里，是为了
    // 逼调用方在同一处同时采两个时钟——若只传墙钟，函数内部就得自己取
    // 单调钟，那会让「两个时钟是否同时采样」变成一个无法从调用点看出的
    // 隐含假设，而这里的时间窗换算恰恰依赖它们同刻。
    (void)mono_now_ms;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_) return std::nullopt;

    // 后沿以**最新帧**为准而不是 now：编码停掉之后 now 会继续走，
    // 若按 now 判就会在没有任何新内容的情况下"到点产出"，产出一个尾巴空空的片段。
    if (newest_mono_ms_ < trigger_mono_ms_ + cfg_.post_roll_ms) return std::nullopt;

    // 从触发点之前的某个 IDR 起，到触发点之后的某处止。
    //
    // 为什么必须从 IDR 起：H.264 的 P 帧依赖前序参考帧。从一个 P 帧开始
    // 的码流交给解码器只会得到花屏或黑屏，**且不报错**——播放器会说
    // "文件已损坏"，而运维会以为留证本身坏了。
    size_t start = frames_.size();
    for (size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].mono_ms > trigger_mono_ms_) break;
        if (frames_[i].keyframe) start = i;   // 记住**最后一个**早于触发点的 IDR
    }

    if (start >= frames_.size()) {
        // 回溯窗口里没有 IDR。可能 GOP 比回溯窗口还长，也可能刚启动。
        // 这里**不降级成"从最旧帧开始"**：那会产出一个不可解码的片段，
        // 而不可解码的"已完成"片段比明确失败更难排查。
        LOG_WARNING(weaknet_dbus::LogModule::VIDEO,
                    "视频留证窗口内无 IDR（回溯 " << cfg_.pre_roll_ms << "ms），放弃本次录制");
        ++clips_aborted_;
        abortLocked();
        return std::nullopt;
    }

    VideoClipMeta meta;
    meta.device_id = cfg_.device_id;
    meta.site_id = cfg_.site_id;
    meta.gateway_id = cfg_.gateway_id;
    meta.incident_id = incident_id_;
    meta.width = cfg_.width;
    meta.height = cfg_.height;

    // clip_id 只生成一次：重试必须复用它（见头文件「幂等」一节）。
    meta.clip_id = VideoClipExporter::makeClipId(cfg_.device_id, wall_now_ms);

    meta.started_at_ms = wall_now_ms > (newest_mono_ms_ - frames_[start].mono_ms)
                             ? wall_now_ms - (newest_mono_ms_ - frames_[start].mono_ms)
                             : 0;
    meta.ended_at_ms = meta.started_at_ms +
                       (newest_mono_ms_ - frames_[start].mono_ms);

    // 帧数与帧率必须是**实测值**，不能沿用配置里的名义值。
    //
    // 为什么：采集侧是变速的（实测 18.5 fps 上下浮动，且驱动会自己改帧率），
    // 配置里写 25 只是一个请求值。把它当事实写进清单会让云端显示一个
    // 从未发生过的帧率；更要紧的是封装 MP4 时要用「帧数 ÷ 实测跨度」当
    // 容器帧率，用名义值会让整段视频的时间轴按 25/18.5 ≈ 1.35 倍拉长，
    // 而「事发那一刻在第几秒」正是留证回看唯一要回答的问题。
    //
    // 取整用四舍五入而不是截断：18.56 截断成 18 会在 60 秒片段上累计
    // 40 帧的可见偏差。至少 1 帧是契约下限（云端 `fps: ge=1`）。
    const uint64_t frame_count = frames_.size() - start;
    const uint64_t span_ms = newest_mono_ms_ - frames_[start].mono_ms;
    meta.frame_count = static_cast<uint32_t>(frame_count);
    if (frame_count >= 2 && span_ms > 0) {
        // fps = 帧数 / (跨度ms/1000) = 帧数×1000 / 跨度ms。
        // 先乘后除并加半个除数，得到「四舍五入到整数」的结果，全程整数运算
        // ——浮点在这里没有任何好处，只会让同一段码流在不同 CPU 上可能差 1。
        const uint64_t fps_rounded = (frame_count * 1000ull + span_ms / 2) / span_ms;
        meta.fps = static_cast<uint32_t>(fps_rounded == 0 ? 1 : fps_rounded);
    }

    for (size_t i = start; i < frames_.size(); ++i) {
        out_bytes->insert(out_bytes->end(), frames_[i].bytes.begin(),
                          frames_[i].bytes.end());
    }

    ++clips_produced_;
    abortLocked();
    return meta;
}

void VideoClipRecorder::abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    abortLocked();
}

void VideoClipRecorder::abortLocked() {
    recording_ = false;
    incident_id_.clear();
    trigger_mono_ms_ = 0;
    // 刻意**不清空** frames_：预录窗口在片段产出后仍要继续攒着，
    // 否则下一次事故会没有回溯内容可录。
}

size_t VideoClipRecorder::bufferedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffered_bytes_;
}

size_t VideoClipRecorder::bufferedFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
}

uint64_t VideoClipRecorder::clipsProduced() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clips_produced_;
}

uint64_t VideoClipRecorder::clipsAborted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clips_aborted_;
}

VideoClipExporter::VideoClipExporter(const weaknet_dbus::WeakNetConfig& config,
                                     std::string telemetry_url)
    : config_(config), telemetry_url_(std::move(telemetry_url)) {}

VideoClipExporter::~VideoClipExporter() {
    stop();
}

// ============================================================================
// URL 派生
// ============================================================================

std::string VideoClipExporter::deriveClipsUrl(const std::string& telemetry_url) {
    // 与 EdgeWirelessUplinkExporter::uplinkUrl() 同一形态：只接受以
    // /edge/telemetry 结尾的形态，其余一律返回空串。猜一个地址意味着
    // 把带签名的留证发到未知端点，那比失败糟得多。
    const std::string suffix = "/edge/telemetry";
    if (telemetry_url.size() < suffix.size()) return {};
    if (telemetry_url.compare(telemetry_url.size() - suffix.size(), suffix.size(),
                              suffix) != 0) {
        return {};
    }
    return telemetry_url.substr(0, telemetry_url.size() - suffix.size()) +
           "/edge/video-clips";
}

std::string VideoClipExporter::deriveChunkUrl(const std::string& telemetry_url,
                                              const std::string& clip_id,
                                              size_t chunk_index) {
    const std::string base = deriveClipsUrl(telemetry_url);
    if (base.empty() || clip_id.empty()) return {};

    // clip_id 会被拼进 URL 路径。云端契约已经把它限制成
    // `^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$`，这里再守一次：即使调用方
    // 传了不合规的值，也不能让 `/` 或 `?` 把路径改写成另一个端点。
    for (char c : clip_id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return {};
    }

    return base + "/" + clip_id + "/chunks/" + std::to_string(chunk_index);
}

std::string VideoClipExporter::makeClipId(const std::string& device_id,
                                          uint64_t epoch_ms) {
    // 云端正则要求首字符是字母数字，所以把 device_id 里的非合规字符压成 '-'，
    // 并去掉首部的非合规字符。device_id 本身通常就合规，这里只是兜底。
    std::string prefix;
    prefix.reserve(device_id.size());
    for (char c : device_id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        prefix.push_back(ok ? c : '-');
    }
    while (!prefix.empty() && !std::isalnum(static_cast<unsigned char>(prefix.front()))) {
        prefix.erase(prefix.begin());
    }
    if (prefix.empty()) prefix = "clip";

    // 长度预算：总上限 128。留出 "-<epoch_ms>-<rand6>" 约 20 字符。
    constexpr size_t kMaxPrefix = 96;
    if (prefix.size() > kMaxPrefix) prefix.resize(kMaxPrefix);

    // 随机后缀用 random_device 播种的 mt19937：不需要密码学强度，只需要
    // 同一毫秒内两次调用不碰撞。用 steady_clock 计数做熵源反而更差
    // （同一毫秒内它不变）。
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const uint64_t r = rng() % 1000000ULL;

    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "%06llu",
                  static_cast<unsigned long long>(r));
    return prefix + "-" + std::to_string(epoch_ms) + "-" + suffix;
}

// ============================================================================
// 编解码原语
// ============================================================================

std::string VideoClipExporter::sha256Hex(const uint8_t* data, size_t size) {
#ifdef WEAKNET_HAVE_TLS
    if (!data && size != 0) return {};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    std::string hex;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, data, size) == 1 &&
        EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1) {
        static const char* kHex = "0123456789abcdef";
        hex.reserve(digest_len * 2);
        for (unsigned int i = 0; i < digest_len; ++i) {
            hex.push_back(kHex[(digest[i] >> 4) & 0x0F]);
            hex.push_back(kHex[digest[i] & 0x0F]);
        }
    }
    EVP_MD_CTX_free(ctx);
    return hex;
#else
    (void)data;
    (void)size;
    return {};
#endif
}

std::string VideoClipExporter::base64Encode(const uint8_t* data, size_t size) {
    if (size == 0) return {};
    if (!data) return {};

    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve((size + 2) / 3 * 4);

    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
    }

    // 尾部：1 或 2 字节，**必须带 padding**。云端用 `validate=True` 解码，
    // 缺 padding 会直接 422 `edge_video_clip_payload_not_base64`。
    if (i < size) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        const bool two = (i + 1 < size);
        if (two) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(two ? kAlphabet[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

size_t VideoClipExporter::planChunkCount(size_t total_bytes) {
    if (total_bytes == 0) return 0;
    return (total_bytes + VIDEO_CLIP_MAX_CHUNK_BYTES - 1) / VIDEO_CLIP_MAX_CHUNK_BYTES;
}

// ============================================================================
// 报文构造（对外契约）
// ============================================================================

std::string VideoClipExporter::buildManifestBody(const VideoClipMeta& meta,
                                                 size_t total_bytes,
                                                 size_t expected_chunks,
                                                 const std::string& content_sha256_hex,
                                                 std::string* error) {
    if (!meta.valid()) {
        if (error) *error = "clip meta 不合法（clip_id/device_id/几何/时间窗）";
        return {};
    }
    if (total_bytes > VIDEO_CLIP_MAX_BYTES) {
        if (error) *error = "片段字节数超过上限";
        return {};
    }
    if (expected_chunks > 0 && planChunkCount(total_bytes) != expected_chunks) {
        if (error) *error = "expected_chunks 与实际分片规划不一致";
        return {};
    }
    if (expected_chunks == 0 && total_bytes != 0) {
        if (error) *error = "无分片的片段不能带字节";
        return {};
    }
    if (!content_sha256_hex.empty() && content_sha256_hex.size() != 64) {
        if (error) *error = "content sha256 不是 64 位 hex";
        return {};
    }

    // 字段顺序固定且与云端 `VideoClipManifest` 逐字对应。云端是
    // `extra="forbid"`，多一个字段就 422；少一个必填字段同样 422。
    std::ostringstream oss;
    oss << "{"
        << "\"schema_version\":" << jsonStr(kVideoClipsSchemaVersion)
        << ",\"clip_id\":" << jsonStr(meta.clip_id)
        << ",\"device_id\":" << jsonStr(meta.device_id)
        << ",\"site_id\":" << jsonStr(meta.site_id)
        << ",\"gateway_id\":" << jsonStr(meta.gateway_id)
        << ",\"incident_id\":" << jsonStr(meta.incident_id)
        << ",\"codec\":\"H264\""
        << ",\"width\":" << meta.width
        << ",\"height\":" << meta.height
        << ",\"fps\":" << meta.fps
        << ",\"started_at_ms\":" << meta.started_at_ms
        << ",\"ended_at_ms\":" << meta.ended_at_ms
        << ",\"total_bytes\":" << total_bytes
        << ",\"expected_chunks\":" << expected_chunks
        << ",\"declared_content_sha256\":" << jsonStr(content_sha256_hex)
        << "}";
    return oss.str();
}

std::string VideoClipExporter::buildChunkBody(const std::string& device_id,
                                              const std::string& clip_id,
                                              size_t chunk_index,
                                              const uint8_t* payload,
                                              size_t payload_size,
                                              std::string* error) {
    if (device_id.empty() || clip_id.empty()) {
        if (error) *error = "device_id / clip_id 为空";
        return {};
    }
    if (payload_size > VIDEO_CLIP_MAX_CHUNK_BYTES) {
        if (error) *error = "分片超过单片上限";
        return {};
    }
    if (payload_size > 0 && !payload) {
        if (error) *error = "payload 为空指针但声明了长度";
        return {};
    }

    const std::string digest = sha256Hex(payload, payload_size);
    if (digest.empty()) {
        if (error) *error = "sha256 计算失败（本构建未编译 OpenSSL？）";
        return {};
    }
    const std::string payload_b64 = base64Encode(payload, payload_size);

    std::ostringstream oss;
    oss << "{"
        << "\"schema_version\":" << jsonStr(kVideoClipsSchemaVersion)
        << ",\"device_id\":" << jsonStr(device_id)
        << ",\"clip_id\":" << jsonStr(clip_id)
        << ",\"chunk_index\":" << chunk_index
        << ",\"size_bytes\":" << payload_size
        << ",\"content_sha256\":" << jsonStr(digest)
        << ",\"payload_b64\":" << jsonStr(payload_b64)
        << "}";
    return oss.str();
}

// ============================================================================
// 签名与发送
// ============================================================================

std::string VideoClipExporter::signBody(const std::string& body,
                                        std::string* error) const {
#ifdef WEAKNET_HAVE_TLS
    const std::string key_path = config_.edge.private_key_path.get();
    if (key_path.empty()) {
        if (error) *error = "edge.private_key_path 未配置";
        return {};
    }

    FILE* fp = std::fopen(key_path.c_str(), "r");
    if (!fp) {
        if (error) *error = "私钥不可读: " + key_path;
        return {};
    }
    EVP_PKEY* key = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!key) {
        if (error) *error = "私钥解析失败";
        return {};
    }
    if (EVP_PKEY_id(key) != EVP_PKEY_ED25519) {
        EVP_PKEY_free(key);
        if (error) *error = "私钥不是 Ed25519";
        return {};
    }

    std::string signature_hex;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx) {
        EVP_PKEY_CTX* pctx = nullptr;
        size_t sig_len = 0;
        if (EVP_DigestSignInit(ctx, &pctx, nullptr, nullptr, key) == 1 &&
            EVP_DigestSign(ctx, nullptr, &sig_len,
                           reinterpret_cast<const unsigned char*>(body.data()),
                           body.size()) == 1) {
            std::vector<unsigned char> sig(sig_len);
            if (EVP_DigestSign(ctx, sig.data(), &sig_len,
                               reinterpret_cast<const unsigned char*>(body.data()),
                               body.size()) == 1) {
                static const char* kHex = "0123456789abcdef";
                signature_hex.reserve(sig_len * 2);
                for (size_t i = 0; i < sig_len; ++i) {
                    signature_hex.push_back(kHex[(sig[i] >> 4) & 0x0F]);
                    signature_hex.push_back(kHex[sig[i] & 0x0F]);
                }
            }
        }
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(key);
    if (signature_hex.empty() && error) *error = "签名失败";
    return signature_hex;
#else
    (void)body;
    if (error) *error = "本构建未编译 OpenSSL，无法签名";
    return {};
#endif
}

bool VideoClipExporter::postSigned(const std::string& url, const std::string& body,
                                   const std::string& signature, std::string* response,
                                   std::string* error) const {
#ifdef WEAKNET_HAVE_CURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error) *error = "curl_easy_init 失败";
        return false;
    }
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string key_header = "X-Edge-Key-Id: " + config_.edge.key_id.get();
    const std::string token_header = "X-Edge-Token: " + config_.edge.token.get();
    const std::string tenant_header = "X-Edge-Tenant: " + config_.edge.tenant.get();
    const std::string sig_header = "X-Edge-Signature: " + signature;
    headers = curl_slist_append(headers, key_header.c_str());
    headers = curl_slist_append(headers, token_header.c_str());
    headers = curl_slist_append(headers, tenant_header.c_str());
    headers = curl_slist_append(headers, sig_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(config_.edge.timeout_ms.load()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(config_.edge.timeout_ms.load()));

    const CURLcode code = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK || http_status < 200 || http_status >= 300) {
        if (error) {
            *error = std::string("上行失败: ") + curl_easy_strerror(code) +
                     " http=" + std::to_string(http_status);
        }
        return false;
    }
    return true;
#else
    (void)url;
    (void)body;
    (void)response;
    if (error) *error = "本构建未链接 libcurl，无法发送";
    return false;
#endif
}

// ============================================================================
// 生命周期
// ============================================================================

bool VideoClipExporter::start() {
    if (running_.load()) return true;
    if (!config_.edge.enabled.load()) {
        LOG_INFO(weaknet_dbus::LogModule::VIDEO,
                 "视频留证上行未启用（edge.enabled=false）");
        return false;
    }
    if (config_.edge.device_id.get().empty() ||
        config_.edge.private_key_path.get().empty() ||
        config_.edge.key_id.get().empty() ||
        config_.edge.token.get().empty() ||
        config_.edge.tenant.get().empty()) {
        LOG_ERROR(weaknet_dbus::LogModule::VIDEO,
                  "视频留证上行配置不完整，已保持关闭（device_id/token/key_id/"
                  "tenant/private_key_path 均必填）");
        return false;
    }
    if (deriveClipsUrl(telemetry_url_).empty()) {
        LOG_ERROR(weaknet_dbus::LogModule::VIDEO,
                  "无法由 edge.url 派生 video-clips 端点: " << telemetry_url_);
        return false;
    }

    stop_requested_.store(false);
    running_.store(true);
    thread_ = std::thread([this]() { run(); });
    LOG_INFO(weaknet_dbus::LogModule::VIDEO,
             "视频留证上行已启动: clips_url=" << deriveClipsUrl(telemetry_url_));
    return true;
}

void VideoClipExporter::stop() {
    if (!running_.load() && !thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    LOG_INFO(weaknet_dbus::LogModule::VIDEO, "视频留证上行已停止");
}

// ============================================================================
// 队列
// ============================================================================

bool VideoClipExporter::enqueue(const VideoClipMeta& meta,
                                std::vector<uint8_t> codec_bytes) {
    // 三条早退路径都必须计入 `clips_dropped`。
    //
    // 头文件把「绝不静默丢弃」写成约定，而 `clips_dropped` 是运维唯一能看见
    // 这件事的地方：如果只有队列满才计数，那么「meta 不合法」与「码流超上限」
    // 这两类**永远不可能成功**的输入，在监控上会表现为「入队总是成功但什么
    // 都没传出去」。归因时它们与「队列压力」必须区分，但首先要都可见。
    auto reject = [this](std::string error) {
        LOG_WARNING(weaknet_dbus::LogModule::VIDEO, "视频留证入队被拒：" << error);
        std::lock_guard<std::mutex> sl(stats_mutex_);
        ++stats_.clips_dropped;
        return false;
    };

    if (!meta.valid()) {
        return reject("meta 不合法 clip_id=" + meta.clip_id);
    }
    if (codec_bytes.empty()) {
        return reject("码流为空 clip_id=" + meta.clip_id);
    }
    if (codec_bytes.size() > VIDEO_CLIP_MAX_BYTES) {
        return reject("超过单片段上限 " + std::to_string(codec_bytes.size()));
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (stop_requested_) return false;

    // 队列满时丢**最旧**的片段。
    //
    // 为什么不是丢最新的（reject new）：最新片段与**当前**事故更相关，
    // 而排在前面的片段很可能已经被上一轮重试消耗掉多轮退避，继续留着
    // 只会拖住后面的。丢旧同时把内存让给新片段。
    //
    // 为什么不是阻塞等待：编码线程绝不能被上传线程拖住（中枢租约会堆积，
    // 面板随之卡顿）。丢帧是可观测的降级，阻塞是不可观测的故障。
    while (queued_bytes_ + codec_bytes.size() > max_queue_bytes_ && !queue_.empty()) {
        PendingClip& victim = queue_.front();
        queued_bytes_ -= victim.bytes.size();
        LOG_WARNING(weaknet_dbus::LogModule::VIDEO,
                    "视频留证队列满，丢弃最旧片段 clip_id=" << victim.meta.clip_id
                    << " bytes=" << victim.bytes.size());
        queue_.pop_front();
        std::lock_guard<std::mutex> sl(stats_mutex_);
        ++stats_.clips_dropped;
    }

    if (queued_bytes_ + codec_bytes.size() > max_queue_bytes_) {
        // 单个片段本身就超过队列上限：接受它会挤掉所有其它片段。
        lock.unlock();
        LOG_WARNING(weaknet_dbus::LogModule::VIDEO,
                    "视频留证队列上限小于单片段体积，拒绝入队 clip_id="
                    << meta.clip_id << " bytes=" << codec_bytes.size()
                    << " limit=" << max_queue_bytes_);
        std::lock_guard<std::mutex> sl(stats_mutex_);
        ++stats_.clips_dropped;
        return false;
    }

    queued_bytes_ += codec_bytes.size();
    queue_.push_back(PendingClip{meta, std::move(codec_bytes), 0});
    lock.unlock();
    cv_.notify_one();
    return true;
}

size_t VideoClipExporter::queueDepth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

VideoClipExporterStats VideoClipExporter::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

// ============================================================================
// 上传
// ============================================================================

bool VideoClipExporter::uploadManifestOnce(const PendingClip& clip,
                                           size_t total_bytes,
                                           size_t expected_chunks,
                                           const std::string& content_sha,
                                           std::string* error) {
    const std::string body = buildManifestBody(clip.meta, total_bytes,
                                               expected_chunks, content_sha, error);
    if (body.empty()) return false;

    const std::string sig = signBody(body, error);
    if (sig.empty()) return false;

    const std::string url = deriveClipsUrl(telemetry_url_);
    if (url.empty()) {
        if (error) *error = "video-clips URL 无法派生";
        return false;
    }

    std::string response;
    if (!postSigned(url, body, sig, &response, error)) return false;

    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.manifests_sent;
    return true;
}

bool VideoClipExporter::uploadChunkOnce(const PendingClip& clip, size_t index,
                                        std::string* error) {
    const size_t offset = index * VIDEO_CLIP_MAX_CHUNK_BYTES;
    if (offset >= clip.bytes.size()) {
        if (error) *error = "分片索引越界";
        return false;
    }
    const size_t len =
        std::min(VIDEO_CLIP_MAX_CHUNK_BYTES, clip.bytes.size() - offset);

    const std::string body = buildChunkBody(
        clip.meta.device_id, clip.meta.clip_id, index,
        clip.bytes.data() + offset, len, error);
    if (body.empty()) return false;

    const std::string sig = signBody(body, error);
    if (sig.empty()) return false;

    const std::string url = deriveChunkUrl(telemetry_url_, clip.meta.clip_id, index);
    if (url.empty()) {
        if (error) *error = "分片 URL 无法派生";
        return false;
    }

    std::string response;
    if (!postSigned(url, body, sig, &response, error)) return false;

    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.chunks_sent;
    stats_.bytes_sent += len;
    return true;
}

bool VideoClipExporter::runOnce(std::string* error) {
    PendingClip clip;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            if (error) *error = "队列为空";
            return false;
        }
        clip = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= clip.bytes.size();
    }

    // ---- 封装 MP4（在分片之前，因为 sha256 与 total_bytes 都要按最终字节算）----
    //
    // 为什么放在这里而不是编码线程：一次封装实测约 0.26 s（570 帧 / 90 KB）。
    // 放在编码线程里，这 0.26 s 内中枢租约会堆积、采集帧被丢弃；而编码线程的
    // 纪律是「绝不被下游拖住」（见 enqueue() 里同一理由）。上传线程本来就在
    // 等网络，0.26 s 的本地 CPU 对它没有影响。
    //
    // 为什么失败不丢证据：封装是**交付形态**的增强，不是留证本身。ffmpeg 缺失
    // 或码流不合规时，裸 Annex-B 依然是有价值的事实——云端历史片段本来就是
    // 裸码流，播放路径已经能处理。放弃上传等于让一次真实事故的现场永久消失，
    // 那个代价远大于「这次没有 mp4 容器」。
    //
    // 必须重算 total_bytes 与 content_sha256：它们在下面用于**云端幂等**
    // （同 clip_id 异内容 = 409）。若封装后仍按原始码流算，云端会判成冲突，
    // 而本地日志只会看见「清单上传失败 409」，完全看不出是字节换了。
    if (!clip.bytes.empty() && !looksLikeMp4(clip.bytes.data(), clip.bytes.size())) {
        const uint32_t span_ms =
            clip.meta.ended_at_ms > clip.meta.started_at_ms
                ? static_cast<uint32_t>(clip.meta.ended_at_ms - clip.meta.started_at_ms)
                : 0;
        VideoClipMuxResult muxed =
            muxAnnexBToMp4(clip.bytes.data(), clip.bytes.size(),
                           clip.meta.frame_count, span_ms, ffmpeg_path_);
        if (muxed.ok) {
            clip.bytes = std::move(muxed.bytes);
        } else {
            // 只警告不失败。错误信息里带 ffmpeg 的 stderr 摘要，让「没装
            // ffmpeg」与「码流不合规」在日志里可区分——两者处置完全不同。
            LOG_WARNING(weaknet_dbus::LogModule::VIDEO,
                        "视频留证封装 MP4 失败，改传裸 H.264 clip_id="
                            << clip.meta.clip_id << " bytes=" << clip.bytes.size()
                            << " frame_count=" << clip.meta.frame_count << "："
                            << muxed.error);
            std::lock_guard<std::mutex> sl(stats_mutex_);
            ++stats_.mux_failures;
        }
    }

    const size_t total_bytes = clip.bytes.size();
    const size_t expected_chunks = planChunkCount(total_bytes);
    const std::string content_sha = sha256Hex(clip.bytes.data(), total_bytes);
    if (content_sha.empty()) {
        if (error) *error = "整段 sha256 计算失败";
        std::lock_guard<std::mutex> sl(stats_mutex_);
        ++stats_.clips_dropped;
        return false;
    }

    LOG_INFO(weaknet_dbus::LogModule::VIDEO,
             "视频留证开始上传 clip_id=" << clip.meta.clip_id
             << " bytes=" << total_bytes << " chunks=" << expected_chunks
             << " sha=" << content_sha.substr(0, 16) << "...");

    // 清单先于分片。顺序不可颠倒：云端对未登记片段的
    // 分片直接 404 `edge_video_clip_not_registered`。
    bool manifest_ok = false;
    std::string manifest_error;
    for (uint32_t attempt = 0; attempt < retry_policy_.max_attempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs(attempt, retry_policy_)));
        }
        if (uploadManifestOnce(clip, total_bytes, expected_chunks, content_sha,
                               &manifest_error)) {
            manifest_ok = true;
            break;
        }
        {
            std::lock_guard<std::mutex> sl(stats_mutex_);
            ++stats_.send_failures;
        }
        LOG_WARNING(weaknet_dbus::LogModule::VIDEO,
                    "清单上传失败（第 " << (attempt + 1) << " 次）clip_id="
                    << clip.meta.clip_id << ": " << manifest_error);
        if (stop_requested_.load()) break;
    }

    if (!manifest_ok) {
        if (error) *error = "清单上传重试耗尽: " + manifest_error;
        std::lock_guard<std::mutex> sl(stats_mutex_);
        ++stats_.clips_dropped;
        return false;
    }

    // 分片：任一片在重试耗尽后仍失败，整个片段放弃。
    //
    // 为什么不跳过坏片继续传后面的：云端按 (clip_id, index) 收，缺一片
    // 就永远是 PENDING，而「半个片段」在运营端是**看不见**的——列表里
    // 它显示为未完成，既不报错也不会自动清理。与其留一个永久卡住的行，
    // 不如让整段失败并计入 clips_dropped，让日志说清楚。
    for (size_t index = 0; index < expected_chunks; ++index) {
        bool chunk_ok = false;
        std::string chunk_error;
        for (uint32_t attempt = 0; attempt < retry_policy_.max_attempts; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(backoffMs(attempt, retry_policy_)));
            }
            if (uploadChunkOnce(clip, index, &chunk_error)) {
                chunk_ok = true;
                break;
            }
            {
                std::lock_guard<std::mutex> sl(stats_mutex_);
                ++stats_.send_failures;
            }
            LOG_WARNING(weaknet_dbus::LogModule::VIDEO,
                        "分片上传失败（第 " << (attempt + 1) << " 次）clip_id="
                        << clip.meta.clip_id << " index=" << index << ": "
                        << chunk_error);
            if (stop_requested_.load()) break;
        }
        if (!chunk_ok) {
            if (error) {
                *error = "分片 " + std::to_string(index) + " 重试耗尽: " + chunk_error;
            }
            std::lock_guard<std::mutex> sl(stats_mutex_);
            ++stats_.clips_dropped;
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> sl(stats_mutex_);
        ++stats_.clips_uploaded;
    }
    LOG_INFO(weaknet_dbus::LogModule::VIDEO,
             "视频留证上传完成 clip_id=" << clip.meta.clip_id
             << " chunks=" << expected_chunks << " bytes=" << total_bytes);
    return true;
}

void VideoClipExporter::run() {
    while (!stop_requested_.load()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this]() {
                return stop_requested_.load() || !queue_.empty();
            });
        }
        if (stop_requested_.load()) break;

        // 处理到队列空为止。每轮只取一个片段，让 stop() 能在片段之间
        // 及时生效——一次上传可能持续几十秒，不能在整批跑完才看停止标志。
        std::string error;
        if (!runOnce(&error)) {
            if (!queue_.empty() && !stop_requested_.load()) {
                LOG_WARNING(weaknet_dbus::LogModule::VIDEO,
                            "视频留证本轮未完成: " << error);
            }
        }
    }
}

}  // namespace weaknet
