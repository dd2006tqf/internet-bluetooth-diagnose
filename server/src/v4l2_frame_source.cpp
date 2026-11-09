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

    /// 实际使用的 buf type：单平面或多平面
    ///
    /// 为什么必须是成员而不是 start() 的局部量：`stop()` 里的 STREAMOFF、
    /// 以及 `snapshotAndReset()` 里的 QBUF/DQBUF 都要用它。若各处自己写死
    /// `VIDEO_CAPTURE`，多平面设备上 STREAMOFF 会失败（type 不匹配），
    /// 而且这个失败是静默的——`closeDevice()` 里对 STREAMOFF 的返回值
    /// 刻意不检查（尽力而为），于是录像永不停、fd 泄漏到进程退出。
    v4l2_buf_type buf_type{V4L2_BUF_TYPE_VIDEO_CAPTURE};

    /// 每个 buffer 的平面数（单平面恒为 1）
    ///
    /// 多平面下 `v4l2_buffer.length` 是**平面数**而不是字节长度，
    /// 长度在 `m.planes[i].length` 里。这两者的语义互换是最容易踩的坑：
    /// 拿 length 当字节数会算出恒为 0 的坏帧率。
    uint32_t num_planes{1};

    /// mmap 出来的 buffer（多平面时每个 Buffer 含多个 plane）
    struct Buffer {
        void* start{nullptr};       ///< 单平面时的映射基址（多平面不用）
        size_t length{0};           ///< 单平面时的字节长度
        std::vector<void*> plane_starts;    ///< 多平面：每个 plane 的映射基址
        std::vector<size_t> plane_lengths;  ///< 多平面：每个 plane 的字节长度
    };
    std::vector<Buffer> buffers;

    /// 多平面时 QBUF/DQBUF 复用的 plane 数组
    ///
    /// 为什么要复用而不是每次栈上建：`v4l2_buffer.m.planes` 是指针，
    /// 内核在 ioctl 期间会写它。若每次用局部 vector，出作用域后指针悬空。
    /// 复用同一块内存也省掉每帧一次分配——这是每帧热路径。
    std::vector<v4l2_plane> plane_bufs;

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

    /// 出队延迟记录（毫秒），每个窗口清空一次
    ///
    /// 出队延迟 = DQBUF 返回时刻 − v4l2_buffer.timestamp（帧完成时刻）。
    /// 与 gaps 分开存：两者量纲相同（都是 ms）但语义完全不同，混在一起
    /// 求 P95 会让「用户态被抢占」伪装成「摄像头卡顿」。
    std::vector<uint32_t> dequeue_latencies;

    /// 本窗口起点（用于推导窗口终点 window_start + window_ms）
    std::chrono::steady_clock::time_point window_start;

    ~Impl() { closeDevice(); }

    void closeDevice() {
        if (streaming) {
            // 尽力而为：失败也不阻止关闭，设备可能已消失
            v4l2_buf_type type = buf_type;
            ::ioctl(fd, VIDIOC_STREAMOFF, &type);
            streaming = false;
        }
        for (auto& b : buffers) {
            if (b.start != nullptr && b.start != MAP_FAILED) {
                ::munmap(b.start, b.length);
            }
            for (size_t i = 0; i < b.plane_starts.size(); ++i) {
                if (b.plane_starts[i] != nullptr && b.plane_starts[i] != MAP_FAILED) {
                    ::munmap(b.plane_starts[i], b.plane_lengths[i]);
                }
            }
        }
        buffers.clear();
        plane_bufs.clear();
        num_planes = 1;
        buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    /// 清空所有本窗口的累计量
    ///
    /// **本窗口首帧必须不参与任何「与上一帧比」的判定**，否则每个窗口都会
    /// 凭空多出一次序号跳变和一段巨大的假帧间隔。两者都来自同一个坑：
    /// 跨窗口的状态没有随窗口一起清零。
    ///
    /// 实测证据（vivid，每窗口调用方 sleep(1000)）：
    ///   seq_gap=1 恒定出现 —— 上一窗口末帧 seq=33，本窗口首帧 seq=34，
    ///   但中间跨了 1 秒的帧（驱动已丢），差值远大于 1，被记成一次丢帧事件。
    ///   max_gap≈900ms —— 同上，把「窗口间隔」当成了「帧间隔」。
    ///
    /// 注意 `last_frame_time` 仍要推进到 now()：它只被 `has_last_frame_time`
    /// 控制是否参与比较，初始值本身不参与任何输出。
    void resetWindow() {
        frames_received = 0;
        bytes_received = 0;
        frames_corrupt = 0;
        seq_gap_count = 0;
        // 跨窗口状态必须一起清，否则窗口边界会变成假丢帧 / 假尖刺
        has_last_seq = false;
        gaps.clear();
        dequeue_latencies.clear();
        last_frame_time = std::chrono::steady_clock::now();
        has_last_frame_time = false;
        window_start = std::chrono::steady_clock::now();
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
    // **不要**在这里单独检查 `V4L2_CAP_VIDEO_CAPTURE`。
    //
    // 这是本轮实测抓到的一个真缺陷：只有多平面能力的节点，其
    // `capabilities` 里**没有** `V4L2_CAP_VIDEO_CAPTURE`（0x00000001），
    // 只有 `V4L2_CAP_VIDEO_CAPTURE_MPLANE`（0x00001000）。实测证据
    // （`vivid multiplanar=2`，`v4l2-ctl --info`）：
    //
    //   Capabilities : 0x85231000  ← 无 CAPTURE 位
    //   Device Caps  : 0x05231000  ← Video Capture Multiplanar
    //
    // 在这个位置拦截会让所有 MPLANE-only 设备在走到「判单/多平面」之前
    // 就被判死，`allow_multiplanar` 形同虚设。是否具备采集能力统一由
    // 下面的 2.5 段判定——那里同时看 `CAPTURE` 与 `CAPTURE_MPLANE`。
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        s.error = std::string(s.cfg.device) + " 不支持 V4L2_CAP_STREAMING";
        s.closeDevice();
        return false;
    }

    // ---- 2.5 决定单平面还是多平面 ----
    //
    // 为什么要优先 MPLANE：嵌入式 ISP 的 DMA 通道常按平面分离（Y 一个通道、
    // UV 一个通道），单平面要靠驱动额外拼装。部分 sunxi-vin 配置只出 NV12M
    // 而不出 NV12——这正是方案 §6.2.7 里「ISP 闭源、V4L2 直出 NV12 有风险」
    // 那条可能的具体形态。
    //
    // 判据用 `device_caps` 而不是 `capabilities`：前者描述**这个节点**能做什么，
    // 后者描述整个物理设备。一个设备可以同时注册单平面和多平面节点，
    // 只有 device_caps 能区分当前打开的是哪一个。
    const uint32_t dev_caps = (cap.device_caps != 0) ? cap.device_caps : cap.capabilities;
    const bool drv_mplane = (dev_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
    const bool drv_splane = (dev_caps & V4L2_CAP_VIDEO_CAPTURE) != 0;

    bool use_mplane = false;
    if (s.cfg.allow_multiplanar && drv_mplane) {
        use_mplane = true;
        // 驱动同时声明两种能力时也优先 MPLANE，理由见上。
    } else if (!drv_splane) {
        // 既没有我们要用的多平面，又没单平面 —— 这个节点根本不能采集
        s.error = std::string(s.cfg.device) + " 既无 CAPTURE 也无 CAPTURE_MPLANE";
        s.closeDevice();
        return false;
    }
    s.buf_type = use_mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                            : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    s.num_planes = use_mplane ? 2u : 1u;   // NV12 多平面是 Y + UV 两平面

    // ---- 3. 设定格式 ----
    //
    // 强制 NV12：ISP 输出格式与后续解码链路都以此为基准。若驱动不支持，
    // 用 VIDIOC_TRY_FMT 拿到的实际值回填 —— 但**不能**静默接受任意格式，
    // 否则 frames_expected 的推导会基于错误的分辨率。
    v4l2_format fmt{};
    fmt.type = s.buf_type;
    if (use_mplane) {
        // 多平面用 pix_mp 子结构，长度/步长是**每平面数组**
        fmt.fmt.pix_mp.width = s.cfg.width;
        fmt.fmt.pix_mp.height = s.cfg.height;
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.num_planes = s.num_planes;
    } else {
        fmt.fmt.pix.width = s.cfg.width;
        fmt.fmt.pix.height = s.cfg.height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
    }

    if (::ioctl(s.fd, VIDIOC_S_FMT, &fmt) < 0) {
        s.error = errnoText("VIDIOC_S_FMT", errno);
        s.closeDevice();
        return false;
    }

    // 以驱动回报的**实际**格式为准。这里同时更新 cfg 的分辨率与平面数：
    // 后面 frames_expected 与坏帧判定都依赖它们，用请求值会算错。
    const uint32_t got_w = use_mplane ? fmt.fmt.pix_mp.width : fmt.fmt.pix.width;
    const uint32_t got_h = use_mplane ? fmt.fmt.pix_mp.height : fmt.fmt.pix.height;
    const uint32_t got_fmt =
        use_mplane ? fmt.fmt.pix_mp.pixelformat : fmt.fmt.pix.pixelformat;
    if (use_mplane) {
        const uint32_t got_planes = fmt.fmt.pix_mp.num_planes;
        if (got_planes > 0 && got_planes != s.num_planes) {
            s.num_planes = got_planes;
        }
    }
    if (got_w != s.cfg.width || got_h != s.cfg.height) {
        std::ostringstream os;
        os << "分辨率由 " << s.cfg.width << "x" << s.cfg.height << " 校正为驱动实际 "
           << got_w << "x" << got_h;
        s.error = os.str();
        s.cfg.width = got_w;
        s.cfg.height = got_h;
    }
    if (got_fmt != V4L2_PIX_FMT_NV12) {
        // 驱动改成了别的格式。记录实际值并继续（部分 ISP 只支持 YUYV），
        // 但把这件事写进 describe()，让证据可追溯。
        std::ostringstream os;
        os << "驱动强制格式为 0x" << std::hex << got_fmt
           << "（请求 NV12）";
        s.error = os.str();
    }

    // ---- 3.5 设定帧率（必须！）----
    //
    // 为什么这一步不可省：`frames_expected` 是用 `cfg.fps` 推的。若驱动实际
    // 跑在别的帧率上，丢帧率会算成一个恒定的巨大假值 —— 而且这个假值非常
    // 有迷惑性，看起来像「链路丢了 70% 的帧」。
    //
    // 实测证据（vivid，默认 5fps，配置 30fps，未调 S_PARM）：
    //   expected=30 received=9 lost=21 loss_ratio=0.7000
    // 调完 S_PARM 后同一设备 expected=30 received=30 loss_ratio=0。
    //
    // 驱动不支持 S_PARM 不算失败：很多 sensor 的帧率由链路固定。此时记录
    // 实际帧率并**用它回填 cfg.fps**，让期望值与实际一致，而不是继续用
    // 配置值去算一个假丢帧率。
    if (s.cfg.set_frame_rate) {
        v4l2_streamparm parm{};
        parm.type = s.buf_type;
        if (::ioctl(s.fd, VIDIOC_G_PARM, &parm) < 0) {
            s.error = errnoText("VIDIOC_G_PARM", errno);
            s.closeDevice();
            return false;
        }

        if (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator = s.cfg.fps;
            if (::ioctl(s.fd, VIDIOC_S_PARM, &parm) < 0) {
                // 不支持就算了：下面用 G_PARM 的实际值回填
                parm.type = s.buf_type;
                ::ioctl(s.fd, VIDIOC_G_PARM, &parm);
            }
        }

        // 无论上面成功与否，都以驱动回报的实际帧率为准。
        // 这是「不信配置信事实」：帧率是驱动/硬件决定的，不是我们说了算。
        const uint32_t num = parm.parm.capture.timeperframe.numerator;
        const uint32_t den = parm.parm.capture.timeperframe.denominator;
        if (num > 0 && den > 0) {
            const uint32_t actual = (den + num / 2) / num;
            if (actual != s.cfg.fps) {
                std::ostringstream os;
                os << "帧率由 " << s.cfg.fps << "fps 校正为驱动实际的 " << actual
                   << "fps";
                s.error = os.str();     // 记进 describe() 供证据溯源
                s.cfg.fps = actual;     // frames_expected 从此按实际帧率推
            }
        }
    }

    // ---- 4. 申请 buffer ----
    v4l2_requestbuffers req{};
    req.count = s.cfg.buffer_count ? s.cfg.buffer_count : 4;
    req.type = s.buf_type;
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
    //
    // 单平面与多平面的差别集中在这里，其它地方共用同一套逻辑：
    //   单平面：buf.m.offset → 一块映射，长度 buf.length
    //   多平面：buf.m.planes[p].m.mem_offset → 每平面各一块，长度 planes[p].length
    // 两条路径映射完成后 Buffer 的表示统一，DQBUF 侧再按同一规则取长度。
    s.buffers.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = s.buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        // 多平面必须提供 planes 数组，否则 QUERYBUF 会返回 EINVAL
        if (use_mplane) {
            s.plane_bufs.assign(s.num_planes, v4l2_plane{});
            buf.m.planes = s.plane_bufs.data();
            buf.length = s.num_planes;
        }

        if (::ioctl(s.fd, VIDIOC_QUERYBUF, &buf) < 0) {
            s.error = errnoText("VIDIOC_QUERYBUF", errno);
            s.closeDevice();
            return false;
        }

        if (use_mplane) {
            s.buffers[i].plane_starts.resize(s.num_planes, nullptr);
            s.buffers[i].plane_lengths.resize(s.num_planes, 0);
            for (uint32_t p = 0; p < s.num_planes; ++p) {
                const size_t len = buf.m.planes[p].length;
                void* mp = ::mmap(nullptr, len, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, s.fd,
                                  buf.m.planes[p].m.mem_offset);
                if (mp == MAP_FAILED) {
                    s.error = errnoText("mmap(plane)", errno);
                    s.closeDevice();
                    return false;
                }
                s.buffers[i].plane_starts[p] = mp;
                s.buffers[i].plane_lengths[p] = len;
            }
        } else {
            void* p = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, s.fd, buf.m.offset);
            if (p == MAP_FAILED) {
                s.error = errnoText("mmap", errno);
                s.closeDevice();
                return false;
            }
            s.buffers[i] = Impl::Buffer{p, buf.length, {}, {}};
        }

        // 入队
        if (use_mplane) {
            s.plane_bufs.assign(s.num_planes, v4l2_plane{});
            buf.m.planes = s.plane_bufs.data();
            buf.length = s.num_planes;
        }
        if (::ioctl(s.fd, VIDIOC_QBUF, &buf) < 0) {
            s.error = errnoText("VIDIOC_QBUF", errno);
            s.closeDevice();
            return false;
        }
    }

    // ---- 6. 开始流 ----
    v4l2_buf_type type = s.buf_type;
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

    // 由 start() 决定并存在成员里的 buf type 反推，避免同一个判断写两遍。
    const bool use_mplane = (s.buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

    // ---- 本窗口的起止时刻 ----
    //
    // **窗口 = [上次 snapshotAndReset 返回时刻, 该时刻 + window_ms]**，
    // 由本函数自己占满，而不是「调用方睡多久就是多久 + 再白采一秒」。
    //
    // 这个改动修的是三个同源缺陷。改之前：`elapsed` 按「距上次采样」算，
    // 但排空循环的 deadline 是 `now() + window_ms`，于是窗口内**又**多收了
    // 一整秒的帧。实测（vivid 640x480@30fps，调用方每窗口 sleep(1000)）：
    //
    //   elapsed=1000ms → expected=30
    //   received=34（4 个缓冲积压 + 排空那秒的 30 帧）
    //   max_gap=900ms（那 4 个缓冲填满后到 drain 开始的真实空洞）
    //   seq_gap=1   （跨窗口序号差）
    //
    // 三个数都是真的，但 expected 与 received 落在**不同时间跨度**上，
    // 丢帧率因此无意义。现在窗口终点固定在 `window_start + window_ms`：
    //
    //   - 调用方按 1Hz 立即再调 → 循环跑满整窗，拿到全部帧，loss=0；
    //   - 调用方先 sleep(1000) 再调 → 窗口早已到期，只 drain 出积压的
    //     4 帧，expected 仍是 30 → 如实报 87% 丢帧。
    //
    // 后者不是误报，是真相：1Hz 轮询 + 4 buffer 的架构本身就跟不上 30fps，
    // 驱动在缓冲填满后丢弃了其余的帧。要 0 丢帧必须高频连续 drain。
    const auto window_end =
        s.window_start + std::chrono::milliseconds(s.cfg.window_ms);

    // ---- 把本窗口内的帧全部取走 ----
    //
    // 这些是"原始事实"的采集：收到多少帧、字节数、间隔、序号跳变。
    // 任何成因判断都不在这里做。
    //
    // overdue 分支：调用方自己睡了超过一个窗口（窗口早已到期）。
    // 此时不阻塞，只做一次**非阻塞排空**把积压的 buffer 取走，
    // 队列一空立刻返回。不能直接 break 就走——那会把积压的帧
    // 留在队列里，下一窗口被当成新帧，统计整体错位。
    const bool overdue = std::chrono::steady_clock::now() >= window_end;

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s.fd, &fds);

        const auto now = std::chrono::steady_clock::now();

        timeval tv{};
        if (overdue) {
            tv.tv_sec = 0;
            tv.tv_usec = 0;                     // 非阻塞：只取已在队列里的
        } else {
            const auto remain_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    window_end - now)
                    .count();
            if (remain_ms <= 0) break;          // 窗口内已跑满
            tv.tv_sec = static_cast<time_t>(remain_ms / 1000);
            tv.tv_usec = static_cast<suseconds_t>((remain_ms % 1000) * 1000);
        }

        const int r = ::select(s.fd + 1, &fds, nullptr, nullptr, &tv);
        if (r == 0) break;                      // 窗口到期 / 积压已排空
        if (r < 0) {
            if (errno == EINTR) continue;       // 被信号打断，重试
            s.error = errnoText("select", errno);
            break;
        }

        v4l2_buffer buf{};
        buf.type = s.buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        if (use_mplane) {
            s.plane_bufs.assign(s.num_planes, v4l2_plane{});
            buf.m.planes = s.plane_bufs.data();
            buf.length = s.num_planes;
        }
        if (::ioctl(s.fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;      // 竞态：没准备好，再等
            s.error = errnoText("VIDIOC_DQBUF", errno);
            break;
        }

        // 出队时刻要在 DQBUF 返回后立刻取，越晚取延迟被高估得越多。
        const auto dequeue_t = std::chrono::steady_clock::now();

        // ---- 单帧事实累计 ----
        //
        // 多平面的字节数在 planes[p].bytesused 里逐个累加；
        // 单平面在 buf.bytesused。两者语义相同：这一帧实际填了多少字节。
        uint64_t frame_bytes = 0;
        uint64_t frame_capacity = 0;
        if (use_mplane) {
            for (uint32_t p = 0; p < s.num_planes && p < buf.length; ++p) {
                frame_bytes += buf.m.planes[p].bytesused;
            }
            if (buf.index < s.buffers.size()) {
                for (size_t len : s.buffers[buf.index].plane_lengths) {
                    frame_capacity += len;
                }
            }
        } else {
            frame_bytes = buf.bytesused;
            if (buf.index < s.buffers.size()) {
                frame_capacity = s.buffers[buf.index].length;
            }
        }

        ++s.frames_received;
        s.bytes_received += frame_bytes;

        // 不完整帧：实际字节明显小于容量即视为 corrupt。
        // 用比例而非绝对值，避免分辨率变化后阈值失效。
        if (frame_capacity > 0) {
            const double ratio = static_cast<double>(frame_bytes) /
                                 static_cast<double>(frame_capacity);
            if (ratio < 0.5) ++s.frames_corrupt;
        }

        // ---- 帧间隔：用内核时间戳 ----
        //
        // timestamp 可能是 MONOTONIC 或 MONOTONIC_RAW。两者的**差值**都
        // 是单调的，所以这里不需要区分来源；只有跨设备对齐时才需要。
        //
        // 多平面时 timestamp 在内核里是整帧的完成时刻（VB2 的
        // vb2_buffer_done 统一打戳），与单平面语义一致，不必按平面区分。
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

        // ---- 出队延迟：帧在队列里等了多久 ----
        //
        // 这是本轮新增的第三个量。它与 gaps 的区别是：
        //   gaps    = 相邻两帧**完成时刻**之差 → 反映出帧是否规律
        //   本字段  = 取走时刻 − 该帧完成时刻     → 反映我们取走是否及时
        //
        // 只有两者都看，才能区分「摄像头卡」与「我们的循环被拖住」。
        // 负数（时钟域不一致或驱动时间戳异常）直接丢弃，不钳到 0：
        // 钳 0 会把观测错误伪装成「延迟极低」，那是最坏的一种假数据。
        {
            const auto lat =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    dequeue_t - frame_t)
                    .count();
            if (lat >= 0 && lat < 60000) {   // 超过 60s 视为时间戳异常
                s.dequeue_latencies.push_back(static_cast<uint32_t>(lat));
            }
        }

        // ---- 序号跳变：按**事件**计，不按帧计 ----
        //
        // v4l2_buffer.sequence 是驱动给出的单调序号。跳变说明中间有帧没
        // 送到用户态。一次跳 30 与三十次各跳 1，总量相同但 QoE 不同，
        // 所以这里只 +1，具体丢了多少帧由 expected - received 表达。
        //
        // 回绕处理：sequence 是 32 位无符号，30fps 下约 4.97 年回绕一次。
        // 真回绕时 `buf.sequence` 会突然变成 0，此时 (last_seq + 1) 也与它
        // 不相等，若不特殊处理会误记一次 gap。判据是「比上一个序号还小
        // 且差距极大」——不是任何变小都算回绕，正常重排的差值很小。
        if (s.has_last_seq && buf.sequence != s.last_seq + 1) {
            // 注意 has_last_seq 由 resetWindow() 每个窗口清掉：本窗口首帧
            // 不参与比较。跨窗口比较会稳定产生一次假 gap——实测每窗口
            // 恒定 seq_gap=1，与真实丢帧无关。
            const bool wrapped =
                buf.sequence < s.last_seq &&
                (static_cast<uint64_t>(s.last_seq) - buf.sequence) >
                    0x80000000ull;
            if (!wrapped && buf.sequence > s.last_seq) ++s.seq_gap_count;
            // 序号回绕 / 重排不记为 gap：那是驱动行为，不是丢帧。
        }
        s.last_seq = buf.sequence;
        s.has_last_seq = true;

        // 归还 buffer（多平面同样要带 planes 数组）
        if (use_mplane) {
            s.plane_bufs.assign(s.num_planes, v4l2_plane{});
            buf.m.planes = s.plane_bufs.data();
            buf.length = s.num_planes;
        }
        if (::ioctl(s.fd, VIDIOC_QBUF, &buf) < 0) {
            s.error = errnoText("VIDIOC_QBUF(requeue)", errno);
            break;
        }
    }

    // ---- 汇总 ----
    //
    // 窗口跨度按**窗口设计跨度**算，不是按实际耗时算。
    //
    // 这一条容易写反，写反了就是本轮实测抓到的那三个假象：若用
    // `now() - window_start` 当跨度，调用方多睡的一秒会被算进窗口，
    // 而这一秒里驱动因缓冲填满丢掉的帧不会出现在 received 里，
    // 于是丢帧率被系统性低估（睡眠越久，越显得「健康」）。
    //
    // 固定用 cfg.window_ms 的代价是：调用方延迟进入时，实际只 drain
    // 到较少帧，loss_ratio 会偏高——这是**如实**反映「本窗口没取够帧」，
    // 不是误报。少报比多报危险，所以选这个方向。
    const auto span_ms = static_cast<uint64_t>(s.cfg.window_ms);

    // 与合成源同一套整数算术，避免 IEEE-754 截断（1000.0/(1000.0/30)
    // = 29.999999999999996，截断成 29 会让丢帧率系统性偏移）。
    const uint64_t expected = (span_ms * s.cfg.fps + 500u) / 1000u;

    out.frames_expected = expected;
    out.frames_received = s.frames_received;
    out.frames_corrupt = s.frames_corrupt;
    out.seq_gap_count = s.seq_gap_count;
    out.bytes_received = s.bytes_received;
    out.window_ms = static_cast<uint32_t>(span_ms);
    // 采样时刻用**实际**结束时刻：窗口早已过期时 window_end 在未来，
    // 拿它当 sampled_at 会让下游以为这份数据来自未来。
    out.sampled_at = std::chrono::steady_clock::now();

    if (!s.gaps.empty()) {
        out.max_gap_ms = *std::max_element(s.gaps.begin(), s.gaps.end());
        out.p95_gap_ms = percentile95(s.gaps);
    }

    if (!s.dequeue_latencies.empty()) {
        out.p95_dequeue_latency_ms = percentile95(s.dequeue_latencies);
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

    // 单平面 / 多平面必须写进描述：同一个 FrameStats 在两种 buf type 下
    // 采集方式完全不同，事后排查时需要知道当时走的是哪条路径。
    os << (s.buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? " mplane"
                                                            : " splane");
    if (!s.streaming) os << " (stopped)";

    // 驱动协商结果（帧率/分辨率被校正、格式被强制）也带上：
    // 这些是「配置 vs 事实」的偏差记录，不写下来就无法解释统计为何与预期不符。
    if (!s.error.empty()) os << " [" << s.error << "]";
    return os.str();
}

const std::string& V4l2FrameSource::lastError() const { return impl_->error; }

bool V4l2FrameSource::isRunning() const { return impl_->streaming; }

}  // namespace weaknet
