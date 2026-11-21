/**
 * @file board_snapshot_check.cpp
 * @brief 板端真机验证：ISnapshotEncoder 的 cedar JPEG 后端能否产出真 JPEG
 *
 * ============================================================================
 * 它回答的问题
 * ============================================================================
 *
 *   1. `createSnapshotEncoder(true)` 在本板是否真的落到 cedar 后端
 *      （而不是回落桩）；
 *   2. `configure()` 用本板真实几何（1640x1232 / stride 1640 / uv 2030336）
 *      能否成功；
 *   3. `capture()` 产出的字节是不是**结构完整**的 JPEG（逐段解析，不只看
 *      magic）；
 *   4. 连续多张是否稳定（描述符池不会在第 5 张卡死——这是 H.264 路径的
 *      著名坑，JPEG 路径必须独立验证一遍）；
 *   5. 单张耗时与字节数，用于给质量参数定档。
 *
 * ============================================================================
 * 为什么用合成帧而**不**去 /dev/video0 取真帧
 * ============================================================================
 *
 * board_encoder_check.cpp 会独占打开视频设备，那要求先停生产服务。而
 * 验证「硬件 JPEG 能不能用」根本不需要真实场景——编码器只关心内存里的
 * NV12 排列。喂合成帧能把变量降到最低：出问题时一定是编码器，不会是
 * V4L2 或驱动。
 *
 * 而且这块板子的采集本身有独立的稳定性问题（开机竞态 / TDM 锁死），
 * 把两者耦合在一起查会让归因变难。
 *
 * 合成帧内容是**梯度 + 棋盘**，不是全零：全零 NV12 编出来体积异常小，
 * 用它测质量参数会得出完全错误的结论。
 *
 * ============================================================================
 * 用法
 * ============================================================================
 *
 *   ./board_snapshot_check [--frames 8] [--quality 85] [--dump /tmp/snap.jpg]
 *
 * 不需要 root，不需要停服务（不碰视频设备）。
 *
 * 退出码：0 = 全部通过；1 = 有断言失败。
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <getopt.h>

#include "snapshot_encoder.hpp"

namespace {

/// 本板实测几何（sunxi-vin 出 NV12 的事实值）
constexpr uint32_t kWidth = 1640;
constexpr uint32_t kHeight = 1232;
constexpr uint32_t kStride = 1640;
constexpr uint32_t kAlignW = 1648;                    // 16 对齐后的编码宽
constexpr uint32_t kUvOffset = kAlignW * kHeight;     // 2030336
constexpr uint32_t kFrameBytes = kUvOffset + kUvOffset / 2;  // 3045504

/**
 * @brief 造一帧有真实纹理的 NV12
 *
 * 为什么不是全零：JPEG 对平坦区域压缩率极高，全零图会小到几十字节，
 * 用它比较 quality 档位会得出「质量几乎不影响体积」的错误结论。
 * 梯度 + 棋盘能在 Y 与 UV 两个平面都制造高频，接近真实画面的熵。
 */
std::vector<uint8_t> makeFrame(uint32_t seed) {
    std::vector<uint8_t> f(kFrameBytes, 0);

    // Y 平面：沿 x 的梯度 + 沿 y 的棋盘
    for (uint32_t y = 0; y < kHeight; ++y) {
        uint8_t* row = f.data() + static_cast<size_t>(y) * kStride;
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint32_t checker = ((x / 8) ^ (y / 8)) & 1;
            row[x] = static_cast<uint8_t>(
                (x * 255u / kWidth + y * 3u + checker * 60u + seed) & 0xFF);
        }
    }

    // UV 平面：交错 U/V，用较慢的变化（色度本就低频）
    uint8_t* uv = f.data() + kUvOffset;
    const size_t uv_len = kUvOffset / 2;
    for (size_t i = 0; i + 1 < uv_len; i += 2) {
        const uint32_t p = static_cast<uint32_t>(i / 2);
        uv[i]     = static_cast<uint8_t>((p / 4 + 128 + seed) & 0xFF);
        uv[i + 1] = static_cast<uint8_t>((p / 7 + 64) & 0xFF);
    }
    return f;
}

/// JPEG 段结构解析结果
struct JpegScan {
    bool soi{false};
    bool eoi{false};
    bool app0_jfif{false};
    int dqt{0};
    bool sof0{false};
    int dht{0};
    bool sos{false};
    uint32_t sof_width{0};
    uint32_t sof_height{0};
    uint8_t sof_precision{0};
    uint8_t sof_components{0};
    size_t segment_errors{0};
    std::string first_error;
};

/**
 * @brief 逐段走一遍 JPEG 结构
 *
 * 为什么不信 `looksLikeJpeg()` 就够了：它只看前 3 字节。板厂库在错误配置下
 * 完全可以给出 `FF D8 FF xx` 开头、后面全是垃圾的内存。只有把段链走完、
 * 读到 EOI，才能说「这是一张完整的图」。SOF0 里的宽高还能反查编码器是否
 * 真的按我们给的几何编的。
 */
