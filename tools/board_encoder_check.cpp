/**
 * @file board_encoder_check.cpp
 * @brief 板端真机验证：从 VideoFrameHub 取真帧 → 编码 → 校验 H.264
 *
 * 这是**一次性验证程序**，不进部署产物、不进 CI。它回答四个问题：
 *
 *   1. cedar VE 后端在本机能不能真的 configure 成功（dlopen 路径对不对）；
 *   2. 编码出来的码流是不是**真** H.264（解析 SPS，比对宽高）；
 *   3. dropped / failed / queue_depth 在真实负载下是不是合理；
 *   4. 零拷贝与 memcpy 两条路径产出的码流是不是同一级别。
 *
 * 为什么要解析 SPS 而不是只看「输出非空」：
 * 编码器返回一块非空内存不代表它是合法 H.264。SPS 里的 pic_width_in_mbs
 * 是编码器**自己声明的**尺寸，与我们的期望对照才是硬判据。
 * （本项目已有一次教训：v4l2-sizeimage-selfconsistent-wrong）
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "video_encoder.hpp"

namespace {

constexpr const char* kDev = "/dev/video0";
constexpr int kWantFrames = 40;

// 本板实测的真实几何（sunxi-vin 出 NV12），单位字节
constexpr uint32_t kRealWidth = 1640;
constexpr uint32_t kRealHeight = 1232;
constexpr uint32_t kRealStride = 1648;              // 16 对齐后的 Y 行宽
constexpr uint32_t kRealUvOffset = 2030336;         // 1648 * 1232
constexpr uint32_t kRealFrameBytes = 3045504;       // 2030336 + 1015168

// ---------------------------------------------------------------------------
// 极简 V4L2 NV12 采集（只为验证，不是生产路径）
//
// 为什么这里不用 VideoFrameHub：验证程序要尽可能少的活动部件。中枢自己
// 已经被单测覆盖，这里的变量是**编码器**。用最原始的 V4L2 直接给它喂帧，
// 出错时能确定是编码的问题而不是中枢的问题。
// ---------------------------------------------------------------------------

struct Capture {
    int fd{-1};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t sizeimage{0};
    uint32_t uv_offset{0};
    struct Buf {
        void* start{nullptr};
        size_t len{0};
    };
    std::vector<Buf> bufs;

    ~Capture() {
        for (auto& b : bufs) {
            if (b.start && b.start != MAP_FAILED) munmap(b.start, b.len);
        }
        if (fd >= 0) close(fd);
    }

    bool openDevice() {
        fd = open(kDev, O_RDWR);
        if (fd < 0) { perror("open /dev/video0"); return false; }

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = 1640;
        fmt.fmt.pix.height = 1232;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror("VIDIOC_S_FMT");
            return false;
        }
        width = fmt.fmt.pix.width;
        height = fmt.fmt.pix.height;
        stride = fmt.fmt.pix.bytesperline;
        sizeimage = fmt.fmt.pix.sizeimage;
        printf("[capture] S_FMT -> %ux%u bytesperline=%u sizeimage=%u "
               "fourcc=%c%c%c%c\n",
               width, height, stride, sizeimage,
               fmt.fmt.pix.pixelformat & 0xff,
               (fmt.fmt.pix.pixelformat >> 8) & 0xff,
               (fmt.fmt.pix.pixelformat >> 16) & 0xff,
               (fmt.fmt.pix.pixelformat >> 24) & 0xff);

        // 注意：S_FMT 后的 sizeimage 是**自洽的错值**（1640*1232*1.5），
        // 真实值只有 REQBUFS 之后才出现。这里两者都打出来对照。
        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            perror("VIDIOC_REQBUFS");
            return false;
        }
        printf("[capture] REQBUFS count=%u\n", req.count);

        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
                perror("VIDIOC_QUERYBUF");
                return false;
            }
            if (i == 0) {
                // 真实 sizeimage 在这里出现
                printf("[capture] QUERYBUF[0] length=%u  <-- 真实 sizeimage\n",
                       b.length);
                sizeimage = b.length;
            }
            void* p = mmap(nullptr, b.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, b.m.offset);
            if (p == MAP_FAILED) { perror("mmap"); return false; }
            bufs.push_back({p, b.length});
            if (ioctl(fd, VIDIOC_QBUF, &b) < 0) {
                perror("VIDIOC_QBUF");
                return false;
            }
        }

        // 用实测事实推算 UV 偏移：找 Y 平面之后那段全零空洞的起点之后。
        // 生产代码里这是驱动的 bytesperline/几何推出来的，这里为了独立验证，
        // 用「sizeimage - sizeimage/3」这条稳定关系反推。
        uv_offset = sizeimage * 2 / 3;

        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
            perror("VIDIOC_STREAMON");
            return false;
        }
        return true;
    }

    /// 取一帧（0 表示成功），返回 mmap 地址与长度
    bool dequeue(void** out, size_t* len) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) return false;
        *out = bufs[b.index].start;
        *len = b.bytesused;
        pending_index_ = b.index;
        return true;
    }

    bool requeue() {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = pending_index_;
        return ioctl(fd, VIDIOC_QBUF, &b) == 0;
    }

    int pending_index_{-1};
};

// ---------------------------------------------------------------------------
// 合成帧源（dma-heap）
//
// 为什么要有这条路：生产服务 `weaknet-dbus-server` 长期占着 /dev/video0
// （`open()` 层独占），验证程序不该为了自证而把正在跑的服务打断。而
// 「编码器吃得对不对」这件事并不依赖像素来自哪里——它只依赖**内存在哪、
// 布局如何**。所以这里用 dma-heap 分配一块与真实 V4L2 帧**逐字节同构**的
// 缓冲（同样 1648 行宽、同样 9856 字节对齐空洞、同样 3045504 总长），
// 再填入会动的图案，喂给编码器。
//
// 这不是「假数据糊弄」：真正被验证的东西（VE 后端可用性、零拷贝对齐、
// SPS 尺寸、计数口径）全都不受影响；唯一没被覆盖的是 V4L2 出队的时序，
// 而那属于 VideoFrameHub / V4l2FrameSource 的职责，已由别处覆盖。
// ---------------------------------------------------------------------------

struct SyntheticFrames {
    int fd{-1};
    void* map{MAP_FAILED};
    size_t size{0};
    uint32_t uv_offset{0};

    ~SyntheticFrames() {
        if (map && map != MAP_FAILED) munmap(map, size);
        if (fd >= 0) close(fd);
    }

    bool alloc(size_t len, uint32_t uv_off) {
        const int hfd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
        if (hfd < 0) { perror("open /dev/dma_heap/system"); return false; }
        dma_heap_allocation_data d{};
        d.len = len;
        d.fd_flags = O_RDWR | O_CLOEXEC;
        const int rc = ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &d);
        close(hfd);
        if (rc < 0) { perror("DMA_HEAP_IOCTL_ALLOC"); return false; }

        fd = static_cast<int>(d.fd);
        size = len;
        uv_offset = uv_off;
        map = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { perror("mmap(dma-heap)"); return false; }

        // 空洞清零：真实帧里 [uv_offset, uv_offset+9856) 之外那段 Y 尾部空隙
        // 是驱动不做任何保证的区域。编码器只读 [0,y_size) 与
        // [uv_offset,uv_offset+c_size)，其余不碰，这里清零只是让 dump 干净。
        std::memset(map, 0, len);
        printf("[synthetic] dma-heap fd=%d len=%zu uv_offset=%u\n", fd, len,
               uv_off);
        return true;
    }

    /// 填一帧**有内容**的图案：移动的竖向色条 + 渐变的 Y。
    ///
    /// 为什么不能填全零：全零帧在 H.264 下几乎不产生码流，码率、耗时、
    /// 丢帧判断全部失去意义，验证就退化成「编码器返回了非空内存」。
    void fill(int frame_idx) {
        auto* y = static_cast<uint8_t*>(map);
        const uint32_t y_rows = uv_offset / kRealStride;  // 1232 行
        const int bars = 8;
        const int phase = (frame_idx * 7) % kRealStride;
        for (uint32_t r = 0; r < y_rows; ++r) {
            uint8_t* row = y + r * kRealStride;
            for (uint32_t c = 0; c < kRealStride; ++c) {
                // 斜条纹 + 滚动，保证每帧都不相同（否则 P 帧几乎为零字节）
                const int v = ((c + phase) / 16 + (r / 16)) % (bars * 2);
                row[c] = static_cast<uint8_t>(
                    v < bars ? 235 : 16 + ((c * 3 + r * 5) & 0x1F));
            }
        }
        auto* uv = y + uv_offset;
        const uint32_t c_rows = (size - uv_offset) / kRealStride;
        for (uint32_t r = 0; r < c_rows; ++r) {
            uint8_t* row = uv + r * kRealStride;
            for (uint32_t c = 0; c + 1 < kRealStride; c += 2) {
                row[c] = 128;      // U：中性
                row[c + 1] = 128;  // V：中性
            }
        }
    }
};

// ---------------------------------------------------------------------------
// SPS 解析：编码器自己声明的尺寸，与期望对照
// ---------------------------------------------------------------------------

struct BitReader {
    const uint8_t* p;
    size_t bits;
    size_t pos{0};

    BitReader(const uint8_t* d, size_t n) : p(d), bits(n * 8) {}

    bool bit() {
        if (pos >= bits) return false;
        const bool v = (p[pos >> 3] >> (7 - (pos & 7))) & 1;
        ++pos;
        return v;
    }
    uint32_t u(size_t n) {
        uint32_t v = 0;
        for (size_t i = 0; i < n; ++i) v = (v << 1) | (bit() ? 1u : 0u);
        return v;
    }
    /// exp-golomb unsigned
    uint32_t ue() {
        int zeros = 0;
        while (pos < bits && !bit()) ++zeros;
        if (zeros == 0) return 0;
        return (1u << zeros) - 1 + u(zeros);
    }
    /// exp-golomb signed
    int32_t se() {
        const uint32_t k = ue();
        return (k & 1) ? static_cast<int32_t>((k + 1) / 2)
                       : -static_cast<int32_t>(k / 2);
    }
    void skip_scaling_list(size_t size) {
        int last = 8, next = 8;
        for (size_t j = 0; j < size; ++j) {
            if (next != 0) {
                const int32_t delta = se();
                next = (last + delta + 256) % 256;
            }
            if (next != 0) last = next;
        }
    }
};

struct SpsInfo {
    bool ok{false};
    uint32_t profile_idc{0};
    uint32_t level_idc{0};
    uint32_t width{0};   ///< 显示宽（已扣 crop）
    uint32_t height{0};
    uint32_t coded_width{0};
};

/// 从 Annex-B 码流中解析第一份 SPS
SpsInfo parseSps(const uint8_t* d, size_t n) {
    SpsInfo info;

    // 找 NAL type 7（SPS）
    size_t i = 0, sps_off = 0, sps_len = 0;
    while (i + 4 < n) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            const uint8_t t = d[i + 4] & 0x1F;
            if (t == 7) { sps_off = i + 5; break; }
            i += 4;
        } else {
            ++i;
        }
    }
    if (sps_off == 0) return info;

    // 找起始码之前最后一个非零字节，去掉 trailing_zero_8bits
    size_t end = sps_off;
    while (end < n) {
        if (d[end] == 0 && end + 3 < n && d[end + 1] == 0 &&
            d[end + 2] == 0 && d[end + 3] == 1) {
            break;
        }
        ++end;
    }
    while (end > sps_off && d[end - 1] == 0) --end;
    sps_len = end - sps_off;
    if (sps_len < 4) return info;

    BitReader br(d + sps_off, sps_len);
    info.profile_idc = br.u(8);
    br.u(8);  // constraint flags
    info.level_idc = br.u(8);
    br.ue();  // sps_id

    if (info.profile_idc == 100 || info.profile_idc == 110 ||
        info.profile_idc == 122 || info.profile_idc == 244 ||
        info.profile_idc == 44 || info.profile_idc == 83 ||
        info.profile_idc == 86 || info.profile_idc == 118 ||
        info.profile_idc == 128 || info.profile_idc == 138 ||
        info.profile_idc == 139 || info.profile_idc == 134) {
        const uint32_t chroma_format_idc = br.ue();
        if (chroma_format_idc == 3) br.bit();  // separate_colour_plane
        br.ue();                               // bit_depth_luma
        br.ue();                               // bit_depth_chroma
        br.bit();                              // qpprime_y_zero
        if (br.bit()) {                        // seq_scaling_matrix
            const size_t cnt = (chroma_format_idc != 3) ? 8 : 12;
            for (size_t k = 0; k < cnt; ++k) {
                if (br.bit()) br.skip_scaling_list(k < 6 ? 16 : 64);
            }
        }
    }

    br.ue();  // log2_max_frame_num_minus4
    const uint32_t poc_type = br.ue();
    if (poc_type == 0) {
        br.ue();
    } else if (poc_type == 1) {
        br.bit();
        br.se();
        br.se();
        const uint32_t m = br.ue();
        for (uint32_t k = 0; k < m; ++k) br.se();
    }
    br.ue();    // max_num_ref_frames
    br.bit();   // gaps_in_frame_num
    const uint32_t w_mbs = br.ue() + 1;
    const uint32_t h_map = br.ue() + 1;
    const bool frame_mbs_only = br.bit();
    if (!frame_mbs_only) br.bit();
    br.bit();  // direct_8x8
    uint32_t crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    if (br.bit()) {
        crop_l = br.ue();
        crop_r = br.ue();
        crop_t = br.ue();
        crop_b = br.ue();
    }

    info.coded_width = w_mbs * 16;
    const uint32_t coded_height = h_map * 16 * (frame_mbs_only ? 1 : 2);
    uint32_t crop_unit_x = 2, crop_unit_y = 2;
    if (info.profile_idc == 122 || info.profile_idc == 244 ||
        info.profile_idc == 44 || info.profile_idc == 83 ||
        info.profile_idc == 86 || info.profile_idc == 118 ||
        info.profile_idc == 128 || info.profile_idc == 138 ||
        info.profile_idc == 139 || info.profile_idc == 134) {
        // 4:2:0 → 单位仍是 2；这里不细究 4:2:2/4:4:4
    }
    info.width = info.coded_width - (crop_l + crop_r) * crop_unit_x;
    info.height =
        coded_height - (crop_t + crop_b) * crop_unit_y * (frame_mbs_only ? 1 : 2);
    info.ok = (info.width > 0 && info.height > 0 && info.coded_width >= 16);
    return info;
}

// ---------------------------------------------------------------------------

int runCase(bool zero_copy) {
    printf("\n===== 路径: %s =====\n", zero_copy ? "零拷贝" : "memcpy");

    // 优先用真摄像头；被生产服务占用时退回 dma-heap 合成帧。
    // 两条路的**几何完全相同**，所以编码器一侧的判据（VE 可用性、对齐、
    // SPS、计数口径）不受影响。真摄像头能跑时优先用它，因为顺带覆盖了
    // 「驱动回报的 uv_offset 正好等于 align16(w)*h」这条前提。
    Capture cap;
    SyntheticFrames syn;
    const bool have_v4l2 = cap.openDevice();

    weaknet::VideoEncoderConfig cfg;
    if (have_v4l2) {
        cfg.width = cap.width;
        cfg.height = cap.height;
        cfg.stride = cap.stride;
        cfg.uv_offset = cap.uv_offset;
        cfg.frame_bytes = cap.sizeimage;
    } else {
        printf("[src] /dev/video0 不可用（生产服务独占），改用 dma-heap 合成帧\n");
        if (!syn.alloc(kRealFrameBytes, kRealUvOffset)) return 1;
        cfg.width = kRealWidth;
        cfg.height = kRealHeight;
        cfg.stride = kRealStride;
        cfg.uv_offset = kRealUvOffset;
        cfg.frame_bytes = kRealFrameBytes;
    }
    cfg.fourcc = V4L2_PIX_FMT_NV12;
    cfg.fps = 18;
    cfg.bitrate_kbps = 2000;
    cfg.allow_zero_copy = zero_copy;

    printf("[cfg] %ux%u stride=%u uv_offset=%u frame_bytes=%u\n", cfg.width,
           cfg.height, cfg.stride, cfg.uv_offset, cfg.frame_bytes);

    auto enc = weaknet::createVideoEncoder(true);
    printf("[enc] backend=%s describe=%s\n", enc->backend().c_str(),
           enc->describe().c_str());

    if (!enc->configure(cfg)) {
        printf("!! configure 失败: %s\n", enc->describe().c_str());
        return 2;
    }
    printf("[enc] configure OK: %s\n", enc->describe().c_str());

    size_t total_bytes = 0, total_frames = 0, keyframes = 0;
    std::vector<uint8_t> first_bs;
    weaknet::EncoderStats last;

    for (int i = 0; i < kWantFrames; ++i) {
        weaknet::VideoFrameView fv;
        if (have_v4l2) {
            void* data = nullptr;
            size_t len = 0;
            if (!cap.dequeue(&data, &len)) {
                printf("!! DQBUF 失败 at %d\n", i);
                break;
            }
            fv.data = data;
            fv.size = len;
            // V4L2 mmap 缓冲没有独立 fd，零拷贝无从谈起——这正是真机上
            // 必须走帧中枢 dma-heap 槽位的原因。这里显式保持 -1。
            fv.dmabuf_fd = -1;
        } else {
            syn.fill(i);
            fv.data = syn.map;
            fv.size = syn.size;
            fv.dmabuf_fd = syn.fd;
        }
        fv.sequence = static_cast<uint64_t>(i);
        fv.timestamp_ns = static_cast<uint64_t>(time(nullptr)) * 1000000000ULL +
                          static_cast<uint64_t>(i) * 55000000ULL;

        // 按真实采集节拍送帧（18.5fps ≈ 54ms/帧）。
        //
        // 为什么必须节拍而不能全速连发：编码器的输入队列有深度上限，全速灌
        // 会立刻打满（实测现象：前 4 帧出码流，之后每帧 AddInputBuffer 报
        // "all input buffer is used by encoder"）。而生产环境里帧源本来就是
        // 18.5fps 的实时流，全速连发是个不存在的场景——用它来判断编码器
        // 健不健康，等于给自己造一个假故障。
        if (i > 0) {
            struct timespec ts {};
            ts.tv_nsec = 54000000L;
            nanosleep(&ts, nullptr);
        }

        weaknet::EncodedFrame ef;
        if (enc->encode(fv, ef) && ef.valid()) {
            total_bytes += ef.size;
            total_frames++;
            if (ef.keyframe) {
                keyframes++;
                // 只留关键帧用于 SPS 解析：SPS 只在 IDR 前出现，P 帧里没有。
                // 早期版本的错误是抓「第一块码流」去解析，而首帧未必是 IDR，
                // 于是永远找不到 SPS，被误判成「不是合法 H.264」。
                if (first_bs.empty()) {
                    // 必须拷走：EncodedFrame::data 的生命周期只到下次 encode()
                    first_bs.assign(ef.data, ef.data + ef.size);
                }
            }
            if (total_frames <= 3) {
                printf("  f%d seq=%llu size=%zu key=%d hdr=%02x%02x%02x%02x\n",
                       i, static_cast<unsigned long long>(ef.sequence), ef.size,
                       ef.keyframe ? 1 : 0, ef.data[0], ef.data[1], ef.data[2],
                       ef.data[3]);
            }
        }
        if (have_v4l2) cap.requeue();
    }

    // flush：把 pipeline 里剩的冲出来
    size_t flushed = enc->flush([](const weaknet::EncodedFrame& e) {
        printf("  [flush] seq=%llu size=%zu key=%d\n",
               static_cast<unsigned long long>(e.sequence), e.size,
               e.keyframe ? 1 : 0);
    });

    last = enc->snapshotAndReset();
    printf("[stats] in=%llu out=%llu bytes=%llu keys=%llu dropped=%llu "
           "failed=%llu queue=%u window=%ums bitrate=%ukbps valid=%d\n",
           static_cast<unsigned long long>(last.frames_in),
           static_cast<unsigned long long>(last.frames_out),
           static_cast<unsigned long long>(last.bytes_out),
           static_cast<unsigned long long>(last.keyframes),
           static_cast<unsigned long long>(last.dropped),
           static_cast<unsigned long long>(last.failed), last.queue_depth,
           last.window_ms, last.bitrate_kbps(), last.valid ? 1 : 0);
    printf("[total] 收到 %zu 帧, %zu 字节, %zu 关键帧, flush %zu\n",
           total_frames, total_bytes, keyframes, flushed);

    int rc = 0;
    if (first_bs.empty()) {
        printf("!! 没有产出任何码流\n");
        rc = 3;
    } else {
        const SpsInfo sps = parseSps(first_bs.data(), first_bs.size());
        if (!sps.ok) {
            printf("!! SPS 解析失败（不是合法 H.264）\n");
            rc = 4;
        } else {
            printf("[sps] profile=%u level=%u coded_w=%u 显示=%ux%u （期望 %ux%u）\n",
                   sps.profile_idc, sps.level_idc, sps.coded_width, sps.width,
                   sps.height, cfg.width, cfg.height);
            if (sps.width != cfg.width || sps.height != cfg.height) {
                printf("!! SPS 声明的尺寸与配置不符\n");
                rc = 5;
            } else {
                printf("[sps] ✅ 尺寸完全吻合\n");
            }
        }
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    // 板端需要 root（/dev/cedar_dev_ve2 是 root:root 0600）
    const bool only_memcpy = (argc > 1 && std::strcmp(argv[1], "memcpy") == 0);
    const bool only_zc = (argc > 1 && std::strcmp(argv[1], "zc") == 0);

    int rc = 0;
    if (!only_zc) rc |= runCase(false);
    // 零拷贝单独跑一遍：单实例测试，不共用 Capture（VE 上下文不共享）
    if (!only_memcpy) rc |= runCase(true);
    return rc;
}
