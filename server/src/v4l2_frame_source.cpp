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

#include <linux/media.h>
#include <linux/dma-heap.h>   // DMA_HEAP_IOCTL_ALLOC：DMABUF 导入路径
#include <sys/stat.h>
#include <sys/sysmacros.h>   // major() / minor()：用于匹配 video 节点与 media 实体

#include "logger.hpp"
#include "media_topology.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
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

// ---------------------------------------------------------------------------
// dma-heap 分配（DMABUF 导入路径的唯一依赖）
// ---------------------------------------------------------------------------

/// 从 dma-heap 分配一块连续内存，返回 dma-buf fd；失败返回 -1
///
/// 为什么优先 `/dev/dma_heap/system` 而不是 `cma`：`system` 堆在所有带
/// dma-buf 的内核上都存在（本机实测有），而 `cma` 需要板级预留 CMA 区域，
/// 大小随设备差异极大，写死会在部分板上直接分配失败。需要连续物理内存的
/// 驱动会自己做 bounce，不需要我们选堆。
///
/// 找不到 `/dev/dma_heap/system` 是**正常情况**（老内核只有 ION，
/// 部分精简发行版干脆不编 dma-buf heaps）。此时返回 -1，由调用方回落到
/// 导出路径，而不是让 start() 失败。
int allocFromDmaHeap(size_t length) {
    const int hfd = ::open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (hfd < 0) return -1;

    dma_heap_allocation_data data{};
    data.len = length;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    data.heap_flags = 0;

    const int rc = ::ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &data);
    ::close(hfd);
    if (rc < 0) return -1;
    return static_cast<int>(data.fd);
}

// ---------------------------------------------------------------------------
// media controller 拓扑枚举（ioctl 部分）
//
// 判定与格式化的**纯算法**已切到 media_topology.{hpp,cpp}：那里可以在
// 合成拓扑上做单测，而这里只剩「把内核结构体搬进 MediaTopology」。
// 这样切分的理由见 media_topology.hpp 的头注释——图算法错了不会报错，
// 只会安静地输出一个看似合理的错误结论，只能靠单测抓。
// ---------------------------------------------------------------------------

/// 枚举一个 media 设备的所有实体
///
/// 迭代方式：每轮把**上一次返回的 id** 作为 `MEDIA_ENT_ID_FLAG_NEXT` 的基址。
/// 用固定的 0..N 循环会重复枚举（实测会输出 29 条而非 11 条实体）。
bool enumerateEntities(int fd, MediaTopology& topo) {
    uint32_t next_id = 0;
    for (;;) {
        media_entity_desc ed{};
        ed.id = next_id | MEDIA_ENT_ID_FLAG_NEXT;
        if (::ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &ed) < 0) break;

        MediaEntity ent;
        ent.id = ed.id;
        ent.name.assign(ed.name, ::strnlen(ed.name, sizeof ed.name));
        ent.kind = classifyEntity(ed.type);
        if (ent.kind == MediaEntityKind::V4lNode) {
            // 旧式编码用 v4l.major/minor，新式用 dev.major/minor。
            // 两者在 union 里位置相同，所以取 dev 即可（实测 vivid 与 vimc 一致）。
            ent.major = ed.dev.major;
            ent.minor = ed.dev.minor;
        }
        topo.entities.push_back(ent);

        // id == 0 是内核约定的"最后一个"，必须 break 否则死循环
        if (ed.id == 0) break;
        next_id = ed.id;
    }
    return !topo.entities.empty();
}

/// 枚举链路
///
/// 注意只对 `links > 0 && pads > 0` 的实体调用：零链路的实体传空数组
/// 会得到 EINVAL，而那不是错误。
void enumerateLinks(int fd, MediaTopology& topo) {
    for (const auto& ent : topo.entities) {
        media_entity_desc probe{};
        probe.id = ent.id;
        if (::ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &probe) < 0) continue;
        if (probe.links == 0 || probe.pads == 0) continue;

        std::vector<media_pad_desc> pads(probe.pads);
        std::vector<media_link_desc> links(probe.links);
        media_links_enum le{};
        le.entity = ent.id;
        le.pads = pads.data();
        le.links = links.data();

        if (::ioctl(fd, MEDIA_IOC_ENUM_LINKS, &le) < 0) continue;

        for (uint32_t i = 0; i < probe.links; ++i) {
            MediaLink ml;
            ml.source_id = ent.id;
            ml.sink_id = links[i].sink.entity;
            ml.sink_pad = links[i].sink.index;
            ml.enabled = (links[i].flags & MEDIA_LNK_FL_ENABLED) != 0;
            topo.links.push_back(ml);
        }
    }
}

/// 尝试解析某个 video 节点所属的 media 拓扑
///
/// 返回 false 表示「没找到 / 没权限 / 驱动不支持」，**不是错误**。
/// 扫描 /dev/media0..15：板上同时存在多个 media 设备（vin / isp / csi 各一个
/// 不罕见），必须按 video 节点的 major:minor 匹配，不能取第一个。
bool probeMediaTopology(const std::string& video_device, MediaTopology& out) {
    struct stat video_st {};
    if (::stat(video_device.c_str(), &video_st) < 0) return false;

    // **必须用 st_rdev 而不是 st_dev**：前者是设备节点自身的 major:minor
    // （/dev/video3 → 81:3），后者是**该文件所在文件系统**的设备号
    // （实测 /dev/video3 的 st_dev 是 0:5，与任何实体都对不上）。
    const uint32_t want_major = static_cast<uint32_t>(major(video_st.st_rdev));
    const uint32_t want_minor = static_cast<uint32_t>(minor(video_st.st_rdev));

    for (int i = 0; i < 16; ++i) {
        const std::string path = "/dev/media" + std::to_string(i);
        const int mfd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (mfd < 0) continue;

        MediaTopology topo;
        topo.device = path;

        media_device_info info{};
        if (::ioctl(mfd, MEDIA_IOC_DEVICE_INFO, &info) == 0) {
            topo.driver.assign(info.driver,
                               ::strnlen(info.driver, sizeof info.driver));
        }

        const bool got_entity = enumerateEntities(mfd, topo);

        // 只有当本 media 设备里存在该 video 节点时才认领
        const bool owns =
            got_entity && claimNodeByDeviceNumber(topo, want_major, want_minor);

        if (owns) {
            enumerateLinks(mfd, topo);
            topo.valid = true;
            out = std::move(topo);
            ::close(mfd);
            return true;
        }
        ::close(mfd);
    }
    return false;
}

}  // namespace

struct V4l2FrameSource::Impl {
    V4l2CaptureConfig cfg;

    int fd{-1};
    bool streaming{false};
    std::string error;

    /// `VIDIOC_S_INPUT` 的异常失败原因（ENOTTY/EINVAL 之外的 errno）
    ///
    /// 单独存而不塞进 `error`：`error` 的语义是「配置与事实的偏差」，
    /// 会被 describe() 以 `[...]` 呈现；而 S_INPUT 失败在多数设备上是
    /// **正常**的（无输入概念）。只有真出意外时才记，且不因此中断启动。
    ///
    /// 为什么值得留一个字段：真机上「S_FMT 报 EINVAL」的唯一现场线索就是
    /// S_INPUT 有没有成功。若它因 `EPERM`/`EBUSY` 之类失败而没人记录，
    /// 事后只能看到 S_FMT 的报错，根因又得重推一遍。
    ///
    /// **不被 closeDevice() 复位**，与 `buffer_source` 同一考虑：它是
    /// 「上一次 start 发生了什么」的事实，停下来看仍应如实呈现。
    std::string input_note;

    /// 实际使用的 buf type：单平面或多平面
    ///
    /// 为什么必须是成员而不是 start() 的局部量：`stop()` 里的 STREAMOFF、
    /// 以及 `snapshotAndReset()` 里的 QBUF/DQBUF 都要用它。若各处自己写死
    /// `VIDEO_CAPTURE`，多平面设备上 STREAMOFF 会失败（type 不匹配），
    /// 而且这个失败是静默的——`closeDevice()` 里对 STREAMOFF 的返回值
    /// 刻意不检查（尽力而为），于是录像永不停、fd 泄漏到进程退出。
    ///
    /// **`closeDevice()` 会把它复位成单平面**（下一次 start 若不重新探测就用
    /// 错类型）。所以它只能回答「现在用什么」，不能回答「刚才用了什么」——
    /// 后者由下面的 `started_plane_kind` 负责。
    v4l2_buf_type buf_type{V4L2_BUF_TYPE_VIDEO_CAPTURE};

    /// 上一次 start() 实际用的是单平面还是多平面（**不被 closeDevice 重置**）
    ///
    /// 为什么需要它：`closeDevice()` 会把 `buf_type` 复位成单平面，于是
    /// `stop()` 之后再调 `describe()` 会把「启动时明明跑在 mplane」误报成
    /// `splane`。板端实测证据（2026-11-14）：
    ///
    ///   camera capture started: ... mplane {vin_video0 <- ... <- imx219}
    ///   camera capture stopped: ... splane (stopped) {vin_video0 <- ...}
    ///
    /// 同一个进程、同一个设备，开头和结尾对同一件事给出两种说法。这与
    /// `buffer_source` 是**完全同源的一类缺陷**（那里已经用「不复位」解决了），
    /// 只是当时只修了内存模型、漏了平面模型。诊断字段最忌讳「停下来看就变了
    /// 个说法」：事后拿停止时那行日志去取证，会得出「本板不需要 MPLANE」
    /// 这个与事实相反的结论。
    ///
    /// 取值为**上一次成功 start 的探测结果**；从未 start 过时是单平面（与
    /// `buf_type` 的初值一致，描述的是「还没跑过」而不是「跑过单平面」）。
    bool started_mplane{false};

    /// 每个 buffer 的平面数（单平面恒为 1）
    ///
    /// 多平面下 `v4l2_buffer.length` 是**平面数**而不是字节长度，
    /// 长度在 `m.planes[i].length` 里。这两者的语义互换是最容易踩的坑：
    /// 拿 length 当字节数会算出恒为 0 的坏帧率。
    uint32_t num_planes{1};

    /// 实际使用的内存模型
    ///
    /// 为什么必须是成员：`VIDIOC_QBUF` / `VIDIOC_DQBUF` / `VIDIOC_REQBUFS`
    /// 三处的 `memory` 字段必须完全一致，任一处写死 `V4L2_MEMORY_MMAP`
    /// 都会在 DMABUF 模式下返回 EINVAL，而错误位置离根因很远（表现是
    /// 「入队失败」而不是「memory 类型不匹配」）。
    uint32_t memory{V4L2_MEMORY_MMAP};

    /// 本次 start() 实际生效的缓冲来源（**不被 closeDevice 重置**）
    ///
    /// 为什么不能直接用 `memory` 回答「DMABUF 生效了吗」：closeDevice()
    /// 会把 `memory` 复位成 MMAP（否则下次 start 会带着旧类型走 REQBUFS），
    /// 于是 stop() 之后 describe() 会把「配置了 DMABUF 且成功了」误报成
    /// 「回落到了 MMAP」。诊断字段最忌讳这种「停下来看就变了个说法」。
    ///
    /// 取值语义：
    ///   kMemMmap            —— 配置没开 DMABUF，走默认 MMAP；
    ///   kMemDmabufHeap      —— 导入路径（dma-heap 分配）；
    ///   kMemDmabufExport    —— 导出路径（EXPBUF）；
    ///   kMemDmabufMmapFallback —— 配置开了 DMABUF 但两级全失败，回落 MMAP。
    enum BufferSource : uint32_t {
        kMemMmap = 0,
        kMemDmabufHeap = 1,
        kMemDmabufExport = 2,
        kMemDmabufMmapFallback = 3,
    };
    BufferSource buffer_source{kMemMmap};