JpegScan scanJpeg(const std::vector<uint8_t>& b) {
    JpegScan s;
    if (b.size() < 4) {
        s.first_error = "too short";
        return s;
    }
    if (b[0] != 0xFF || b[1] != 0xD8) {
        s.first_error = "no SOI";
        return s;
    }
    s.soi = true;

    size_t i = 2;
    while (i + 1 < b.size()) {
        if (b[i] != 0xFF) {
            ++s.segment_errors;
            if (s.first_error.empty()) {
                s.first_error = "expected marker at offset " + std::to_string(i);
            }
            break;
        }
        // 跳过填充的 0xFF
        while (i < b.size() && b[i] == 0xFF) ++i;
        if (i >= b.size()) break;
        const uint8_t marker = b[i++];

        if (marker == 0xD9) {  // EOI
            s.eoi = true;
            break;
        }
        // 无载荷的 marker
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            continue;
        }
        if (i + 1 >= b.size()) {
            ++s.segment_errors;
            if (s.first_error.empty()) s.first_error = "truncated length";
            break;
        }
        const size_t seg_len = (static_cast<size_t>(b[i]) << 8) | b[i + 1];
        if (seg_len < 2 || i + seg_len > b.size()) {
            ++s.segment_errors;
            if (s.first_error.empty()) {
                s.first_error = "bad segment length at offset " + std::to_string(i);
            }
            break;
        }
        const uint8_t* p = b.data() + i + 2;
        const size_t plen = seg_len - 2;

        switch (marker) {
            case 0xE0:  // APP0
                if (plen >= 5 && std::memcmp(p, "JFIF\0", 5) == 0) s.app0_jfif = true;
                break;
            case 0xDB: ++s.dqt; break;
            case 0xC4: ++s.dht; break;
            case 0xC0:  // SOF0 baseline
                s.sof0 = true;
                if (plen >= 6) {
                    s.sof_precision = p[0];
                    s.sof_height = (static_cast<uint32_t>(p[1]) << 8) | p[2];
                    s.sof_width = (static_cast<uint32_t>(p[3]) << 8) | p[4];
                    s.sof_components = p[5];
                }
                break;
            case 0xDA:  // SOS：熵编码数据跟在后面，段链到此为止
                s.sos = true;
                i += seg_len;
                goto entropy_done;
            default:
                break;
        }
        i += seg_len;
    }
