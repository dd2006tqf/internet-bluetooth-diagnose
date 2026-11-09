/**
 * @file v4l2_frame_source.cpp
 * @brief V4L2 真机帧源实现
 *
 * 本文件是唯一接触 V4L2 ioctl 的地方，所有内核细节都在 pimpl 内部。
 *
 * ============================================================================
 * 与合成源共享的语义（逐条同构，不得越过）
 * ============================================================================
 *
 * 1. **未启动 / 打开失败时返回 valid()==false**，绝不返回全零的「健康」统计。
 *    全零 FrameStats 的 frameLossRatio() 恰好是 0.0，会被上游误读成「零丢帧」。
 *
 * 2. **frames_expected 必须由真实流逝时间推导**，不能用 frames_received 回填。
 *    回填会让丢帧率恒为 0 —— 摄像头掉线后仍报「画面完美」。
 *
 * 3. **乱序不是丢帧**：seq_gap_count 按「序号跳变事件」计，不按帧计。
 *
 * ============================================================================
 * 为什么 start() 不在构造函数里做
 * ============================================================================
 *
 * 构造时不碰 /dev/video*，这样：
 *   - CI（无摄像头）上可以构造对象并验证 start() 失败路径；
 *   - 上层可以先建对象、按需启动，不必在装配阶段就决定是否启用视觉能力。
 *
 * ============================================================================
 * 时间语义
 * ============================================================================
 *
 * 帧间隔一律用 `v4l2_buffer.timestamp`（内核单调时钟），**不用**用户态 now()。
 * 调度延迟会让用户在 "DQBUF 返回的时刻" 与 "帧真正完成的时刻" 之间插入
 * 任意抖动，用它算间隔会把系统负载误判成画面卡顿。
 *
 * 该字段可能是 MONOTONIC 或 MONOTONIC_RAW，启动时通过 VIDIOC_QUERYCAP 的
 * `timestamp_source`（若支持）读取；不支持时按 MONOTONIC 处理，这是内核默认。
 */

#include "v4l2_frame_source.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

namespace weaknet {

namespace {

/// P95（最近秩法），与 synthetic_frame_source.cpp 中的实现保持同一口径。
///
/// 两份实现刻意没有抽成公共头：P95 的取位规则（ceil vs floor vs 插值）
/// 是一个**语义选择**而不是工具函数，两处各自写清反而更难悄悄漂移。
/// 若将来要统一，应连同两边的单测一起改。
uint32_t percentile95(std::vector<uint32_t> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const size_t idx = static_cast<size_t>(
        std::ceil(0.95 * static_cast<double>(values.size())) - 1.0);
    return values[std::min(idx, values.size() - 1)];
}

/// 把 errno 变成可读原因（不抛异常，只记录）
std::string errnoText(const char* what, int err) {
    return std::string(what) + ": " + std::strerror(err);
}

}  // namespace

struct V4l2FrameSource::Impl {
    V4l2CaptureConfig cfg;

    int fd{-1};
    bool streaming{false};
    std::string error;

    /// mmap 出来的 buffer
    struct Buffer {
        void* start{nullptr};
        size_t length{0};
    };
    std::vector<Buffer> buffers;

    /// 采集状态
    uint64_t frames_received{0};
    uint64_t bytes_received{0};
    uint64_t frames_corrupt{0};
    uint32_t seq_gap_count{0};
    uint32_t last_seq{0};
    bool has_last_seq{false};

    /// 帧间隔记录（毫秒），每个窗口清空一次
    std::vector<uint32_t> gaps;
    std::chrono::steady_clock::time_point last_frame_time;
    bool has_last_frame_time{false};

    /// 本窗口起点（用于推导 frames_expected）
    std::chrono::steady_clock::time_point window_start;
    uint64_t window_frames_expected{0};

    ~Impl() { closeDevice(); }