    /// 每个 buffer 的平面存储
    ///
    /// 同一个结构同时表达 MMAP 与 DMABUF 两种来源：
    ///   - MMAP：`start` / `plane_starts` 持有映射地址，关闭时要 munmap；
    ///   - DMABUF：`dma_fds` 持有 dma-buf fd，关闭时要 close。
    ///
    /// 刻意**不做成两个结构体**：下游的「帧容量」计算（坏帧率判定）在两种
    /// 模型下完全一致，都要用到 `length` / `plane_lengths`。分成两个结构
    /// 会让那段逻辑出现两份分支，而它已经是「拿长度算比例」这种容易写错的
    /// 地方，不该再加一层分叉。
    struct Buffer {
        void* start{nullptr};       ///< 单平面 MMAP 时的映射基址（多平面不用）
        size_t length{0};           ///< 单平面时的字节长度（DMABUF 下也填 = 帧容量）
        std::vector<void*> plane_starts;    ///< 多平面 MMAP：每个 plane 的映射基址
        std::vector<size_t> plane_lengths;  ///< 多平面：每个 plane 的字节长度
        std::vector<int> dma_fds;           ///< DMABUF：每个 plane 一个 fd（未用为 -1）
        std::vector<uint32_t> dma_lengths;  ///< DMABUF：每个 plane 的字节长度
    };
    std::vector<Buffer> buffers;

    /// 从一份 v4l2_format 推导并记录交付几何（stride / uv_offset / sizeimage）
    ///
    /// ========================================================================
    /// 为什么做成独立方法：它在 start() 里必须被调用**两次**
    /// ========================================================================
    ///
    /// 第一次在 S_FMT 之后：此时 `sizeimage` 往往是 **V4L2 框架填的默认值**，
    /// 而不是驱动的真值。它可能是个**自洽的错值**（实测 3030720 = 紧凑 NV12
    /// 字节数，而真值是 3045504），足以骗过下面每一道 guard —— 详见 step 5.5
    /// 那段注释里的实测日志与等号推导。
    ///
    /// 第二次在 REQBUFS 之后（`queue_setup()` 才把 `payload[]` 算成真值）：
    /// 这一次才是可采信的。两处若各写一份逻辑，早晚会漂移成两个判断标准。
    void deriveGeometry(const v4l2_format& fmt, bool use_mplane,
                        uint32_t got_w, uint32_t got_h, uint32_t got_fmt) {
        frame_fourcc = got_fmt;
        frame_sizeimage = use_mplane ? fmt.fmt.pix_mp.plane_fmt[0].sizeimage
                                     : fmt.fmt.pix.sizeimage;

        // stride 候选：bytesperline（>0），否则 width
        uint32_t stride_cand = use_mplane ? fmt.fmt.pix_mp.plane_fmt[0].bytesperline
                                          : fmt.fmt.pix.bytesperline;
        if (stride_cand < got_w) stride_cand = got_w;   // 比宽度还小必然损坏

        // 自洽性检查：候选 stride 下的 NV12 整帧必须装得进 sizeimage。
        // **注意这条检查挡不住「自洽的错值」**：若 sizeimage 恰好等于
        // stride*height*1.5（S_FMT 之后框架默认值就是），它必然通过。
        // 所以这个函数必须在 REQBUFS 之后再调用一次，见 start() step 5.5。
        if (frame_sizeimage > 0 &&
            static_cast<uint64_t>(stride_cand) * got_h * 3 / 2 > frame_sizeimage) {
            stride_cand = got_w;   // 否决：装不下，按紧凑排布
        }
        frame_stride = stride_cand;

        // 把「这次推导是凭什么成立的」记下来。**这不是调试残留**：
        // sizeimage==0 时上面两道 guard 全部被跳过，等于没有校验，而
        // 函数返回 void、ioctl 也返回 0，外部完全看不出区别。没有这行
        // 日志，事后只能看到最终几何，无法判断它是校验出来的还是蒙出来的。
        LOG_DEBUG(weaknet_dbus::LogModule::NETWORK,
                  "v4l2 geometry: sizeimage=" << frame_sizeimage
                  << " bytesperline=" << (use_mplane
                         ? fmt.fmt.pix_mp.plane_fmt[0].bytesperline
                         : fmt.fmt.pix.bytesperline)
                  << " -> stride=" << frame_stride
                  << (frame_sizeimage == 0
                          ? " (sizeimage=0: guard SKIPPED, compact fallback)"
                          : ""));

        if (frame_sizeimage == 0) {
            // 驱动没报 sizeimage：按紧凑 NV12 兜底（UV 紧跟在 stride*height）。
            frame_sizeimage = static_cast<size_t>(frame_stride) * got_h * 3 / 2;
        }

        // UV 平面偏移：默认「紧跟 Y」，即 stride*height。
        size_t uv_off = static_cast<size_t>(frame_stride) * got_h;
        if (got_fmt == V4L2_PIX_FMT_NV12) {
            // 只有当驱动把 payload 报成**整帧**字节数时，payload×2/3 才等于
            // Y 平面字节数（从而等于 UV 起始）。两条约束都必须满足才采信：
            //   a) 不小于 stride*height —— 否则说明 payload 只是 Y 平面，
            //      按 2/3 反推会算出一个比 Y 还可疑的位置；
            //   b) UV 平面必须装得下 —— uv_off + stride*(h/2) <= sizeimage。
            // 任何一条不满足就退回 stride*height（标准紧凑布局）。
            const size_t cand = static_cast<size_t>(frame_sizeimage) * 2 / 3;
            const size_t uv_bytes = static_cast<size_t>(frame_stride) * (got_h / 2);
            if (cand >= uv_off && cand + uv_bytes <= frame_sizeimage) {
                uv_off = cand;
            }
        }
        frame_uv_offset = static_cast<uint32_t>(uv_off);
    }

    // ---- 交付几何（供 pixel_sink 使用）----
    //
    // 为什么必须单独记，而不是让消费者从 cfg 里取：cfg.width/height 是**请求值**
    // 或驱动回报值，但 stride / uv_offset / sizeimage 只在 v4l2_format 里，
    // 且 `bytesperline` 本身可能是假的（sunxi-vin 给 2460 而真实是 1640，
    // 详见 configureFormat 里那段推导），消费者无法自行推导。
    uint32_t frame_stride{0};      ///< Y 平面每行字节数（已过自洽性校验）
    uint32_t frame_uv_offset{0};   ///< UV 平面相对首平面的字节偏移
    uint32_t frame_fourcc{0};      ///< 驱动实际回报的 pixelformat
    size_t frame_sizeimage{0};     ///< 首平面 sizeimage（NV12 单平面 = 整帧）

    /// 像素消费者。空则完全不触发，零开销。
    PixelSink pixel_sink;
    // 采集线程是否已经跑起来（决定 setPixelSink 能否接受注册）
    std::atomic<bool> capture_running{false};

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

    /// 本窗口帧间隔的**微秒**精度原始值，专供帧率基线校准
    ///
    /// 为什么不复用 `gaps`：本板真实帧间隔 53.87ms 在 `gaps` 里被截成 54ms，
    /// 反推得到 `1e6/54000 = 18.52 → 18fps`（方向偶然正确）或按毫秒算
    /// `1000/54 = 18.5`，误差都会被**一次性校准永久固化**。校准是建立基线
    /// 而非持续测量，所以必须用原始精度。
    ///
    /// 与 `gaps` 同生命周期：由 `resetWindow()` 一起清空，由
    /// `tryCalibrateFps()` 在清空**之前**消费。
    std::vector<uint64_t> window_gaps_us;

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

    /// ---- 自驱采集线程（2026-11-12 引入）----
    ///
    /// ========================================================================
    /// 为什么必须改成自驱线程
    /// ========================================================================
    ///
    /// 改造前 `snapshotAndReset()` 内部有一个 `select()` 排空循环，**调用它
    /// 就阻塞 window_ms**。这在测试里没问题（调用方按 1Hz 紧挨着调），但接入
    /// 生产循环后有两个致命后果：
    ///
    ///   1. 质量循环间隔是 15s，一次调用阻塞 1s 意味着循环被整体拖慢；
    ///   2. **更根本**：网络侧用的是 120s 滑动窗口。改造前要拿到 120s 的帧
    ///      统计，就得让调用方阻塞 120s——生产上不可能这么做。于是帧窗口
    ///      永远只能是 1s，而 `attributeVideoQoe` 的 `window_ms != interval_ms`
    ///      判定会**恒定**返回 `window_mismatch`，归因链路等于没接。
    ///
    /// 改成自驱后：
    ///   - 线程持续 DQBUF/QBUF，**永不因缓冲填满而丢帧**（改造前「低频轮询 +
    ///     4 buffer」的架构本身就跟不上 30fps，驱动在缓冲填满后丢弃其余帧——
    ///     那不是链路问题，是我们自己的）
    ///   - `snapshotAndReset()` 变成「读走累计值 + 清零」，**立即返回**，
    ///     调用方可以按任意节奏取（包括 120s 一次的滑动窗口）
    ///   - 统计覆盖跨度自然等于「两次快照之间的真实时长」，不再需要靠阻塞
    ///     去「凑满」一个窗口
    ///
    /// ========================================================================
    /// 线程安全
    /// ========================================================================
    ///
    /// 所有累计量由 `mtx` 保护。正常路径（约 18fps）下每帧只持锁几十纳秒，
    /// `snapshotAndReset()` 与线程只在快照瞬间竞争。
    ///
    /// `describe()` 也读 `error` / `calibration_note`，而线程可能写 `error`，
    /// 所以 `mtx` 声明为 mutable 并在 describe() 里加锁——不能因为「它只读」
    /// 就跳过，那是数据竞争，不是风格问题。
    std::thread capture_thread;
    mutable std::mutex mtx;

    /// 停止请求。用 atomic 而不是「锁内 bool」：线程可能正卡在 select() 里，
    /// 只有无锁标志能让它在超时返回后立刻看到。
    std::atomic<bool> thread_should_stop{false};

    /// 线程对象是否已创建（仅调用方线程读写，不需要锁）
    bool thread_active{false};

    /// 线程是否因致命错误退出（mtx 保护）
    ///
    /// 为什么单独记：线程死后累计值就冻结在死亡时刻。若继续把这些值当成
    /// 「本窗口统计」交出去，`frames_expected` 仍按真实时长增长，于是会报出
    /// 一个巨大的、纯属虚构的丢帧率——把「我们自己采不到」栽给链路。
    /// 置位后 `snapshotAndReset()` 一律返回 invalid，`describe()` 说明原因。
    bool thread_fatal{false};