entropy_done:

    // SOS 之后是熵编码数据，长度不固定，只能从尾部找 EOI。
    if (s.sos && !s.eoi) {
        if (b.size() >= 2 && b[b.size() - 2] == 0xFF && b[b.size() - 1] == 0xD9) {
            s.eoi = true;
        }
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t frames = 8;
    // 默认 0 = **不设置**，让编码器用它自己的默认质量（70）。
    //
    // 这里刻意不写 85：探针要测的是**生产默认行为**，而不是探针作者
    // 随手填的一个数。之前写死 85，结果探针一直测的是一档生产上不会
    // 使用的质量，还顺手掩盖了「85 会超云端单分片上限」这件事。
    int quality = 0;
    const char* dump_path = nullptr;

    static const option kLongOpts[] = {
        {"frames", required_argument, nullptr, 'n'},
        {"quality", required_argument, nullptr, 'q'},
        {"dump", required_argument, nullptr, 'd'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int ch = 0;
    while ((ch = getopt_long(argc, argv, "n:q:d:h", kLongOpts, nullptr)) != -1) {
        switch (ch) {
            case 'n': frames = static_cast<uint32_t>(std::atoi(optarg)); break;
            case 'q': quality = std::atoi(optarg); break;
            case 'd': dump_path = optarg; break;
            case 'h':
                std::printf("用法: %s [--frames N] [--quality 1-100] [--dump FILE]\n",
                            argv[0]);
                return 0;
            default:
                return 1;
        }
    }

    std::printf("=== 快照编码器板端验证 ===\n");

    weaknet::SnapshotEncoderConfig cfg;
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.stride = kStride;
    cfg.uv_offset = kUvOffset;
    cfg.frame_bytes = kFrameBytes;
    cfg.quality = quality;

    auto enc = weaknet::createSnapshotEncoder(true);
    if (!enc) {
        std::printf("FAIL: 工厂返回空（接口承诺非空，这是 bug）\n");
        return 1;
    }
    std::printf("[factory] backend=%s\n", enc->backend().c_str());
    std::printf("[factory] %s\n", enc->describe().c_str());

    bool hardware = (enc->backend() != "fake");
    std::printf("[factory] %s\n",
                hardware ? "-> 硬件后端（继续验证硬件路径）"
                         : "-> **回落桩后端**，本机硬件不可用");

    if (!enc->configure(cfg)) {
        std::printf("FAIL: configure 失败: %s\n", enc->describe().c_str());
        return 1;
    }
    std::printf("[config ] OK  %s\n", enc->describe().c_str());

    int failures = 0;
    size_t first_size = 0;
    size_t min_size = 0, max_size = 0;
    long long total_ms = 0;

    for (uint32_t i = 0; i < frames; ++i) {
        auto frame_bytes_vec = makeFrame(i);
        weaknet::SnapshotFrameView v;
        v.data = frame_bytes_vec.data();
        v.size = frame_bytes_vec.size();
        v.sequence = 1000 + i;
        v.timestamp_ns = 1700000000000000000ull + i * 33333333ull;

        weaknet::SnapshotImage img;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = enc->capture(v, img);
        const auto t1 = std::chrono::steady_clock::now();
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        total_ms += ms;

        if (!ok) {
            std::printf("[frame %u] FAIL: %s\n", i, enc->describe().c_str());
            ++failures;
            continue;
        }
        if (i == 0) first_size = img.bytes.size();
        if (min_size == 0 || img.bytes.size() < min_size) min_size = img.bytes.size();
        if (img.bytes.size() > max_size) max_size = img.bytes.size();

        const JpegScan s = scanJpeg(img.bytes);
        std::printf("[frame %u] %zu bytes  %lld ms  seq=%llu  %ux%u  "
                    "SOI=%d EOI=%d APP0=%d DQT=%d SOF0=%d %ux%u p%d c%d "
                    "DHT=%d SOS=%d\n",
                    i, img.bytes.size(), ms,
                    static_cast<unsigned long long>(img.sequence),
                    img.width, img.height,
                    s.soi ? 1 : 0, s.eoi ? 1 : 0, s.app0_jfif ? 1 : 0,
                    s.dqt, s.sof0 ? 1 : 0, s.sof_width, s.sof_height,
                    s.sof_precision, s.sof_components, s.dht, s.sos ? 1 : 0);

        // ---- 断言 ----
        if (!s.soi) { std::printf("  !! 缺 SOI\n"); ++failures; }
        if (!s.eoi) { std::printf("  !! 缺 EOI（图不完整，落盘会是坏文件）\n"); ++failures; }
        if (!s.sof0) { std::printf("  !! 缺 SOF0\n"); ++failures; }
        if (!s.sos) { std::printf("  !! 缺 SOS\n"); ++failures; }
        if (s.dqt < 2) { std::printf("  !! DQT 段只有 %d 个（应 ≥2）\n", s.dqt); ++failures; }
        if (s.sof0) {
            if (s.sof_width != kWidth || s.sof_height != kHeight) {
                std::printf("  !! SOF0 声明尺寸 %ux%u 与请求 %ux%u 不符\n",
                            s.sof_width, s.sof_height, kWidth, kHeight);
                ++failures;
            }
            if (s.sof_components != 3) {
                std::printf("  !! SOF0 分量数 %u（应 3）\n", s.sof_components);
                ++failures;
            }
            if (s.sof_precision != 8) {
                std::printf("  !! SOF0 精度 %u（应 8）\n", s.sof_precision);
                ++failures;
            }
        }
        if (img.sequence != 1000 + i) {
            std::printf("  !! sequence 未透传\n");
            ++failures;
        }
        if (img.bytes.size() < 1000) {
            std::printf("  !! 体积 %zu 过小，疑似空白图\n", img.bytes.size());
            ++failures;
        }
        if (!s.first_error.empty()) {
            std::printf("  !! 段结构错误: %s（共 %zu 处）\n",
                        s.first_error.c_str(), s.segment_errors);
            ++failures;
        }

        if (dump_path && i == 0) {
            FILE* fp = std::fopen(dump_path, "wb");
            if (fp) {
                std::fwrite(img.bytes.data(), 1, img.bytes.size(), fp);
                std::fclose(fp);
                std::printf("  -> 已写出 %s\n", dump_path);
            } else {
                std::printf("  !! 无法写 %s\n", dump_path);
            }
        }
    }

    const auto st = enc->stats();
    std::printf("\n=== 汇总 ===\n");
    std::printf("frames=%u captured=%llu failed=%llu\n", frames,
                static_cast<unsigned long long>(st.captured),
                static_cast<unsigned long long>(st.failed));
    if (frames > 0) {
        std::printf("size first=%zu min=%zu max=%zu avg_ms=%.1f\n",
                    first_size, min_size, max_size,
                    static_cast<double>(total_ms) / static_cast<double>(frames));
    }
    std::printf("backend=%s hardware=%s\n", enc->backend().c_str(),
                hardware ? "yes" : "no");
    std::printf("断言失败数: %d\n", failures);

    if (failures == 0) {
        std::printf("\nRESULT: PASS —— 连续 %u 张全部是结构完整的 JPEG\n", frames);
        return 0;
    }
    std::printf("\nRESULT: FAIL —— 见上面 !! 行\n");
    return 1;
}