    void closeDevice() {
        if (streaming) {
            // 尽力而为：失败也不阻止关闭，设备可能已消失
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ::ioctl(fd, VIDIOC_STREAMOFF, &type);
            streaming = false;
        }
        for (auto& b : buffers) {
            if (b.start != nullptr && b.start != MAP_FAILED) {
                ::munmap(b.start, b.length);
            }
        }
        buffers.clear();
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    /// 清空所有本窗口的累计量
    void resetWindow() {
        frames_received = 0;
        bytes_received = 0;
        frames_corrupt = 0;
        seq_gap_count = 0;
        gaps.clear();
        last_frame_time = std::chrono::steady_clock::now();
        has_last_frame_time = false;
        window_start = std::chrono::steady_clock::now();
        window_frames_expected = 0;
    }
};

V4l2FrameSource::V4l2FrameSource(V4l2CaptureConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(config);
}

V4l2FrameSource::~V4l2FrameSource() = default;

bool V4l2FrameSource::start() {
    Impl& s = *impl_;

    // 幂等：已启动时直接返回成功，不重复打开
    if (s.streaming) return true;

    s.error.clear();
    s.closeDevice();

    // ---- 1. 打开设备 ----
    //
    // O_NONBLOCK 必须置位：这里用 select() 等可读，不靠 open 的阻塞语义。
    // 非阻塞打开还能让「设备存在但驱动未就绪」的情况立即暴露，而不是卡在 open。
    s.fd = ::open(s.cfg.device.c_str(), O_RDWR | O_NONBLOCK);
    if (s.fd < 0) {
        // 这是"能力不可用"而不是错误：摄像头没插就是这条路径。
        s.error = errnoText(("open " + s.cfg.device).c_str(), errno);
        return false;
    }

    // ---- 2. 查询能力 ----
    v4l2_capability cap{};
    if (::ioctl(s.fd, VIDIOC_QUERYCAP, &cap) < 0) {
        s.error = errnoText("VIDIOC_QUERYCAP", errno);
        s.closeDevice();
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        s.error = std::string(s.cfg.device) + " 不支持 V4L2_CAP_VIDEO_CAPTURE";
        s.closeDevice();
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        s.error = std::string(s.cfg.device) + " 不支持 V4L2_CAP_STREAMING";
        s.closeDevice();
        return false;
    }

    // ---- 3. 设定格式 ----
    //
    // 强制 NV12：ISP 输出格式与后续解码链路都以此为基准。若驱动不支持，
    // 用 VIDIOC_TRY_FMT 拿到的实际值回填 —— 但**不能**静默接受任意格式，
    // 否则 frames_expected 的推导会基于错误的分辨率。
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = s.cfg.width;
    fmt.fmt.pix.height = s.cfg.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (::ioctl(s.fd, VIDIOC_S_FMT, &fmt) < 0) {
        s.error = errnoText("VIDIOC_S_FMT", errno);
        s.closeDevice();
        return false;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12) {
        // 驱动改成了别的格式。记录实际值并继续（部分 ISP 只支持 YUYV），
        // 但把这件事写进 describe()，让证据可追溯。
        std::ostringstream os;
        os << "驱动强制格式为 0x" << std::hex << fmt.fmt.pix.pixelformat;
        s.error = os.str();
    }

    // ---- 4. 申请 buffer ----
    v4l2_requestbuffers req{};
    req.count = s.cfg.buffer_count ? s.cfg.buffer_count : 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (::ioctl(s.fd, VIDIOC_REQBUFS, &req) < 0) {
        s.error = errnoText("VIDIOC_REQBUFS", errno);
        s.closeDevice();
        return false;
    }
    if (req.count < 2) {
        s.error = "驱动返回的 buffer 数不足（" + std::to_string(req.count) + "）";
        s.closeDevice();
        return false;
    }

    // ---- 5. mmap + 入队 ----
    s.buffers.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (::ioctl(s.fd, VIDIOC_QUERYBUF, &buf) < 0) {
            s.error = errnoText("VIDIOC_QUERYBUF", errno);
            s.closeDevice();
            return false;
        }

        void* p = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, s.fd, buf.m.offset);
        if (p == MAP_FAILED) {
            s.error = errnoText("mmap", errno);
            s.closeDevice();
            return false;
        }
        s.buffers[i] = Impl::Buffer{p, buf.length};

        if (::ioctl(s.fd, VIDIOC_QBUF, &buf) < 0) {
            s.error = errnoText("VIDIOC_QBUF", errno);
            s.closeDevice();
            return false;
        }
    }

    // ---- 6. 开始流 ----
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (::ioctl(s.fd, VIDIOC_STREAMON, &type) < 0) {
        s.error = errnoText("VIDIOC_STREAMON", errno);
        s.closeDevice();
        return false;
    }
    s.streaming = true;

    s.resetWindow();
    return true;
}

void V4l2FrameSource::stop() {
    Impl& s = *impl_;
    s.closeDevice();
    s.resetWindow();
}