    /// 上一次快照的时刻（mtx 保护）。`start()` 时置为当时，
    /// 使第一份快照的跨度 = 「从开始采到第一次取走」。
    std::chrono::steady_clock::time_point last_snapshot_time;
    bool has_last_snapshot{false};

    /// 校准子窗口的起点（仅采集线程读写，持 mtx 修改）
    ///
    /// 校准需要「若干个等长的窗口」，这个边界由**线程**按 cfg.window_ms
    /// 自行推进，与调用方的快照节奏无关。若跟着快照相走，调用方改成
    /// 15s 取一次就会让「3 个校准窗口」变成 45s 的样本——校准本身没错，
    /// 但样本量暴涨且语义漂移。
    std::chrono::steady_clock::time_point calibration_window_start;

    /// ---- 帧率基线校准状态 ----
    ///
    /// 为什么需要它：`G_PARM` 在 sunxi-vin 上返回的是模式表常数
    /// （`sensor_helper.c:892` 直接读 `current_wins->fps_fixed`），
    /// 与硬件实际帧率无关。本板满分辨率模式表写 30fps/278MHz，
    /// 而该模式 PLL 只配到 172.8MHz → 真实 18.56fps，造成恒定 38% 假丢帧。
    ///
    /// 校准**只做一次**，之后 fps 固定。绝不能每窗口都重算——那样真实丢帧
    /// 会被分母吸收掉（丢帧→实测帧率降低→期望值同步降低→丢帧率恒 0）。
    ///
    /// **为什么校准样本用微秒而不是毫秒**：
    /// `gaps` 存的是取整后的毫秒。本板真实帧间隔 53.87ms 会被截成 54ms，
    /// 反推得到 `1000/54 = 18.5 → 19fps`，比真实值高 2.4%。
    /// 校准是**一次性建立基线**，这个偏差会被永久固化下来——38% 的假丢帧
    /// 会变成 36% 的假丢帧，问题没解决只是变小了。所以校准必须用
    /// 原始时长的微秒精度，不能复用 `gaps`。
    std::vector<uint64_t> calibration_gaps_us;   ///< 校准窗口内的高精度帧间隔
    bool calibration_done{false};

    /// 校准失败的原因（非空时写进 describe()，说明为何维持原 fps）
    std::string calibration_note;

    /// 在**校准子窗口**末尾尝试完成一次基线校准
    ///
    /// 调用点：只有 `calibrateTickLocked()`（2026-11-12 自驱线程改造后，
    /// 它由 `captureLoop()` 逐帧推进，不再依赖调用方的快照节奏）。
    /// 返回 true 表示本次调用完成了校准（cfg.fps 已被改写）。
    ///
    /// 判据全部为「不够稳就不采纳」，因为**校准错误比不校准更危险**：
    /// 不校准只是丢帧率偏高（假阳性，运维会去查）；校准错了会让基线本身
    /// 偏移，真实劣化被永久掩盖（假阴性，没人会去查）。
    bool tryCalibrateFps() {
        if (calibration_done) return false;
        if (cfg.fps_calibration_windows == 0) return false;

        // 本校准窗口的帧间隔并入累积池（微秒精度）
        calibration_gaps_us.insert(calibration_gaps_us.end(),
                                   window_gaps_us.begin(), window_gaps_us.end());
        // **由本函数负责清空**，而不是 resetWindow()：window_gaps_us 的归属
        // 是「一个校准窗口」，其边界由采集线程按 cfg.window_ms 自行推进，
        // 与调用方的快照节奏无关（理由见 calibrateTickLocked 的注释）。
        window_gaps_us.clear();

        // 还没攒够窗口数就继续等
        if (++calibration_windows_seen < cfg.fps_calibration_windows) return false;

        calibration_done = true;

        // 样本太少时中位数不可信：至少要覆盖一个窗口的量级。
        // 用 10 作为下限而不是 2——两帧的「中位数」只是均值，没有抗噪能力。
        if (calibration_gaps_us.size() < 10) {
            std::ostringstream os;
            os << "帧率校准跳过：样本仅 " << calibration_gaps_us.size()
               << " 帧，不足 10 帧";
            calibration_note = os.str();
            return false;
        }

        // 中位数：抗首帧/AE 收敛造成的偶发长间隔
        std::vector<uint64_t> sorted = calibration_gaps_us;
        std::sort(sorted.begin(), sorted.end());
        const uint64_t median_us = sorted[sorted.size() / 2];
        if (median_us == 0) {
            calibration_note = "帧率校准跳过：中位帧间隔为 0";
            return false;
        }

        // 离散度检查：p95 / p05 相对中位数的偏差超限就放弃。
        // 不稳定的窗口说明采集本身有问题，拿它当基线会把异常固化成正常。
        const uint64_t p05 = sorted[sorted.size() * 5 / 100];
        const uint64_t p95 = sorted[sorted.size() * 95 / 100];
        const uint64_t max_spread = cfg.fps_calibration_max_spread_percent;
        // 用 128 位避免 us 量级乘 100 溢出（uint64_t 装得下，但显式说明意图）
        const bool p05_ok = (median_us - p05) * 100u <= median_us * max_spread;
        const bool p95_ok = (p95 - median_us) * 100u <= median_us * max_spread;
        if (!p05_ok || !p95_ok) {
            std::ostringstream os;
            os << "帧率校准跳过：帧间隔离散度过大（p05=" << p05 / 1000.0
               << "ms p95=" << p95 / 1000.0 << "ms 中位=" << median_us / 1000.0
               << "ms，上限 " << max_spread << "%）";
            calibration_note = os.str();
            return false;
        }

        // 由中位间隔反推帧率，**微秒精度**。
        //
        // 为什么必须微秒：本板真实间隔 53.875ms。若先把间隔截成 54ms
        // 再算 `1000/54`，得到 19fps（误差 +2.4%）——那是往上偏的方向，
        // 会让假丢帧率变小，掩盖问题。用微秒算出 18.56，再**向下取整**
        // 到 18。
        //
        // 为什么向下取整而不是四舍五入：18.56 四舍五入是 19，而 19
        // 会让 `frames_expected` 偏大、丢帧率偏小——即**少报丢帧**。
        // 本项目纪律是「少报比多报危险」（见 frame_source.hpp 里
        // frames_expected 的注释），所以宁可取 18（丢帧率略偏高）。
        const double measured_fps = 1e6 / static_cast<double>(median_us);

        // 荒谬值一律拒绝：小于 1fps 或大于 1000fps 说明测量本身出了问题
        if (measured_fps < 1.0 || measured_fps > 1000.0) {
            std::ostringstream os;
            os << "帧率校准跳过：推算值 " << measured_fps << "fps 超出合理范围";
            calibration_note = os.str();
            return false;
        }

        // 向下取整：18.56 → 18
        const uint32_t measured = static_cast<uint32_t>(measured_fps);

        // 与整数 fps 相同则不写 note（避免「30fps 校正为 30fps」的噪音）。
        // 注意这里比较的是取整后的值：真实 29.94fps 会取整成 29，
        // 与配置的 30 不同，此时会触发一次「30 → 29」的校正——这是
        // 正确行为，因为驱动声称的 30 确实与实际不符。
        if (measured == cfg.fps) {
            calibration_gaps_us.clear();
            calibration_gaps_us.shrink_to_fit();
            return true;
        }

        std::ostringstream os;
        os << "帧率基线由 " << cfg.fps << "fps 校准为实测 "
           << (median_us / 1000.0) << "ms 对应 " << measured
           << "fps（驱动 G_PARM 回报不可信）";
        calibration_note = os.str();
        cfg.fps = measured;

        // 校准完成后释放样本，避免长期占内存
        calibration_gaps_us.clear();
        calibration_gaps_us.shrink_to_fit();
        return true;
    }

    /// 已观测的窗口数（用于判断是否攒够校准窗口）
    uint32_t calibration_windows_seen{0};

