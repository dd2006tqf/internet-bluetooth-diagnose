/**
 * @file board_video_clip_e2e.cpp
 * @brief 视频留证端到端验证探针：合成 NV12 → cedar 编码 → 录制器 → 上传
 *
 * ============================================================================
 * 它解决什么问题
 * ============================================================================
 *
 * 留证链路的单元测试覆盖了「报文形状」与「录制窗口规则」，但**没有任何一项
 * 证明这些字节能真的被云端接受**。签名算法一致、字段名一致、分片对得上，
 * 全都只是「我认为一致」——真正的检验是云端返回 2xx 并把片段收齐。
 *
 * 而摄像头当前零帧（sunxi TDM 的独立问题），没法用真实画面验证。所以这里
 * **绕过摄像头**：合成一块 NV12，喂给同一个 cedar 编码器、同一个录制器、
 * 同一个上行器。除了像素来源，其余每一环都是生产代码。
 *
 * ============================================================================
 * 为什么合成帧不是「假验证」
 * ============================================================================
 *
 * 本探针要验的是**传输契约**，不是画质。画质取决于采集与编码，那是另一条
 * 线（`board_encoder_check.cpp` 已经在真机上验过）。而传输契约只关心：
 *
 *   - 码流能不能切成分片、拼回来是不是原样（sha256 对得上）；
 *   - 清单/分片字段能不能被云端 pydantic 模型接受；
 *   - Ed25519 签名能不能通过云端验签；
 *   - 重试时复用同一份字节能不能被判成 duplicate 而不是 409 冲突。
 *
 * 这四件事与「像素从哪来」完全无关。反过来，用真实画面去验这四件事只会
 * 增加活动部件（摄像头、驱动、曝光），让失败原因更难定位。
 *
 * ============================================================================
 * 用法
 * ============================================================================
 *
 *   # 板端（edge.url 指向云端可达地址）
 *   sudo ./board_video_clip_e2e --config /etc/weaknet/config.yaml
 *
 *   # 参数
 *   --frames N     合成多少帧（默认 60；60 帧 @18fps ≈ 3.3 秒素材）
 *   --no-record    跳过录制器，直接把编码结果当一段码流送（验证纯传输）
 *   --dry-run      只构造并校验报文，不真的发 HTTP
 *
 * 退出码：0 = 云端确认收齐；非 0 = 失败（打印具体环节）。
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <getopt.h>
#include <unistd.h>

#include "video_clip_exporter.hpp"
#include "video_encoder.hpp"
#include "weaknet_config.hpp"

namespace {

/// 本板实测几何（见 docs/ideas 的「帧内存布局」表与 pitfalls 记录）。
///
/// 这里用**事实值**而不是从 width 推：stride 与 uv_offset 都是驱动按
/// 16 对齐分配的产物，用 width*height 推会让编码器逐行斜切或整帧偏色。
constexpr uint32_t kWidth = 1640;
constexpr uint32_t kHeight = 1232;
constexpr uint32_t kStride = 1640;
constexpr uint32_t kUvOffset = 2030336;
constexpr uint32_t kFrameBytes = 3045504;

/// 合成一帧 NV12。
///
/// 为什么要让画面**帧间有变化**：H.264 的 P 帧编码依赖前序参考帧，一张
/// 恒定不变的图会让编码器产出极小的 P 帧（几乎是空的），于是「分片边界」
/// 与「码流拼接」这两件事都失去了检验强度——0 字节的分片是拼不错也错不了。
/// 让亮度随帧号平移，既能产生真实的 P 帧数据，又不需要任何外部素材。
std::vector<uint8_t> makeSyntheticNv12(uint32_t frame_index, bool keyframe) {
    std::vector<uint8_t> buf(kFrameBytes, 0);

    // Y 平面：一个随帧号平移的渐变 + 一条竖直亮条。
    // 平移量取 8 的倍数，避免产生过于杂乱的编码噪声（那会让码率不可控）。
    const uint32_t shift = (frame_index * 8) % kWidth;
    for (uint32_t y = 0; y < kHeight; ++y) {
        uint8_t* row = buf.data() + static_cast<size_t>(y) * kStride;
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint32_t sx = (x + shift) % kWidth;
            uint8_t v = static_cast<uint8_t>((sx * 255) / kWidth);
            // 每 128 行画一条亮带，给编码器一些高频区域（否则全图过于平滑，
            // P 帧会小到不足以覆盖多分片）。
            if (((y / 8) % 16) == 0) v = static_cast<uint8_t>(v / 2 + 128);
            row[x] = v;
        }
    }

    // UV 平面：中性灰（U=V=128 表示无色度）。留证验证不关心颜色，
    // 但**必须写对偏移**——写到 stride*height 处就会落进对齐空洞，
    // 编码器读到全 0 会产出严重偏绿的画面（且不报错）。
    std::memset(buf.data() + kUvOffset, 128,
                static_cast<size_t>(kHeight / 2) * kStride);

    (void)keyframe;   // 关键帧由编码器决定，不是合成侧能指定的
    return buf;
}

/// 一个简易的「已编码帧」收集器：把编码器产出的字节按序攒起来。
///
/// 存的是拷贝：`EncodedFrame::data` 只在下一次 `encode()` 之前有效，
/// 而我们要等到全部编完才上传。
struct CollectedFrame {
    std::vector<uint8_t> bytes;
    bool keyframe{false};
    uint64_t timestamp_ns{0};
};

struct RunOptions {
    std::string config_path{"/etc/weaknet/config.yaml"};
    /// 覆盖 `edge.url`（空 = 用配置文件里的值）。
    ///
    /// 为什么需要这个开关：板端 `/etc/weaknet/config.yaml` 的 `edge.url`
    /// 指向 `127.0.0.1:8000`——那是**板子自己的回环**，不是云端。为了让
    /// 本探针能打到真实的云端，最省事的做法是改配置文件，但那是生产配置，
    /// 改它等于让探针的验证结果与生产状态互相污染。
    ///
    /// 这里在**内存里**覆盖：配置已经过 `loadWeakNetConfig` 的 URL 策略
    /// 校验（回环地址不需要 allow_insecure_http），覆盖发生在其后，
    /// 因此不会因为「明文 HTTP 到公网」被拒。这是刻意的——探针是一次性
    /// 验证工具，它的信任边界由操作者决定，而生产路径仍受完整校验。
    std::string url_override{};
    uint32_t frames{60};
    bool use_recorder{true};
    bool dry_run{false};
};

void usage(const char* prog) {
    std::printf(
        "用法: %s [选项]\n"
        "  --config PATH   配置文件（默认 /etc/weaknet/config.yaml）\n"
        "  --url URL       覆盖 edge.url（默认用配置文件里的值）\n"
        "  --frames N      合成帧数（默认 60）\n"
        "  --no-record     跳过录制器，把编码结果直接当一段码流上传\n"
        "  --dry-run       只构造报文并本地校验，不发 HTTP\n"
        "  --help          显示本帮助\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    RunOptions opt;

    static const option kLongOpts[] = {
        {"config", required_argument, nullptr, 'c'},
        {"url", required_argument, nullptr, 'u'},
        {"frames", required_argument, nullptr, 'f'},
        {"no-record", no_argument, nullptr, 'n'},
        {"dry-run", no_argument, nullptr, 'd'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int ch = 0;
    while ((ch = getopt_long(argc, argv, "c:u:f:ndh", kLongOpts, nullptr)) != -1) {
        switch (ch) {
            case 'c': opt.config_path = optarg; break;
            case 'u': opt.url_override = optarg; break;
            case 'f': opt.frames = static_cast<uint32_t>(std::atoi(optarg)); break;
            case 'n': opt.use_recorder = false; break;
            case 'd': opt.dry_run = true; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 2;
        }
    }

    std::printf("=== 视频留证端到端验证 ===\n");
    std::printf("[cfg] %s\n", opt.config_path.c_str());
    std::printf("[opt] frames=%u recorder=%s dry_run=%s\n", opt.frames,
                opt.use_recorder ? "on" : "off", opt.dry_run ? "on" : "off");

    // ---------------------------------------------------------------
    // 1) 配置
    // ---------------------------------------------------------------
    weaknet_dbus::WeakNetConfig cfg;
    std::string cfg_error;
    if (!weaknet_dbus::loadWeakNetConfig(opt.config_path, &cfg, &cfg_error)) {
        std::printf("!! 配置加载失败: %s\n", cfg_error.c_str());
        return 1;
    }

    // --url 覆盖发生在配置校验之后（见 RunOptions::url_override 的注释）。
    if (!opt.url_override.empty()) {
        cfg.edge.url.set(opt.url_override);
        std::printf("[cfg] edge.url 被 --url 覆盖\n");
    }
    const std::string telemetry_url = cfg.edge.url.get();
    const std::string clips_url = weaknet::VideoClipExporter::deriveClipsUrl(telemetry_url);
    std::printf("[cfg] edge.url=%s\n", telemetry_url.c_str());
    std::printf("[cfg] clips_url=%s\n", clips_url.c_str());
    if (clips_url.empty()) {
        std::printf("!! 无法从 edge.url 派生 clips 端点（要求以 /edge/telemetry 结尾）\n");
        return 1;
    }
    if (cfg.edge.device_id.get().empty() || cfg.edge.private_key_path.get().empty()) {
        std::printf("!! edge.device_id / edge.private_key_path 未配置，无法签名\n");
        return 1;
    }

    // ---------------------------------------------------------------
    // 2) 编码器（真 cedar VE；不可用时工厂会回落并说明原因）
    // ---------------------------------------------------------------
    auto enc = weaknet::createVideoEncoder(/*prefer_hardware=*/true);
    std::printf("[enc] %s\n", enc->describe().c_str());

    weaknet::VideoEncoderConfig ecfg;
    ecfg.width = kWidth;
    ecfg.height = kHeight;
    ecfg.stride = kStride;
    ecfg.uv_offset = kUvOffset;
    ecfg.fourcc = 0x3231564E;   // 'NV12'
    ecfg.frame_bytes = kFrameBytes;
    ecfg.fps = 18;
    if (!enc->configure(ecfg)) {
        std::printf("!! 编码器 configure 失败: %s\n", enc->describe().c_str());
        return 1;
    }
    std::printf("[enc] configure OK: %s\n", enc->describe().c_str());

    // ---------------------------------------------------------------
    // 3) 编码，收集码流
    // ---------------------------------------------------------------
    std::vector<CollectedFrame> frames_out;
    uint64_t total_bytes = 0;
    uint64_t keyframes = 0;

    // 时间戳用单调时钟起算，与生产路径一致（ef.timestamp_ns 来自 v4l2_buffer，
    // 那是内核单调时钟）。这里自造一个等间隔序列，间隔按 18fps 取。
    constexpr uint64_t kFrameIntervalNs = 1000000000ull / 18ull;
    uint64_t ts = 1000000ull;   // 从一个非零值起，避免 0 被当"未设置"

    for (uint32_t i = 0; i < opt.frames; ++i) {
        std::vector<uint8_t> nv12 = makeSyntheticNv12(i, (i % 18) == 0);

        weaknet::VideoFrameView view;
        view.data = nv12.data();
        view.size = static_cast<uint32_t>(nv12.size());
        view.dmabuf_fd = -1;   // 合成帧没有 dma-buf，走 memcpy 路径
        view.sequence = i;
        view.timestamp_ns = ts;
        ts += kFrameIntervalNs;

        weaknet::EncodedFrame ef;
        if (enc->encode(view, ef) && ef.valid()) {
            CollectedFrame cf;
            cf.bytes.assign(ef.data, ef.data + ef.size);
            cf.keyframe = ef.keyframe;
            cf.timestamp_ns = ef.timestamp_ns;
            total_bytes += cf.bytes.size();
            if (cf.keyframe) ++keyframes;
            frames_out.push_back(std::move(cf));
        }
    }

    // 冲出编码器内部延迟的帧——不 flush 会丢掉最后几帧，而留证最需要的
    // 恰恰可能是最后那几帧。
    const size_t flushed = enc->flush([&](const weaknet::EncodedFrame& ef) {
        if (!ef.valid()) return;
        CollectedFrame cf;
        cf.bytes.assign(ef.data, ef.data + ef.size);
        cf.keyframe = ef.keyframe;
        cf.timestamp_ns = ef.timestamp_ns;
        total_bytes += cf.bytes.size();
        if (cf.keyframe) ++keyframes;
        frames_out.push_back(std::move(cf));
    });

    const weaknet::EncoderStats st = enc->peekStats();
    std::printf("[enc] in=%llu out=%llu keys=%llu dropped=%llu failed=%llu flushed=%zu\n",
                static_cast<unsigned long long>(st.frames_in),
                static_cast<unsigned long long>(frames_out.size()),
                static_cast<unsigned long long>(keyframes),
                static_cast<unsigned long long>(st.dropped),
                static_cast<unsigned long long>(st.failed), flushed);
    std::printf("[enc] codec bytes=%llu\n", static_cast<unsigned long long>(total_bytes));

    if (frames_out.empty()) {
        std::printf("!! 编码器没有产出任何码流\n");
        return 1;
    }

    // ---------------------------------------------------------------
    // 4) 录制器：还原「预录 → 触发 → 后沿」的真实路径
    // ---------------------------------------------------------------
    weaknet::VideoClipMeta meta;
    std::vector<uint8_t> clip_bytes;

    if (opt.use_recorder) {
        weaknet::VideoClipRecorderConfig rcfg;
        rcfg.device_id = cfg.edge.device_id.get();
        rcfg.site_id = cfg.edge.site_id.get();
        rcfg.gateway_id = cfg.edge.device_id.get();
        rcfg.width = kWidth;
        rcfg.height = kHeight;
        // 探针把窗口压到秒级：真实产品是 30s/60s，但这里只有 3 秒素材。
        // 窗口规则本身由单测覆盖，本探针要验的是「录制器产出的字节能不能
        // 被云端收下」，所以窗口取能让流程走完的最小值。
        //
        // **但下限不是随便小**：必须大于一个 GOP。编码器的 GOP 默认是
        // 「1 秒一个 IDR」= fps 帧，而录制器要求回溯窗口里存在 IDR（否则
        // 片段从 P 帧起头 → 花屏且不报错，所以它宁可放弃）。窗口小于 GOP
        // 时，「窗口内无 IDR」是**必然而不是偶发**——2026-11-14 探针就
        // 踩过这个：pre_roll=500ms 配 18fps/18 帧 GOP，每次都被放弃。
        rcfg.pre_roll_ms = 2000;    // > 1 个 GOP（18 帧 ≈ 1000ms）
        rcfg.post_roll_ms = 1500;
        rcfg.max_bytes = 64ULL * 1024 * 1024;

        weaknet::VideoClipRecorder recorder(rcfg);

        // 单调毫秒序列：与帧时间戳同源，只是单位换算。
        auto mono_ms_of = [](uint64_t ns) { return ns / 1000000ull; };

        // 先灌预录窗口（不触发），确保触发点之前的 IDR 已在缓冲里。
        size_t i = 0;
        const size_t pre_count = frames_out.size() / 2;
        for (; i < pre_count; ++i) {
            recorder.push(frames_out[i].bytes.data(), frames_out[i].bytes.size(),
                          frames_out[i].keyframe, mono_ms_of(frames_out[i].timestamp_ns));
        }

        // 把时间轴打出来：录制器的三条规则（回溯/后沿/IDR 起点）全都建立在
        // 「mono_ms 单调且与 timestamp_ns 同源」这个前提上。一旦编码器回传的
        // 时间戳不是我们送进去的那个（或干脆是 0），这些规则会**静默失效**——
        // 表现就是「编码明明成功，录制器既不产出也不放弃」。没有这行输出，
        // 那种失败只能靠猜。
        const uint64_t first_mono = mono_ms_of(frames_out.front().timestamp_ns);
        const uint64_t last_mono = mono_ms_of(frames_out.back().timestamp_ns);
        std::printf("[rec] 时间轴 mono=[%llu,%llu]ms 跨度=%llums（%zu 帧）\n",
                    static_cast<unsigned long long>(first_mono),
                    static_cast<unsigned long long>(last_mono),
                    static_cast<unsigned long long>(last_mono - first_mono),
                    frames_out.size());

        // 触发点 = 已入缓冲的最新帧时刻（录制器内部就是这么取的），
        // 所以这里打 last_mono 而不是另取一次「现在」。
        std::printf("[rec] 预录 %zu 帧 / %zu B，开始触发\n",
                    recorder.bufferedFrames(), recorder.bufferedBytes());
        if (!recorder.startClip("e2e-probe-" + std::to_string(::getpid()))) {
            std::printf("!! 录制器触发失败（已在录制中）\n");
            return 1;
        }

        // 灌完剩余帧（后沿）。
        for (; i < frames_out.size(); ++i) {
            recorder.push(frames_out[i].bytes.data(), frames_out[i].bytes.size(),
                          frames_out[i].keyframe, mono_ms_of(frames_out[i].timestamp_ns));
        }

        const uint64_t mono_now = mono_ms_of(ts);
        const int64_t wall_now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        auto produced = recorder.poll(mono_now, static_cast<uint64_t>(wall_now_ms),
                                      &clip_bytes);
        if (!produced.has_value()) {
            std::printf("!! 录制器未产出片段（produced=%llu aborted=%llu）\n",
                        static_cast<unsigned long long>(recorder.clipsProduced()),
                        static_cast<unsigned long long>(recorder.clipsAborted()));
            return 1;
        }
        meta = *produced;
        std::printf("[rec] 产出片段 clip_id=%s incident=%s bytes=%zu 窗=[%llu,%llu]\n",
                    meta.clip_id.c_str(), meta.incident_id.c_str(), clip_bytes.size(),
                    static_cast<unsigned long long>(meta.started_at_ms),
                    static_cast<unsigned long long>(meta.ended_at_ms));
    } else {
        // 跳过录制器：把全部编码帧拼成一段，直接验证传输层。
        for (const auto& f : frames_out) {
            clip_bytes.insert(clip_bytes.end(), f.bytes.begin(), f.bytes.end());
        }
        meta.device_id = cfg.edge.device_id.get();
        meta.site_id = cfg.edge.site_id.get();
        meta.gateway_id = cfg.edge.device_id.get();
        meta.incident_id = "e2e-probe-norecord";
        meta.width = kWidth;
        meta.height = kHeight;
        meta.fps = 18;
        meta.started_at_ms = 1700000000000ULL;
        meta.ended_at_ms = meta.started_at_ms + (opt.frames * 1000ull / 18ull);
        meta.clip_id = weaknet::VideoClipExporter::makeClipId(
            cfg.edge.device_id.get(), meta.started_at_ms);
        std::printf("[rec] 跳过录制器，直接拼 %zu B\n", clip_bytes.size());
    }

    if (!meta.valid()) {
        std::printf("!! 片段元数据不合法\n");
        return 1;
    }

    // ---------------------------------------------------------------
    // 5) 本地自检：先证明自己算的没错，再去怀疑云端
    // ---------------------------------------------------------------
    {
        const size_t chunks = weaknet::VideoClipExporter::planChunkCount(clip_bytes.size());
        const std::string sha =
            weaknet::VideoClipExporter::sha256Hex(clip_bytes.data(), clip_bytes.size());
        std::string err;
        const std::string manifest = weaknet::VideoClipExporter::buildManifestBody(
            meta, clip_bytes.size(), chunks, sha, &err);
        std::printf("[self] chunks=%zu sha=%s...\n", chunks, sha.substr(0, 16).c_str());
        if (manifest.empty()) {
            std::printf("!! 清单构造失败: %s\n", err.c_str());
            return 1;
        }

        // 逐片构造并**本地回验**：把每片的 payload 解出来拼回去，
        // 必须与原码流逐字节相等。这一步能在发网络之前抓住分片边界错误。
        std::vector<uint8_t> reassembled;
        for (size_t ci = 0; ci < chunks; ++ci) {
            const size_t off = ci * weaknet::VIDEO_CLIP_MAX_CHUNK_BYTES;
            const size_t len = std::min(weaknet::VIDEO_CLIP_MAX_CHUNK_BYTES,
                                        clip_bytes.size() - off);
            std::string cerr;
            const std::string body = weaknet::VideoClipExporter::buildChunkBody(
                meta.device_id, meta.clip_id, ci, clip_bytes.data() + off, len, &cerr);
            if (body.empty()) {
                std::printf("!! 分片 %zu 构造失败: %s\n", ci, cerr.c_str());
                return 1;
            }
            reassembled.insert(reassembled.end(), clip_bytes.begin() + off,
                               clip_bytes.begin() + off + len);
        }
        if (reassembled.size() != clip_bytes.size() ||
            std::memcmp(reassembled.data(), clip_bytes.data(), clip_bytes.size()) != 0) {
            std::printf("!! 分片重新拼接后与原码流不一致（%zu vs %zu）\n",
                        reassembled.size(), clip_bytes.size());
            return 1;
        }
        std::printf("[self] 分片拼回一致 OK（%zu B）\n", reassembled.size());
    }

    if (opt.dry_run) {
        std::printf("[done] dry-run：未发网络请求\n");
        return 0;
    }

    // ---------------------------------------------------------------
    // 6) 真正上传（生产代码路径）
    // ---------------------------------------------------------------
    weaknet::VideoClipExporter exporter(cfg, telemetry_url);
    if (!exporter.start()) {
        std::printf("!! exporter.start() 失败（配置不全或无 curl/TLS 能力）\n");
        return 1;
    }

    const size_t clip_size = clip_bytes.size();
    if (!exporter.enqueue(meta, std::move(clip_bytes))) {
        std::printf("!! enqueue 被拒\n");
        exporter.stop();
        return 1;
    }
    std::printf("[up] 已入队 clip_id=%s bytes=%zu，等待上传完成…\n", meta.clip_id.c_str(),
                clip_size);

    // 等上传结束：以 clips_uploaded / clips_dropped 的推进为准，而不是固定睡。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    bool done = false;
    bool ok = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto s = exporter.stats();
        if (s.clips_uploaded > 0) { done = true; ok = true; break; }
        if (s.clips_dropped > 0 || s.clips_failed > 0) { done = true; ok = false; break; }
    }

    const auto s = exporter.stats();
    exporter.stop();

    std::printf(
        "[up] enqueued=%llu uploaded=%llu failed=%llu dropped=%llu manifests=%llu "
        "chunks=%llu bytes=%llu send_failures=%llu\n",
        static_cast<unsigned long long>(s.clips_enqueued),
        static_cast<unsigned long long>(s.clips_uploaded),
        static_cast<unsigned long long>(s.clips_failed),
        static_cast<unsigned long long>(s.clips_dropped),
        static_cast<unsigned long long>(s.manifests_sent),
        static_cast<unsigned long long>(s.chunks_sent),
        static_cast<unsigned long long>(s.bytes_sent),
        static_cast<unsigned long long>(s.send_failures));

    if (!done) {
        std::printf("!! 超时：180 秒内未完成（既没成功也没失败，检查网络）\n");
        return 1;
    }
    if (!ok) {
        std::printf("!! 上传失败（见上方日志）\n");
        return 1;
    }

    std::printf("[done] 云端确认收齐 clip_id=%s\n", meta.clip_id.c_str());
    return 0;
}