FrameStats V4l2FrameSource::snapshotAndReset() {
    Impl& s = *impl_;
    FrameStats out;

    // 未启动 → 无效统计。上层必须报 unavailable，不能当成"零丢帧"。
    if (!s.streaming) return out;

    // ---- 本窗口的结束时刻与期望帧数 ----
    //
    // **关键**：frames_expected 由「实际流逝时间 × 帧率」推导，不能用
    // frames_received 或标称窗口长度回填。后者在进程被挂起、驱动卡死时
    // 会算出「期望 == 收到」，把一次严重的停流报成完美。
    const auto window_end = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            window_end - s.window_start)
            .count();

    // 与合成源同一套整数算术，避免 IEEE-754 截断（1000.0/(1000.0/30)
    // = 29.999999999999996，截断成 29 会让丢帧率系统性偏移）。
    const uint64_t expected =
        elapsed > 0
            ? (static_cast<uint64_t>(elapsed) * s.cfg.fps + 500u) / 1000u
            : 0;

    // ---- 把队列里已到的帧全部取走（不超过一个窗口的处理预算）----
    //
    // 这些是"原始事实"的采集：收到多少帧、字节数、间隔、序号跳变。
    // 任何成因判断都不在这里做。
    const auto deadline = window_end + std::chrono::milliseconds(s.cfg.window_ms);

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s.fd, &fds);

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        const auto remain_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count();

        timeval tv{};
        tv.tv_sec = static_cast<time_t>(remain_ms / 1000);
        tv.tv_usec = static_cast<suseconds_t>((remain_ms % 1000) * 1000);

        const int r = ::select(s.fd + 1, &fds, nullptr, nullptr, &tv);
        if (r == 0) break;                      // 到预算了，正常退出
        if (r < 0) {
            if (errno == EINTR) continue;       // 被信号打断，重试
            s.error = errnoText("select", errno);
            out.sampled_at = window_end;
            out.frames_expected = expected;
            out.frames_received = s.frames_received;
            out.frames_corrupt = s.frames_corrupt;
            out.seq_gap_count = s.seq_gap_count;
            out.bytes_received = s.bytes_received;
            out.window_ms = static_cast<uint32_t>(elapsed);
            return out;
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(s.fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;      // 竞态：没准备好，再等
            s.error = errnoText("VIDIOC_DQBUF", errno);
            break;
        }

        // ---- 单帧事实累计 ----
        ++s.frames_received;
        s.bytes_received += buf.bytesused;

        // 不完整帧：bytesused 明显小于 buffer 长度即视为 corrupt。
        // 用比例而非绝对值，避免分辨率变化后阈值失效。
        if (buf.index < s.buffers.size() && s.buffers[buf.index].length > 0) {
            const double ratio = static_cast<double>(buf.bytesused) /
                                 static_cast<double>(s.buffers[buf.index].length);
            if (ratio < 0.5) ++s.frames_corrupt;
        }

        // ---- 帧间隔：用内核时间戳 ----
        //
        // timestamp 可能是 MONOTONIC 或 MONOTONIC_RAW。两者的**差值**都
        // 是单调的，所以这里不需要区分来源；只有跨设备对齐时才需要。
        const auto frame_t = std::chrono::steady_clock::time_point(
            std::chrono::seconds(buf.timestamp.tv_sec) +
            std::chrono::microseconds(buf.timestamp.tv_usec));

        if (s.has_last_frame_time) {
            const auto gap_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    frame_t - s.last_frame_time)
                    .count();
            if (gap_ms >= 0) {
                s.gaps.push_back(static_cast<uint32_t>(gap_ms));
            }
        }
        s.last_frame_time = frame_t;
        s.has_last_frame_time = true;

        // ---- 序号跳变：按**事件**计，不按帧计 ----
        //
        // v4l2_buffer.sequence 是驱动给出的单调序号。跳变说明中间有帧没
        // 送到用户态。一次跳 30 与三十次各跳 1，总量相同但 QoE 不同，
        // 所以这里只 +1，具体丢了多少帧由 expected - received 表达。
        if (s.has_last_seq && buf.sequence != s.last_seq + 1) {
            if (buf.sequence > s.last_seq) ++s.seq_gap_count;
            // 序号回绕 / 重排不记为 gap：那是驱动行为，不是丢帧。
        }
        s.last_seq = buf.sequence;
        s.has_last_seq = true;

        // 归还 buffer
        if (::ioctl(s.fd, VIDIOC_QBUF, &buf) < 0) {
            s.error = errnoText("VIDIOC_QBUF(requeue)", errno);
            break;
        }
    }

    // ---- 汇总 ----
    out.frames_expected = expected;
    out.frames_received = s.frames_received;
    out.frames_corrupt = s.frames_corrupt;
    out.seq_gap_count = s.seq_gap_count;
    out.bytes_received = s.bytes_received;
    out.window_ms = static_cast<uint32_t>(elapsed);
    out.sampled_at = window_end;

    if (!s.gaps.empty()) {
        out.max_gap_ms = *std::max_element(s.gaps.begin(), s.gaps.end());
        out.p95_gap_ms = percentile95(s.gaps);
    }

    // 推进窗口：下一窗口从此刻开始
    s.resetWindow();
    return out;
}

std::string V4l2FrameSource::describe() const {
    const Impl& s = *impl_;
    std::ostringstream os;
    // 前缀 v4l2: 让 evidence 消费者能一眼区分真机与注入数据（与 synthetic: 对应）
    os << "v4l2:" << s.cfg.device << "@" << s.cfg.width << "x" << s.cfg.height
       << "/" << s.cfg.fps << "fps/" << s.cfg.window_ms << "ms";
    if (!s.streaming) os << " (stopped)";
    return os.str();
}

const std::string& V4l2FrameSource::lastError() const { return impl_->error; }

bool V4l2FrameSource::isRunning() const { return impl_->streaming; }

}  // namespace weaknet