    /// start() 时解析出来的 media 拓扑（可为空 = 没解析到）
    ///
    /// 只读快照，采集期间不再刷新：拓扑在运行中被改动本身就是要排查的异常，
    /// 而每帧去重新枚举会带来不必要的 ioctl 开销。运维改了链路应重启服务。
    MediaTopology topology;

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
            // dma-buf fd 必须逐个 close，否则每次 start 都泄漏 N 个 fd。
            // 注意顺序：先 STREAMOFF（上面已做）再 close，反过来会让驱动
            // 持有的引用与用户态 fd 计数错位。
            for (int f : b.dma_fds) {
                if (f >= 0) ::close(f);
            }
        }
        buffers.clear();
        plane_bufs.clear();
        num_planes = 1;
        buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        // 必须一并复位：否则 stop() 后再 start() 会带着上一次的 memory 类型
        // 去走 REQBUFS，与本次实际分配的缓冲来源不符。
        memory = V4L2_MEMORY_MMAP;
        // buffer_source **不**在这里复位：它是「上一次 start 实际生效的方案」，
        // 停流后 describe() 仍应如实报告当时用的是哪条路。下一次 start()
        // 成功时会覆盖它；start() 失败则保留旧值，比谎报 MMAP 更接近事实。
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    /// ---- 缓冲分配：三条路径，产出统一的 Buffer 表示 ----
    ///
    /// 三条路径的**唯一差别**是像素内存从哪来，帧统计完全不受影响
    /// （只消费 v4l2_buffer 的元数据）。所以这里只关心「怎么拿到可入队的
    /// 内存」，之后的 QBUF/DQBUF 全部共用同一套逻辑。

    /// 释放当前队列（REQBUFS count=0）并清掉所有 fd / 映射
    ///
    /// 回落时必须调用：`VIDIOC_REQBUFS` 在队列非空时返回 EBUSY，
    /// 直接换 memory 类型再申请会失败，而错误信息（"Device or resource busy"）
    /// 完全指不出根因是「上一次申请没释放」。
    void releaseQueue() {
        if (fd >= 0) {
            v4l2_requestbuffers req{};
            req.count = 0;
            req.type = buf_type;
            req.memory = memory;
            // 尽力而为：即使失败也继续清理本地状态，否则 fd 泄漏
            ::ioctl(fd, VIDIOC_REQBUFS, &req);
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
            for (int f : b.dma_fds) {
                if (f >= 0) ::close(f);
            }
        }
        buffers.clear();
        plane_bufs.clear();
    }

    /// 路径 A：MMAP（默认）
    [[nodiscard]] bool allocBuffersMmap(uint32_t count, bool use_mplane,
                                        std::string& err) {
        memory = V4L2_MEMORY_MMAP;
        v4l2_requestbuffers req{};
        req.count = count;
        req.type = buf_type;
        req.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            err = errnoText("VIDIOC_REQBUFS(MMAP)", errno);
            return false;
        }
        if (req.count < 2) {
            err = "驱动返回的 buffer 数不足（" + std::to_string(req.count) + "）";
            return false;
        }

        buffers.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = buf_type;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (use_mplane) {
                plane_bufs.assign(num_planes, v4l2_plane{});
                buf.m.planes = plane_bufs.data();
                buf.length = num_planes;
            }
            if (::ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
                err = errnoText("VIDIOC_QUERYBUF", errno);
                return false;
            }

            if (use_mplane) {
                buffers[i].plane_starts.resize(num_planes, nullptr);
                buffers[i].plane_lengths.resize(num_planes, 0);
                for (uint32_t p = 0; p < num_planes; ++p) {
                    const size_t len = buf.m.planes[p].length;
                    void* mp = ::mmap(nullptr, len, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd,
                                      buf.m.planes[p].m.mem_offset);
                    if (mp == MAP_FAILED) {
                        err = errnoText("mmap(plane)", errno);
                        return false;
                    }
                    buffers[i].plane_starts[p] = mp;
                    buffers[i].plane_lengths[p] = len;
                }
            } else {
                void* p = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, buf.m.offset);
                if (p == MAP_FAILED) {
                    err = errnoText("mmap", errno);
                    return false;
                }
                buffers[i].start = p;
                buffers[i].length = buf.length;
            }
        }
        return true;
    }

    /// 路径 B：DMABUF 导入（从 dma-heap 自己分配）
    ///
    /// 失败时（无 /dev/dma_heap、驱动不接受外部 fd）返回 false，
    /// 调用方回落到导出或 MMAP。**不把失败写进 error**：这三条路径对
    /// 上层是等价的，回落成功就不该在 describe() 里留下一条误导性的偏差记录。
    [[nodiscard]] bool allocBuffersDmaHeap(uint32_t count, bool use_mplane,
                                          std::string& err) {
        memory = V4L2_MEMORY_DMABUF;
        v4l2_requestbuffers req{};
        req.count = count;
        req.type = buf_type;
        req.memory = V4L2_MEMORY_DMABUF;
        if (::ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            err = errnoText("VIDIOC_REQBUFS(DMABUF)", errno);
            return false;
        }
        if (req.count < 2) {
            err = "驱动返回的 DMABUF 数不足";
            return false;
        }

        // 先拿一帧的容量。DMABUF 模式下 QUERYBUF 拿不到驱动预分配的长度，
        // 只有单平面 v4l2_format.sizeimage 可靠；多平面用 planes[p].length
        // 需要先 QUERYBUF，而 DMABUF 的 QUERYBUF 只回填 length 字段本身。
        v4l2_format fmt{};
        fmt.type = buf_type;
        if (::ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
            err = errnoText("VIDIOC_G_FMT", errno);
            return false;
        }

        buffers.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            buffers[i].dma_fds.assign(use_mplane ? num_planes : 1, -1);
            buffers[i].dma_lengths.assign(use_mplane ? num_planes : 1, 0);

            for (uint32_t p = 0; p < buffers[i].dma_fds.size(); ++p) {
                const size_t len =
                    use_mplane ? fmt.fmt.pix_mp.plane_fmt[p].sizeimage
                               : fmt.fmt.pix.sizeimage;
                if (len == 0) {
                    err = "G_FMT 返回的 sizeimage 为 0，无法分配 dma-heap";
                    return false;
                }
                const int dfd = allocFromDmaHeap(len);
                if (dfd < 0) {
                    err = "dma-heap 分配失败（/dev/dma_heap/system 不存在或无权限）";
                    return false;
                }
                buffers[i].dma_fds[p] = dfd;
                buffers[i].dma_lengths[p] = static_cast<uint32_t>(len);
                if (!use_mplane) buffers[i].length = len;   // 供坏帧率判定用
            }
            if (use_mplane) {
                buffers[i].plane_lengths.assign(buffers[i].dma_lengths.begin(),
                                                buffers[i].dma_lengths.end());
            }
        }
        return true;
    }

    /// 路径 C：DMABUF 导出（MMAP 申请 → EXPBUF 拿到 fd → 释放 MMAP 队列）
    ///
    /// 这条路径对本帧源收益为零（见头文件），保留它只作为导入不可用时的
    /// 兜底，用于证明 DMABUF 通路本身是通的。
    ///
    /// **依赖一个非 ABI 保证的内核行为**：`REQBUFS(0)` 释放 MMAP 队列后，
    /// 已导出的 dma-buf fd 仍然有效（vb2 把底层内存的释放推迟到 dma-buf
    /// detach）。实测 6.8 内核成立，但换内核不保证。这也是它只做兜底、
    /// 不做默认的原因。
    [[nodiscard]] bool allocBuffersExport(uint32_t count, bool use_mplane,
                                          std::string& err) {
        // 第一步：按 MMAP 申请并导出 fd
        std::string mmap_err;
        if (!allocBuffersMmap(count, use_mplane, mmap_err)) {
            err = "导出路径的 MMAP 准备阶段失败：" + mmap_err;
            return false;
        }

        // 从 QUERYBUF 拿长度（EXPBUF 需要 index，长度用于后续校验）
        std::vector<std::vector<unsigned int>> lengths(buffers.size());
        for (size_t i = 0; i < buffers.size(); ++i) {
            lengths[i].resize(use_mplane ? num_planes : 1, 0);
            v4l2_buffer buf{};
            buf.type = buf_type;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = static_cast<uint32_t>(i);
            if (use_mplane) {
                plane_bufs.assign(num_planes, v4l2_plane{});
                buf.m.planes = plane_bufs.data();
                buf.length = num_planes;
            }
            if (::ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
                err = errnoText("VIDIOC_QUERYBUF(export)", errno);
                return false;
            }
            if (use_mplane) {
                for (uint32_t p = 0; p < num_planes; ++p) {
                    lengths[i][p] = buf.m.planes[p].length;
                }
            } else {
                lengths[i][0] = buf.length;
            }
        }

        // 第二步：EXPBUF 导出（必须在释放 MMAP 队列之前）
        for (size_t i = 0; i < buffers.size(); ++i) {
            buffers[i].dma_fds.assign(use_mplane ? num_planes : 1, -1);
            buffers[i].dma_lengths.assign(use_mplane ? num_planes : 1, 0);
            for (uint32_t p = 0; p < buffers[i].dma_fds.size(); ++p) {
                v4l2_exportbuffer exp{};
                exp.type = buf_type;
                exp.index = static_cast<uint32_t>(i);
                // 多平面导出用 plane 字段指定平面
                exp.plane = use_mplane ? p : 0;
                exp.flags = O_RDWR | O_CLOEXEC;
                if (::ioctl(fd, VIDIOC_EXPBUF, &exp) < 0) {
                    err = errnoText("VIDIOC_EXPBUF", errno);
                    return false;
                }
                buffers[i].dma_fds[p] = exp.fd;
                buffers[i].dma_lengths[p] = lengths[i][p];
            }
        }

        // 第三步：释放 MMAP 队列（保留已导出的 fd），再以 DMABUF 重新申请
        //
        // 注意这里**不能**调 releaseQueue()：它会连 dma_fds 一起 close 掉。
        {
            v4l2_requestbuffers req{};
            req.count = 0;
            req.type = buf_type;
            req.memory = V4L2_MEMORY_MMAP;
            ::ioctl(fd, VIDIOC_REQBUFS, &req);
        }
        // 只清映射，保留 fd
        for (auto& b : buffers) {
            if (b.start != nullptr && b.start != MAP_FAILED) {
                ::munmap(b.start, b.length);
                b.start = nullptr;
            }
            for (size_t p = 0; p < b.plane_starts.size(); ++p) {
                if (b.plane_starts[p] != nullptr && b.plane_starts[p] != MAP_FAILED) {
                    ::munmap(b.plane_starts[p], b.plane_lengths[p]);
                }
            }
            b.plane_starts.clear();
            b.plane_lengths.clear();
        }
        // 把长度拷进 dma_lengths 后，重新填 length 供坏帧率判定
        for (auto& b : buffers) {
            if (use_mplane) {
                b.plane_lengths.assign(b.dma_lengths.begin(), b.dma_lengths.end());
            } else {
                b.length = b.dma_lengths[0];
            }
        }

        memory = V4L2_MEMORY_DMABUF;
        v4l2_requestbuffers req{};
        req.count = static_cast<uint32_t>(buffers.size());
        req.type = buf_type;
        req.memory = V4L2_MEMORY_DMABUF;
        if (::ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            err = errnoText("VIDIOC_REQBUFS(DMABUF after export)", errno);
            return false;
        }
        if (req.count < buffers.size()) {
            buffers.resize(req.count);
        }
        return true;
    }

    /// 把当前 buffers 全部入队（三条路径共用）
    [[nodiscard]] bool queueAll(bool use_mplane, std::string& err) {
        for (uint32_t i = 0; i < buffers.size(); ++i) {
            v4l2_buffer buf{};
            buf.type = buf_type;
            buf.memory = memory;
            buf.index = i;
            if (memory == V4L2_MEMORY_DMABUF) {
                if (use_mplane) {
                    plane_bufs.assign(num_planes, v4l2_plane{});
                    for (uint32_t p = 0; p < num_planes; ++p) {
                        plane_bufs[p].m.fd = buffers[i].dma_fds[p];
                    }
                    buf.m.planes = plane_bufs.data();
                    buf.length = num_planes;
                } else {
                    buf.m.fd = buffers[i].dma_fds[0];
                }
            } else if (use_mplane) {
                plane_bufs.assign(num_planes, v4l2_plane{});
                buf.m.planes = plane_bufs.data();
                buf.length = num_planes;
            }
            if (::ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                err = errnoText("VIDIOC_QBUF", errno);
                return false;
            }
        }
        return true;
    }

    /// 按配置选择路径分配缓冲并入队
    ///
    /// 回落顺序：导入 → 导出 → MMAP。每一级失败都先 releaseQueue() 再试
    /// 下一级，否则残留队列会让下一级 REQBUFS 返回 EBUSY。
    [[nodiscard]] bool setupBuffers(bool use_mplane, std::string& err) {
        const uint32_t want = cfg.buffer_count ? cfg.buffer_count : 4;

        if (cfg.use_dmabuf) {
            // 一级：导入
            if (allocBuffersDmaHeap(want, use_mplane, err)) {
                if (queueAll(use_mplane, err)) {
                    buffer_source = kMemDmabufHeap;
                    return true;
                }
                releaseQueue();
            }
            // 一级失败是预期内的（板上无 dma-heap），不写进 error
            releaseQueue();

            // 二级：导出
            if (allocBuffersExport(want, use_mplane, err)) {
                if (queueAll(use_mplane, err)) {
                    buffer_source = kMemDmabufExport;
                    return true;
                }
                releaseQueue();
            }
            releaseQueue();
            // 导出也失败时，把两级的原因留下来供 describe() 溯源
            err = "DMABUF 不可用，已回落 MMAP（" + err + "）";
            buffer_source = kMemDmabufMmapFallback;
        } else {
            buffer_source = kMemMmap;
        }

        // 三级：MMAP（默认路径）
        if (!allocBuffersMmap(want, use_mplane, err)) {
            return false;
        }
        return queueAll(use_mplane, err);
    }

    // ========================================================================
    // 自驱采集线程
    // ========================================================================

    /// 采集线程主体：持续 DQBUF/QBUF，直到被要求停止
    ///
    /// 与改造前 `snapshotAndReset()` 里那段排空循环的差别只在**退出条件**：
    ///
    ///   旧：跑到 `window_start + window_ms` 就退出（窗口由调用方节奏决定）
    ///   新：跑到 `thread_should_stop` 置位才退出（窗口与采集彻底解耦）
    ///
    /// 为什么必须**持续**取而不是按窗口取：驱动只有 `buffer_count`（默认 4）
    /// 个缓冲。不取，队列几毫秒内就填满，之后驱动**丢弃**新帧——那是我们
    /// 自己造成的丢帧，却会被 `frames_expected - frames_received` 记成
    /// 「链路丢帧」。本板实测：1Hz 轮询 + 4 buffer @30fps 恒定报 87% 丢帧，
    /// 而链路本身完好。这不是精度问题，是把观测缺陷栽给链路的问题。
    ///
    /// 睡眠策略：`select()` 超时设 200ms。不是 0（纯自旋会吃掉一个核），
    /// 也不是窗口长度（那又回到「低频批量取」，缓冲照样会满）。200ms 远大于
    /// 帧周期（30fps 时 33ms），所以正常情况下每次 select 立刻返回可读，
    /// 只在无帧时才真正睡满——既不让 CPU 空转，也不会因取帧太慢而丢帧。
    void captureLoop() {
        const bool use_mplane = (buf_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

        // 线程启动时**拷一份** sink 的本地副本。
        //
        // 采集线程从头到尾用这一份：`setPixelSink` 在线程运行期间本来就
        // 拒绝注册（见其实现），但把它读成本地副本让「这条线程不会看到
        // 中途换人」成为编译期可见的事实，而不是依赖调用方的自觉。
        const PixelSink sink = pixel_sink;

        while (!thread_should_stop.load(std::memory_order_relaxed)) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            // 超时 200ms：既保证停止请求能在 200ms 内被响应，
            // 又远大于帧周期，不至于因等待过久让缓冲填满。
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 200000;

            const int r = ::select(fd + 1, &fds, nullptr, nullptr, &tv);
            if (r < 0) {
                if (errno == EINTR) continue;   // 被信号打断，重试
                std::lock_guard<std::mutex> lk(mtx);
                error = errnoText("select", errno);
                thread_fatal = true;
                return;
            }
            if (r == 0) continue;               // 超时无帧：正常（可能停了流），继续等

            // 每帧一个局部 v4l2_buffer / plane 数组：这里**不能**复用
            // Impl::plane_bufs，因为每帧都要重新 assign，复用也只是省一次
            // 小分配，却要引入「谁在用这块内存」的隐式约束。
            v4l2_buffer buf{};
            buf.type = buf_type;
            buf.memory = memory;
            std::vector<v4l2_plane> planes;
            if (use_mplane) {
                planes.assign(num_planes, v4l2_plane{});
                buf.m.planes = planes.data();
                buf.length = num_planes;
            }
            if (::ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN) continue;  // 竞态：没准备好，再等
                std::lock_guard<std::mutex> lk(mtx);
                error = errnoText("VIDIOC_DQBUF", errno);
                thread_fatal = true;
                return;
            }

            // 出队时刻要在 DQBUF 返回后立刻取，越晚取延迟被高估得越多。
            const auto dequeue_t = std::chrono::steady_clock::now();

            // ---- 单帧事实累计（持锁）----
            //
            // 锁只覆盖累计，不覆盖 ioctl：ioctl 可能耗时几十微秒，
            // 把它们放进临界区会让 snapshotAndReset() 的调用方跟着等，
            // 而它们读的数据此时还没准备好。
            {
                std::lock_guard<std::mutex> lk(mtx);

                uint64_t frame_bytes = 0;
                uint64_t frame_capacity = 0;
                if (use_mplane) {
                    for (uint32_t p = 0; p < num_planes && p < buf.length; ++p) {
                        frame_bytes += planes[p].bytesused;
                    }
                    if (buf.index < buffers.size()) {
                        for (size_t len : buffers[buf.index].plane_lengths) {
                            frame_capacity += len;
                        }
                    }
                } else {
                    frame_bytes = buf.bytesused;
                    if (buf.index < buffers.size()) {
                        frame_capacity = buffers[buf.index].length;
                    }
                }

                ++frames_received;
                bytes_received += frame_bytes;

                // 不完整帧：实际字节明显小于容量即视为 corrupt。
                // 用比例而非绝对值，避免分辨率变化后阈值失效。
                if (frame_capacity > 0) {
                    const double ratio = static_cast<double>(frame_bytes) /
                                         static_cast<double>(frame_capacity);
                    if (ratio < 0.5) ++frames_corrupt;
                }

                // ---- 帧间隔：用内核时间戳 ----
                //
                // timestamp 可能是 MONOTONIC 或 MONOTONIC_RAW。两者的**差值**都
                // 是单调的，所以这里不需要区分来源；只有跨设备对齐时才需要。
                const auto frame_t = std::chrono::steady_clock::time_point(
                    std::chrono::seconds(buf.timestamp.tv_sec) +
                    std::chrono::microseconds(buf.timestamp.tv_usec));

                if (has_last_frame_time) {
                    const auto gap = frame_t - last_frame_time;
                    const auto gap_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(gap)
                            .count();
                    if (gap_ms >= 0) {
                        gaps.push_back(static_cast<uint32_t>(gap_ms));
                        // 同一间隔的微秒精度副本，只喂给帧率基线校准。
                        // 负间隔（时间戳回退）两边都不收：钳到 0 会把观测错误
                        // 伪装成「间隔极短」，是最坏的一种假数据。
                        window_gaps_us.push_back(
                            static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(gap)
                                    .count()));
                    }
                }
                last_frame_time = frame_t;
                has_last_frame_time = true;

                // ---- 出队延迟：帧在队列里等了多久 ----
                //
                // 负数（时钟域不一致或驱动时间戳异常）直接丢弃，不钳到 0：
                // 钳 0 会把观测错误伪装成「延迟极低」，那是最坏的一种假数据。
                {
                    const auto lat =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            dequeue_t - frame_t)
                            .count();
                    if (lat >= 0 && lat < 60000) {   // 超过 60s 视为时间戳异常
                        dequeue_latencies.push_back(static_cast<uint32_t>(lat));
                    }
                }

                // ---- 序号跳变：按**事件**计，不按帧计 ----
                //
                // 一次跳 30 与三十次各跳 1，总量相同但 QoE 不同，
                // 所以这里只 +1，具体丢了多少帧由 expected - received 表达。
                //
                // 回绕处理：sequence 是 32 位无符号，30fps 下约 4.97 年回绕一次。
                // 判据是「比上一个序号还小且差距极大」——不是任何变小都算回绕，
                // 正常重排的差值很小。
                if (has_last_seq && buf.sequence != last_seq + 1) {
                    // 注意 has_last_seq **不再**由 resetWindow() 每个快照清掉
                    // （2026-11-12 自驱线程改造，理由见 resetWindow 的注释）。
                    // 采集连续运行，本快照首帧与上一快照末帧之间若真的跳号，
                    // 那是**真实**的丢帧，必须如实记下来；清掉它反而是把一个
                    // 真实故障藏起来。healthy 流上两者恰好连号，不会产生假
                    // gap——这也是改造前不得不清、改造后必须不清的原因。
                    const bool wrapped =
                        buf.sequence < last_seq &&
                        (static_cast<uint64_t>(last_seq) - buf.sequence) >
                            0x80000000ull;
                    if (!wrapped && buf.sequence > last_seq) ++seq_gap_count;
                    // 序号回绕 / 重排不记为 gap：那是驱动行为，不是丢帧。
                }
                last_seq = buf.sequence;
                has_last_seq = true;

                // 校准子窗口推进：**必须在这里**（持锁、每帧一次）而不是在
                // snapshotAndReset() 里。理由是校准窗口的边界由采集节奏决定，
                // 而采集节奏与调用方的取数节奏已彻底解耦（见 calibrateTickLocked
                // 的注释）。每帧调一次的代价是一次 steady_clock::now() 与几个
                // 整数比较，18fps 下完全可忽略。
                calibrateTickLocked(dequeue_t,
                                    std::chrono::steady_clock::time_point{});
            }

            // ---- 交付像素（在归还 buffer 之前！）----
            //
            // 位置是硬约束：`VDIOC_QBUF` 一归还，驱动就可能立刻往这块内存写
            // 下一帧。回调必须在 QBUF **之前**跑完，且回调里不得保留指针。
            //
            // 为什么放在锁外：中枢的 publish() 会做一次 3 MB 的 memcpy
            // （限速到 10fps），放在 mtx 里会让 snapshotAndReset() 的调用方
            // 跟着等。帧统计与像素交付是两件独立的事，不该互相阻塞。
            //
            // 为什么 sink 为空时零开销：一个 bool 判断，没有虚调用。
            if (sink) {
                const void* px = nullptr;
                size_t px_size = 0;
                if (use_mplane) {
                    // 多平面：首平面（Y）在 plane_starts[0] / planes[0]
                    if (buf.index < buffers.size() &&
                        !buffers[buf.index].plane_starts.empty()) {
                        px = buffers[buf.index].plane_starts[0];
                    } else if (buf.index < buffers.size() &&
                               !buffers[buf.index].dma_fds.empty()) {
                        // DMABUF 导入路径没有映射地址。此时不交付像素——
                        // 中枢是「拷一份快照」模型，没有 CPU 可读地址就无法拷。
                        // 不在这里临时 mmap：那条路会在每帧制造一次映射开销，
                        // 而且与 use_dmabuf 的设计意图（零拷贝给硬件）相悖。
                        px = nullptr;
                    }
                    if (px != nullptr && buf.length > 0 && !planes.empty()) {
                        px_size = planes[0].bytesused;
                    }
                } else {
                    if (buf.index < buffers.size()) {
                        px = buffers[buf.index].start;
                    }
                    px_size = buf.bytesused;
                }

                if (px != nullptr && px_size > 0) {
                    PixelFrame pf;
                    pf.data = px;
                    pf.size = px_size;
                    pf.width = cfg.width;
                    pf.height = cfg.height;
                    pf.stride = frame_stride;
                    pf.uv_offset = frame_uv_offset;
                    pf.fourcc = frame_fourcc;
                    pf.sequence = buf.sequence;
                    pf.timestamp_ns = static_cast<uint64_t>(buf.timestamp.tv_sec) * 1000000000ull +
                                      static_cast<uint64_t>(buf.timestamp.tv_usec) * 1000ull;
                    pf.multiplanar = use_mplane;
                    // 回调里若抛异常，绝不能让采集线程死掉——那会被上层
                    // 看成「摄像头坏了」，而真正的问题在消费者。
                    try {
                        sink(pf);
                    } catch (...) {
                        // 静默吞掉：中枢内部的失败已由它自己的 stats 记录，
                        // 这里再记一条只会在每帧刷屏。
                    }
                }
            }

            // ---- 归还 buffer（不持锁）----
            //
            // **刻意构造一个全新的 v4l2_buffer，而不是复用 DQBUF 写过的那个。**
            // 实测（vivid 6.8 单平面 DMABUF）DQBUF 后 `buf.m.fd` 会被原样保留，
            // 于是「忘了重新绑定 fd」也能 requeue 成功。但 V4L2 从未保证这点：
            // 换个摄像头就变成「第一帧之后全部 EINVAL」，且错误点离根因很远。
            //
            // 构造新结构体后 fd 必须由我们自己填，这条路径才真正承重：
            // 漏填就是 fd=0，内核在 QBUF 阶段校验并返回 EINVAL（已实测确认）。
            v4l2_buffer requeue{};
            requeue.type = buf_type;
            requeue.memory = memory;
            requeue.index = buf.index;
            std::vector<v4l2_plane> requeue_planes;
            if (use_mplane) {
                requeue_planes.assign(num_planes, v4l2_plane{});
                if (memory == V4L2_MEMORY_DMABUF && buf.index < buffers.size()) {
                    // 多平面 DMABUF：每个 plane 各绑一个 fd。这里若漏填，
                    // plane_bufs 全是 0，内核会以 EINVAL 拒绝——不是静默错误。
                    for (uint32_t p = 0;
                         p < num_planes && p < buffers[buf.index].dma_fds.size(); ++p) {
                        requeue_planes[p].m.fd = buffers[buf.index].dma_fds[p];
                    }
                }
                requeue.m.planes = requeue_planes.data();
                requeue.length = num_planes;
            } else if (memory == V4L2_MEMORY_DMABUF && buf.index < buffers.size()) {
                requeue.m.fd = buffers[buf.index].dma_fds[0];
            }
            if (::ioctl(fd, VIDIOC_QBUF, &requeue) < 0) {
                std::lock_guard<std::mutex> lk(mtx);
                error = errnoText("VIDIOC_QBUF(requeue)", errno);
                thread_fatal = true;
                return;
            }
        }
    }

    /// 启动采集线程（调用方已确保未启动）
    void startCaptureThread() {
        thread_should_stop.store(false, std::memory_order_relaxed);
        thread_fatal = false;
        thread_active = true;
        capture_running.store(true, std::memory_order_release);
        capture_thread = std::thread([this] { captureLoop(); });
    }

    /// 停止采集线程并 join
    ///
    /// **置位后必须 join**，不能只置标志就返回：线程还在跑时 closeDevice()
    /// 会把 fd 关掉，而线程正拿它做 ioctl——那是 use-after-close。
    void stopCaptureThread() {
        if (!thread_active) return;
        thread_should_stop.store(true, std::memory_order_relaxed);
        if (capture_thread.joinable()) capture_thread.join();
        thread_active = false;
        // 线程已经 join，再清这个标志：它同时是 setPixelSink 的准入判据，
        // 置位期间注册会被拒绝。先 join 再放开，避免有人在线程末帧里
        // 换掉 sink 却在下一帧用法完全不同。
        capture_running.store(false, std::memory_order_release);
    }

    /// 按 `cfg.window_ms` 推进一个**校准子窗口**（调用方须持有 mtx）
    ///
    /// 为什么单独一个边界：校准要求「若干个等长的窗口」，而这个边界必须由
    /// **采集节奏**决定，不能跟着调用方的快照节奏走。改造后快照跨度是变化的
    /// （可能 200ms，也可能 120s），拿它当校准窗口会让 `fps_calibration_windows`
    /// 这个配置项失去意义——同一个 3 在 15s 的取数节奏下变成 45s 的样本。
    ///
    /// 校准只在启动后的最初几个窗口进行，之后 `tryCalibrateFps()` 直接返回
    /// false，本函数退化为一次 `calibration_done` 判断，开销可忽略。
    void calibrateTickLocked(std::chrono::steady_clock::time_point now,
                             std::chrono::steady_clock::time_point fallback_start) {
        if (calibration_done) return;
        if (cfg.fps_calibration_windows == 0) return;
        if (cfg.window_ms == 0) return;   // 未声明窗口：无法定义子窗口，放弃校准

        if (calibration_window_start == std::chrono::steady_clock::time_point{}) {
            // 首个校准窗口从「上一次快照的起点」算起：那一时刻正是本窗口
            // 帧间隔开始累积的地方（`resetWindow()` 在 start() 里设过）。
            calibration_window_start =
                (fallback_start == std::chrono::steady_clock::time_point{})
                    ? now
                    : fallback_start;
            return;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - calibration_window_start);
        if (elapsed.count() < static_cast<long long>(cfg.window_ms)) return;

        // 一个校准子窗口到期：结算并重新起算。
        // 注意 tryCalibrateFps() 消费 window_gaps_us 并自行清空，
        // 所以这里不需要（也不能）再清一次。
        tryCalibrateFps();
        calibration_window_start = now;
    }

    /// 清空所有本窗口的累计量
    ///
    /// ========================================================================
    /// 边界语义（2026-11-12 自驱线程改造后发生变化）
    /// ========================================================================
    ///
    /// 改造前窗口由调用方节奏切分，所以「跨窗口状态」必须一起清：
    /// 本窗口首帧与上一窗口末帧比较会产生假 seq_gap 与假帧间隔（实测恒定
    /// 每窗口 seq_gap=1、max_gap≈900ms）。
    ///
    /// 改造后采集线程**连续**运行，快照只是「取走累计值」，不再切断采集。
    /// 此时：
    ///   - `has_last_seq` / `has_last_frame_time` / `last_frame_time`
    ///     **不能被清**。它们现在是连续的：跨快照边界的那一帧间隔是**真实**
    ///     的（两帧相邻到达），把它丢掉反而会让 `frames_expected` 与实测
    ///     间隔对不上。清除它们还会**藏起**跨边界的真实序号跳变。
    ///   - 一个边界效应仍在：边界那一帧的间隔会被计入**后**一个快照，而它的
    ///     前半段落在前一个快照。跨度通常 ≥ 15s 而帧间隔 ~54ms，误差
    ///     0.04% 量级，可以忽略；显式记在这里是为了不让后来者以为漏了。
    void resetWindow() {
        frames_received = 0;
        bytes_received = 0;
        frames_corrupt = 0;
        seq_gap_count = 0;
        gaps.clear();
        dequeue_latencies.clear();
        // `window_gaps_us` **不在这里清**：它的归属是「一个校准窗口」，
        // 由 calibrateTickLocked() 按 cfg.window_ms 推进、由 tryCalibrateFps()
        // 消费并清空。若跟着快照清，调用方取得比 window_ms 快时校准永远
        // 攒不够样本。
        window_start = std::chrono::steady_clock::now();
    }
};

V4l2FrameSource::V4l2FrameSource(V4l2CaptureConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(config);
}

/// 析构必须先停线程再销毁 Impl
///
/// 默认实现的 `~Impl()` 里只有 `closeDevice()`，它不知道采集线程的存在。
/// 线程对象在 `std::thread` 析构时若仍 joinable 会直接 `std::terminate`，
/// 而构造函数之外的地方（异常路径、用户忘记调 stop()）恰好最容易命中。
/// 所以这里显式停：停 → join → 然后才轮到 Impl 析构关设备。
V4l2FrameSource::~V4l2FrameSource() {
    if (impl_) impl_->stopCaptureThread();
}

bool V4l2FrameSource::start() {
    Impl& s = *impl_;

    // 幂等：已启动时直接返回成功，不重复打开
    if (s.streaming) return true;

    s.error.clear();
    // 与 error 一起清：两者都是「本次 start 的偏差记录」，
    // 留着上一次的会让新一轮的 describe() 谎报。
    s.input_note.clear();
    // 校准状态必须随 start() 一起复位：重新启动可能换了分辨率、换了设备，
    // 上一次的帧率基线不再适用。若保留，会把旧基线当成本次事实继续使用。
    s.calibration_gaps_us.clear();
    s.calibration_done = false;
    s.calibration_note.clear();
    s.calibration_windows_seen = 0;
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
    // 同步记下「本次实际生效的平面模型」。必须在**探测成功之后立刻**写，
    // 不能等 start() 末尾——中途任何一处失败 return 时，describe() 仍应
    // 说出「当时探测到的是哪种」，那正是排查失败原因需要的线索。
    s.started_mplane = use_mplane;
    s.num_planes = use_mplane ? 2u : 1u;   // NV12 多平面是 Y + UV 两平面

    // ---- 2.8 选择输入（必须在 S_FMT 之前）----
    //
    // ========================================================================
    // 这一步不是可选的「锦上添花」，它是 sunxi-vin 上能否出帧的前提
    // ========================================================================
    //
    // 真机现象：Radxa Cubie A7A（sun60iw2 / IMX219）上，`S_FMT` 必返回
    // `EINVAL`，dmesg 里有
    //   `WARNING ... at vin_video.c:820 vin_pipeline_try_format`
    // `ENUM_FRAMESIZES` 还会返回 `EPERM`（root 下同样，所以不是权限问题）。
    //
    // 逐行读 BSP（分支 cubie-aiot-v1.4.6）后的因果链：
    //
    //   1. `vin_pipeline_try_format()` 取 `sd = pipe.sd[VIN_IND_SENSOR]`，
    //      随后 `if (WARN_ON(!sd || !tfmt || !fmt_id)) return -EINVAL;`
    //      —— 这是**空指针检查**，不是格式比较。WARN 命中说明 sd 为 NULL。
    //   2. `pipe.sd[]` 唯一由 `vin_md_prepare_pipeline()` 填充（`vin.c:83`，
    //      写入在 `:119`）。它靠 `media_pad_remote_pad_first()` 逐跳上溯，
    //      **只走 ENABLED 链路**。
    //   3. `vin.c:2035` 建 sensor→mipi 链路时传 `flags = 0`（disabled），
    //      而同函数其它链路全带 `MEDIA_LNK_FL_ENABLED`。于是 probe 期那次
    //      填充在 mipi 这一跳就 `break`，`pipe.sd[VIN_IND_SENSOR]` 保持 NULL。
    //   4. 用户态唯一能补这次填充的入口就是 `VIDIOC_S_INPUT`：
    //      `__vin_s_input()` → `__vin_sensor_setup_link(..., 1)` →
    //      `vin_pipeline_call(open, ..., prepare = true)`，而 prepare=true
    //      正是重跑 `vin_md_prepare_pipeline()` 的开关（`vin_video.c:2412`）。
    //
    // **为什么不能靠 STREAMON 代替**：`vidioc_streamon` 也调用了
    // `__vin_sensor_setup_link`，但**不**带 `prepare=true`，`pipe.sd[]` 仍是
    // NULL，随后的 `REQBUFS`/`QBUF` 一样会失败。实测顺序上也来不及——
    // `S_FMT` 排在 `STREAMON` 前面，早就返回 EINVAL 了。
    //
    // **为什么不能靠手动 `MEDIA_IOC_SETUP_LINK` 代替**：那条路径只把链路
    // 置 ENABLED，不触发 `vin_md_prepare_pipeline()`，`pipe.sd[]` 还是 NULL。
    // 前一轮排查正是卡在这里（链路全绿了 S_FMT 依旧 EINVAL），因此
    // 本实现坚持只读拓扑、绝不自己改链路标志。
    //
    // ========================================================================
    // 但它是「可选 ioctl」，失败绝不能判死设备
    // ========================================================================
    //
    // 标准 V4L2 里 `VIDIOC_S_INPUT` 是可选的：UVC 摄像头、vim2m 之类的
    // mem2mem 节点都返回 `ENOTTY`，多数单输入设备返回 `0`，少数在无输入时
    // 返回 `EINVAL`。把它们当成致命错误会让本来能用的设备完全无法采集——
    // 这与本项目「缺数据不得伪装成健康」的纪律是两件事：这里是「能力不存在」，
    // 正确反应是跳过，而不是报错。
    //
    // 因此：`ENOTTY` / `EINVAL` 静默跳过（这是设备没有输入概念的正常形态，
    // 与上面 `G_PARM` 的处理同一原则，不写进 describe() 制造噪音）；
    // 其它 errno 仍然不中断启动，但记进 `input_note`——它是真机上
    // 「S_FMT 报 EINVAL 却看不出为什么」的唯一线索，必须留下来。
    {
        int input = 0;
        if (::ioctl(s.fd, VIDIOC_S_INPUT, &input) < 0) {
            const int err = errno;
            if (err != ENOTTY && err != EINVAL) {
                s.input_note = errnoText("VIDIOC_S_INPUT", err);
            }
        }
    }

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

    // ---- 记录交付几何（stride / uv_offset / fourcc / sizeimage）----
    //
    // ========================================================================
    // 这一段是本文件最容易写错的地方，两处都已用真机实测证伪过旧结论
    // ========================================================================
    //
    // 1) `bytesperline` **不可信**，必须先过自洽性检查。
    //
    //    sunxi-vin 的 `vin_video.c:505` 是
    //        bytesperline[0] = o_width * depth[0] / 8
    //    对 NV12 而言 depth[0]=12（亮度+色度共 12 位/像素），于是 1640 宽得到
    //    **2460**。但 2460 是「整帧按每像素 12 位算的每行字节数」，对 planar
    //    NV12 的逐行跨度毫无意义。板端实测（2026-11-14，1640x1232）：
    //        bytesperline=2460 → 2460*1232*1.5 = 4,546,080 > sizeimage=3,045,504
    //    一个装不进自己 buffer 的 stride 不可能是真的。判据就用这个：
    //    **装不下即否决**，回落 width 并按紧凑排布处理。
    //
    //    真实 stride 由行间距微扫独立确认：扫 1632..1662 逐个算「相邻行同列
    //    绝对差」，pitch=1640 取尖锐极小值 1.1795，其它偶数 pitch 都在
    //    1.54~1.59，奇数 pitch 跳到 4.6 以上。所以真实 stride = width = 1640。
    //
    // 2) UV 平面**不紧跟** `stride * height`，中间有一块对齐空洞。
    //
    //    驱动算 size 用的是
    //        pix_size = ALIGN(o_width,16) * ALIGN(o_height,16)      (vin_video.c:216,512)
    //        payload[0] = pix_size * depth / 8                       → 整帧字节数
    //    即 UV 起始 = ALIGN(w,16)*ALIGN(h,16) = 2030336，而
    //    stride*height = 1640*1232 = 2020480，差 9856 字节（= 1232*8）。
    //
    //    实测该区间**全零**（不是图像内容），且尾部 [3040576, 3045504) 的
    //    4928 字节同样全零。零字节游程扫描给出的正是这两段，位置在两帧上
    //    完全一致，说明是稳定布局而非偶然。
    //
    //    对 NV12 有 payload = Y + UV = 3/2 × Y，所以 Y 平面字节数 = payload×2/3
    //    = UV 起始偏移。这个关系不依赖 ALIGN 常量具体是 16 还是别的值，
    //    但**要用两条约束验证后才采信**（见下面的 guards）。
    // ========================================================================
    // 推导交付几何。注意这里 sizeimage 可能还是 0（驱动只在 REQBUFS 的
    // queue_setup 里才填 payload），所以 start() 在 setupBuffers() 之后
    // 会用同一方法**再算一次**。两处共用一份逻辑，避免判断标准漂移。
    s.deriveGeometry(fmt, use_mplane, got_w, got_h, got_fmt);
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
    // 实测证据（vivid，另起进程把驱动设成 15fps，本进程配置 30fps 且
    // set_frame_rate=false）：
    //   expected=30 received=15 lost=15 loss_ratio=0.5000
    // 驱动帧率状态**跨进程保留**，所以这个坑会在「上一步设过帧率」之后突然出现。
    // 开启 S_PARM 后同一设备 expected=30 received=30 loss_ratio=0。
    //
    // 驱动不支持 S_PARM 不算失败：很多 sensor 的帧率由链路固定。此时记录
    // 实际帧率并**用它回填 cfg.fps**，让期望值与实际一致，而不是继续用
    // 配置值去算一个假丢帧率。
    if (s.cfg.set_frame_rate) {
        v4l2_streamparm parm{};
        parm.type = s.buf_type;
        if (::ioctl(s.fd, VIDIOC_G_PARM, &parm) < 0) {
            // **不能在这里 return false**：部分驱动（实测 vimc 的 capture 节点
            // 报 ENOTTY）根本不实现 G_PARM，而帧率由链路硬件固定。此时正确
            // 行为是沿用配置帧率继续采集，而不是拒绝启动。
            //
            // 这与本函数上方注释自己写的话一致（「驱动不支持 S_PARM 不算失败」），
            // 但此前代码在 G_PARM 处提前返回，注释与实现互相矛盾——到了真机上，
            // 这会让一个本来能出帧的节点完全无法使用。
            //
            // 记录原因但**不当作 error** 塞进 describe()：ENOTTY 在这里是
            // 正常情况，写进偏差记录反而会让读者以为出了问题。
            parm = v4l2_streamparm{};
            parm.type = s.buf_type;
            // timeperframe 保持 0/0，下面的判定会跳过回填，cfg.fps 维持配置值
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

    // ---- 4/5. 申请缓冲并入队 ----
    //
    // 三条路径（DMABUF 导入 / DMABUF 导出 / MMAP）的回落逻辑都在
    // Impl::setupBuffers() 里，本函数不再区分。选择哪条路只影响像素内存
    // 来自哪里，不影响 FrameStats 的语义——统计全部来自 v4l2_buffer 的元数据。
    {
        std::string buf_err;
        if (!s.setupBuffers(use_mplane, buf_err)) {
            s.error = buf_err;
            s.closeDevice();
            return false;
        }
    }

    // ---- 5.5 重新读一次几何（**顺序是硬约束**）----
    //
    // ========================================================================
    // 为什么必须在这里重读：S_FMT 之后读到的是**自洽的错值**
    // ========================================================================
    //
    // 真机实测（2026-11-14，1640x1232 NV12，开 server.log_level=debug）：
    //
    //   sizeimage=3030720 bytesperline=2460 -> stride=1640   ← S_FMT 之后
    //   sizeimage=3045504 bytesperline=2460 -> stride=1640   ← REQBUFS 之后
    //
    // 第一次的 3030720 = 1640*1232*1.5，恰好是「紧凑 NV12 整帧」字节数，
    // 比真值 3045504 只小 14784（= 两块对齐空洞之和）。它不是 0，因此
    // **所有以 sizeimage 为前提的 guard 都会通过**：
    //   cand = 3030720*2/3 = 2020480 = stride*height  → cand >= uv_off（等号）
    //   cand + uv_bytes   = 3030720 = sizeimage       → 装得下（等号）
    // 两道 guard 都卡在等号上通过，于是「按紧凑布局」被"证明"成正确答案，
    // 而驱动实际给的是 Y 与 UV 之间夹 9856 字节空洞的布局。
    //
    // 真值来自 queue_setup()（VB2 在 VIDIOC_REQBUFS 时回调）：
    //   vin_video.c:512  size       = ALIGN(w,16) * ALIGN(h,16)  = 2030336
    //   vin_video.c:542  payload[0] = size * depth / 8           = 3045504
    //   vin_video.c:800  plane_fmt[0].sizeimage = payload[0]     ← 只在这里回填
    //
    // 所以几何推导必须放在 REQBUFS **之后**。这里重读 G_FMT 覆盖掉
    // S_FMT 阶段那份「看起来对但数值错」的结果。失败不致命（保留旧值），
    // 但记 error —— 不能静默。
    if (::ioctl(s.fd, VIDIOC_G_FMT, &fmt) == 0) {
        s.deriveGeometry(fmt, use_mplane, got_w, got_h, got_fmt);
    } else {
        s.error = errnoText("VIDIOC_G_FMT(after REQBUFS)", errno);
    }

    // ---- 6. 开始流 ----
    v4l2_buf_type type = s.buf_type;
    if (::ioctl(s.fd, VIDIOC_STREAMON, &type) < 0) {
        s.error = errnoText("VIDIOC_STREAMON", errno);
        s.closeDevice();
        return false;
    }
    s.streaming = true;

    // ---- 7. 解析 media 拓扑（只读、失败不影响采集）----
    //
    // 放在 STREAMON 之后：此刻设备已确认可用，拓扑信息才有意义。
    // 失败一律忽略——拓扑是诊断增强，拿不到不该让采集失败。
    // 真机上这正是判断「节点打开了但上游链路 DISABLED」的唯一依据。
    if (s.cfg.probe_media_topology) {
        MediaTopology topo;
        if (probeMediaTopology(s.cfg.device, topo)) {
            s.topology = std::move(topo);
        }
    }

    // ---- 8. 启动自驱采集线程 ----
    //
    // **必须在 STREAMON 之后、返回 true 之前**。
    //
    // 在 STREAMON 之前启动的话，线程的第一次 select 会看到 fd 可读？
    // 不会——没 STREAMON 时队列里没有待取帧，select 会睡到超时。真正的问题
    // 在于反方向：STREAMON 之后到线程起来之前，驱动已经开始往 4 个 buffer
    // 里写帧，而缓冲只有 4 个——这段间隙里攒满 4 帧后驱动就开始丢弃。
    // 所以要在 STREAMON 之后**立刻**启动，把间隙压到最小。
    //
    // 启动失败的处置：`std::thread` 构造抛 system_error（资源不足）。
    // 此时不谎报成功——关设备、记 error、返回 false。理由与 start() 其他
    // 失败路径一致：这个能力此刻确实不可用。
    {
        // 先清累计量并**把 window_start 定在此刻**：本份统计从 STREAMON
        // 开始算。少了这一步，`window_start` 会沿用上一次 start 的旧值，
        // 于是「重启后第一份快照」的跨度会包含上一次停流的那整段时间。
        s.resetWindow();

        // 首份快照的跨度起点就是 window_start，即 STREAMON 时刻。
        s.last_snapshot_time = s.window_start;
        s.has_last_snapshot = false;
        s.thread_fatal = false;
        s.calibration_window_start = std::chrono::steady_clock::time_point{};
        try {
            s.startCaptureThread();
        } catch (const std::system_error& e) {
            s.error = std::string("采集线程创建失败: ") + e.what();
            s.closeDevice();
            s.resetWindow();
            return false;
        }
    }

    return true;
}

void V4l2FrameSource::stop() {
    Impl& s = *impl_;

    // 顺序不可交换：先让线程退出，再关 fd。
    //
    // 反过来的话，线程正卡在 `ioctl(fd, VIDIOC_DQBUF)` 或 `select(fd+1)`
    // 里，而 fd 已被关闭——轻则 EBADF，重则 fd 号被别的 open 复用，
    // 线程拿我们刚关掉的句柄去操作一个完全无关的文件（实测这种 bug
    // 的现场是「摄像头停了之后写坏了另一个设备的缓冲」）。
    s.stopCaptureThread();

    s.closeDevice();
    s.resetWindow();
}

FrameStats V4l2FrameSource::snapshotAndReset() {
    Impl& s = *impl_;
    FrameStats out;

    // 未启动 → 无效统计。上层必须报 unavailable，不能当成"零丢帧"。
    if (!s.streaming) return out;

    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lk(s.mtx);

    // 采集线程因致命错误退出 → 累计值已冻结，绝不能当成"本窗口统计"。
    //
    // 这是本轮改造最容易写错的一处：线程死后 `frames_received` 停在死亡
    // 时刻的数值，而 `frames_expected` 仍按真实流逝时间增长。若不拦截，
    // 调用方会读到「expected=1200, received=37」这种统计，并把它归因为
    // 「链路丢了 97% 的帧」——实际上是**我们自己采不到**。把观测缺陷
    // 栽给链路，是本案最坏的一种错误（运维会去查一条完好的线）。
    if (s.thread_fatal) return out;

    // ---- 本份统计的实际覆盖跨度 ----
    //
    // `window_ms` 报**事实**（这次统计真的覆盖了多久），不是配置值。
    //
    // 改动前报的是 cfg.window_ms（配置），而实际跨度靠"调用方阻塞满一个
    // 窗口"来保证。自驱线程之后这条保证消失了：调用方可能 15s 才取一次，
    // 也可能 200ms 取一次。若仍报配置值，`attributeVideoQoe` 的
    // `window_ms != network.interval_ms` 判定会在**正确对齐**的情况下
    // 反而失败——因为两侧报的都不是真话。
    //
    // 现在两侧语义统一为「这份数据覆盖了多久」，调用方只要让取数节奏与
    // 网络窗口一致，窗口自然对齐；对不齐时归因层会如实拒绝，而不是被
    // 一个假的一致值骗过去。
    const auto span = std::chrono::duration_cast<std::chrono::milliseconds>(
        s.has_last_snapshot ? (now - s.last_snapshot_time)
                            : (now - s.window_start));
    // 钳到 1ms：0 会被归因层当成「未声明窗口」（window_ms==0）而拒绝归因，
    // 而「两个快照挨得极近」是一个真实存在但无意义的窗口，不该被当成缺失。
    const uint32_t span_ms =
        span.count() > 0 ? static_cast<uint32_t>(span.count()) : 1u;
    const auto span_start = s.has_last_snapshot ? s.last_snapshot_time : s.window_start;

    // ---- 汇总本份统计 ----
    //
    // expected 用**实际跨度**推，与上面报出去的 window_ms 严格同源。
    // 两者若各算各的，丢帧率立刻失去意义（这也是改造前那三个假象的来源）。
    //
    // 整数运算 + 四舍五入，避免 IEEE-754 截断（1000.0/(1000.0/30)
    // = 29.999999999999996，截断成 29 会让丢帧率系统性偏移）。
    const uint64_t expected =
        (static_cast<uint64_t>(span_ms) * s.cfg.fps + 500u) / 1000u;

    out.frames_expected = expected;
    out.frames_received = s.frames_received;
    out.frames_corrupt = s.frames_corrupt;
    out.seq_gap_count = s.seq_gap_count;
    out.bytes_received = s.bytes_received;
    out.window_ms = span_ms;
    // 本实现是**自驱连续采集**：线程从不停止取帧，所以统计绝不会漏掉任何
    // 一段时间。于是「这份统计覆盖的跨度」与「调用方两次取数的间隔」由
    // **构造**决定恒等——这正是 `observationGapMs()==0` 真实可信的原因，
    // 而不是我们把盲区藏起来了。
    //
    // 两者都报而不省略一个，是为了让归因层能对**任何**实现用同一套判据：
    // 换成拉模式实现（只在快照时取帧）时，window_ms 会报配置值、这里报
    // 真实间隔，差值立刻暴露盲区。
    out.snapshot_interval_ms = span_ms;
    // 采样时刻用**实际**取数时刻。窗口起点是 span_start，终点即 now。
    out.sampled_at = now;

    if (!s.gaps.empty()) {
        out.max_gap_ms = *std::max_element(s.gaps.begin(), s.gaps.end());
        out.p95_gap_ms = percentile95(s.gaps);
    } else {
        // 没有任何帧 → 没有帧间隔可比。
        //
        // 此时若沿用上一窗口的 max_gap/p95，会凭空造出「画面有长卡顿」的
        // 假证据（而真相是一整段没有画面）。留 0 并让 frames_received==0
        // 自己说话：0 帧 + 0 间隔 = 「没有任何观测」，不是「间隔很健康」。
        out.max_gap_ms = 0;
        out.p95_gap_ms = 0;
    }

    if (!s.dequeue_latencies.empty()) {
        out.p95_dequeue_latency_ms = percentile95(s.dequeue_latencies);
    }

    // ---- 推进快照边界 ----
    s.last_snapshot_time = now;
    s.has_last_snapshot = true;

    // 校准的推进**不在这里**：它由采集线程按 cfg.window_ms 自行推进
    // （见 calibrateTickLocked）。若跟着快照相走，调用方改成 15s 取一次
    // 就会让「3 个校准窗口」变成 45s 的样本。校准必须在快照之前推进完毕，
    // 所以这里只做一次「若有待处理的校准窗口则结算」的兜底。
    s.calibrateTickLocked(now, span_start);

    s.resetWindow();
    return out;
}

std::string V4l2FrameSource::describe() const {
    const Impl& s = *impl_;

    // **必须加锁**：采集线程会写 `error`（DQBUF/QBUF 失败时），
    // 而 describe() 读它。不加锁是数据竞争——不是"风格问题"，
    // 是 std::string 的读写竞争（可能读到重分配一半的内部状态，
    // 表现为崩溃或乱码）。describe() 只在排障与日志路径调用，
    // 不是热路径，锁开销无关紧要。
    std::lock_guard<std::mutex> lk(s.mtx);

    std::ostringstream os;
    // 前缀 v4l2: 让 evidence 消费者能一眼区分真机与注入数据（与 synthetic: 对应）
    os << "v4l2:" << s.cfg.device << "@" << s.cfg.width << "x" << s.cfg.height
       << "/" << s.cfg.fps << "fps/" << s.cfg.window_ms << "ms";

    // 单平面 / 多平面必须写进描述：同一个 FrameStats 在两种 buf type 下
    // 采集方式完全不同，事后排查时需要知道当时走的是哪条路径。
    //
    // 用 `started_mplane` 而不是 `buf_type`：后者被 `closeDevice()` 复位成
    // 单平面，stop() 之后 describe() 会把跑过 mplane 的设备误报成 splane
    // （板端实测过，详见该字段注释）。这里问的是「刚才用了什么」，不是
    // 「现在正用什么」。
    os << (s.started_mplane ? " mplane" : " splane");

    // 缓冲来源必须写进描述，且**只在配置要求 DMABUF 时才写**。
    //
    // 理由：`use_dmabuf=true` 有三条可能的落点（导入 / 导出 / 回落 MMAP），
    // 而三者在 FrameStats 上完全看不出区别。若不写出来，运维无法确认
    // 「我配了 DMABUF 到底生效没有」——这正是配置项最典型的静默失效形态。
    // MMAP 是默认值，写出来只会给每一行日志加噪音，所以不写。
    //
    // 用 buffer_source 而不是 memory：后者被 closeDevice() 复位成 MMAP，
    // stop() 之后再看 describe() 会把成功过的 DMABUF 误报成「回落」。
    if (s.cfg.use_dmabuf) {
        switch (s.buffer_source) {
            case Impl::kMemDmabufHeap:   os << " dmabuf(heap)"; break;
            case Impl::kMemDmabufExport: os << " dmabuf(export)"; break;
            case Impl::kMemDmabufMmapFallback:
                os << " dmabuf->mmap-fallback";
                break;
            case Impl::kMemMmap:
            default:                     os << " mmap"; break;
        }
    }

    if (!s.streaming) os << " (stopped)";

    // S_INPUT 的异常失败单独用 `input:` 标出，与 `[...]` 的偏差记录区分：
    // 它不影响启动成败，但直接影响「S_FMT 为什么会 EINVAL」的归因。
    if (!s.input_note.empty()) os << " [input: " << s.input_note << "]";

    // 驱动协商结果（帧率/分辨率被校正、格式被强制）也带上：
    // 这些是「配置 vs 事实」的偏差记录，不写下来就无法解释统计为何与预期不符。
    if (!s.error.empty()) os << " [" << s.error << "]";

    // 帧率基线校准结果必须单独可见，不能用上面那条 `error` 通道。
    //
    // 理由是语义相反：`error` 记录的是「配置与驱动不一致」，而校准记录的是
    // 「驱动说的与硬件实际不一致」——后者恰恰是本板丢帧率虚高的根源。
    // 归因时若看不到这一条，就无法判断 `frames_expected` 用的到底是
    // 配置值、G_PARM 值，还是实测校准值，38% 的丢帧率会被重复归因。
    //
    // 未校准时也要写：`calibration_note` 说明了为什么跳过（样本不足/
    // 离散度过大），这比静默维持原值有用——否则运维会以为校准生效了。
    if (!s.calibration_note.empty()) os << " [fps: " << s.calibration_note << "]";

    // media 拓扑：只写「上游链路是否通」，不铺开整张图。
    //
    // 真机上这段能直接回答「为什么节点打开了却拿不到帧」——V4L2 层只会报
    // EINVAL，而拓扑里能看出是哪一跳 DISABLED。没解析到就整段不写，
    // 不要输出 "topology unavailable"：那句话会让读者以为出了问题，
    // 实际只是这个驱动/环境没有 media controller（x86 上的 UVC 就没有）。
    if (s.topology.valid) {
        const std::string up = summarizeUpstream(s.topology);
        if (!up.empty()) os << " {" << up << "}";
    }

    // 采集线程已死必须**显式说出**，不能只靠 `[...]` 里的 errno。
    //
    // 线程死后 `streaming` 仍是 true（fd 还开着、流还在推），`describe()`
    // 若只报 "mplane" 之类的静态信息，读的人会以为一切正常。真相是：
    // 帧统计从此冻结，`snapshotAndReset()` 返回 invalid。这两件事必须
    // 在同一行里连着读，才不会误判成「摄像头没问题，是上游链路丢了帧」。
    if (s.thread_fatal) os << " (capture-thread-dead)";

    return os.str();
}

/// 返回最近一次失败原因
///
/// **返回副本而不是引用**（虽然签名上是 const ref，实现上借用了内部的
/// `std::string`）：采集线程会改写 `error`，若返回内部引用，调用方拿到的
/// 是随时可能被重分配失效的句柄。`lastError()` 在排障路径上调用，
/// 一次拷贝可以接受；若将来变成热路径，应改为返回值语义或加锁取副本。
const std::string& V4l2FrameSource::lastError() const { return impl_->error; }

bool V4l2FrameSource::isRunning() const { return impl_->streaming; }

bool V4l2FrameSource::setPixelSink(PixelSink sink) {
    Impl& s = *impl_;
    // 运行期不允许换 sink：采集线程会无锁读取它（见 captureLoop），
    // 换掉就是数据竞争。拒绝而不是加锁，是因为「运行中换消费者」没有
    // 真实场景——消费者要在 start 之前就位。
    if (s.capture_running.load(std::memory_order_acquire)) return false;
    s.pixel_sink = std::move(sink);
    return true;
}

}  // namespace weaknet
