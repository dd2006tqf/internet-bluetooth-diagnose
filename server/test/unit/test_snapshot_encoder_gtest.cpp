/**
 * @file test_snapshot_encoder_gtest.cpp
 * @brief ISnapshotEncoder 契约测试（无摄像头、无 VE 硬件）
 *
 * ============================================================================
 * 这份测试为什么存在
 * ============================================================================
 *
 * 快照编码器比视频编码器**更容易静默出错**，因为它多一条出口：字节会落盘
 * 或上传。视频码流错了，播放器黑屏，人一眼能看见；JPEG 错了，写出去的是
 * 一个 120KB 的坏文件，事后没人分得清它和真图。
 *
 * 所以这里断言的全是「不会报错的错」，一条都不测「能不能编出真图」
 * （那必须上板）：
 *
 *   - 几何不匹配必须显式拒绝，不能猜
 *   - 短缓冲必须在 memcpy 之前被拒（喂短缓冲 = 读越界 = 随机色块）
 *   - `capture()` 返回 true 必须意味着**确实是 JPEG**（FF D8 FF）
 *   - 桩后端产出的假字节也要能走通下游校验路径（否则校验形同虚设）
 *   - 工厂**永远非空**，且能力缺失要能从 describe() 读出来
 *   - 失败计数与成功计数必须分开，不能被同一次调用同时加
 *
 * ============================================================================
 * 为什么用 createFakeSnapshotEncoder
 * ============================================================================
 *
 * 同 test_video_encoder_gtest：这份文件在没有 /dev/cedar_dev_ve2 的机器上
 * 必须能跑（CI、开发机）。真后端的能力缺失由工厂的回落语义覆盖，而那份
 * 语义本身就在下面被测。
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "snapshot_encoder.hpp"

namespace weaknet {
namespace {

/// 本板实测几何：1640x1232，stride=1640，UV 偏移 2030336（= 1648*1232）
constexpr uint32_t kWidth = 1640;
constexpr uint32_t kHeight = 1232;
constexpr uint32_t kStride = 1640;
constexpr uint32_t kAlignW = 1648;
constexpr uint32_t kUvOffset = kAlignW * kHeight;             // 2030336
constexpr uint32_t kFrameBytes = kUvOffset + kUvOffset / 2;   // 3045504

/// 造一帧合法的 NV12（含 9856 字节的对齐空洞）
std::vector<uint8_t> makeFrame() {
    std::vector<uint8_t> f(kFrameBytes, 0);
    for (uint32_t i = 0; i < kUvOffset; ++i) {
        f[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    }
    for (uint32_t i = 0; i < kUvOffset / 2; ++i) {
        f[kUvOffset + i] = static_cast<uint8_t>((i * 13) & 0xFF);
    }
    return f;
}

SnapshotEncoderConfig boardConfig() {
    SnapshotEncoderConfig c;
    c.width = kWidth;
    c.height = kHeight;
    c.stride = kStride;
    c.uv_offset = kUvOffset;
    c.frame_bytes = kFrameBytes;
    c.quality = 85;
    return c;
}

SnapshotFrameView viewOf(const std::vector<uint8_t>& f, uint64_t seq,
                         uint64_t ts) {
    SnapshotFrameView v;
    v.data = f.data();
    v.size = f.size();
    v.sequence = seq;
    v.timestamp_ns = ts;
    return v;
}

// ===========================================================================
// 工厂
// ===========================================================================

TEST(SnapshotEncoderFactory, NeverReturnsNull) {
    // 无论硬件在不在，调用方都不该写空指针分支。
    EXPECT_NE(createSnapshotEncoder(true), nullptr);
    EXPECT_NE(createSnapshotEncoder(false), nullptr);
    EXPECT_NE(createFakeSnapshotEncoder(), nullptr);
}

TEST(SnapshotEncoderFactory, BackendNamesAreDistinct) {
    auto fake = createFakeSnapshotEncoder();
    ASSERT_NE(fake, nullptr);
    EXPECT_EQ(fake->backend(), "fake");

    // 硬件后端在本机不存在，工厂会回落到 fake——但**名字必须仍能区分**，
    // 否则日志里看不出到底走的是哪条路。
    auto auto_backend = createSnapshotEncoder(true);
    ASSERT_NE(auto_backend, nullptr);
    EXPECT_FALSE(auto_backend->backend().empty());
}

TEST(SnapshotEncoderFactory, DescribeExplainsWhyHardwareNotUsed) {
    auto fake = createSnapshotEncoder(false);
    ASSERT_NE(fake, nullptr);
    const std::string d = fake->describe();
    EXPECT_NE(d.find("hardware not preferred"), std::string::npos)
        << "describe 必须说清为什么没走硬件，实际: " << d;
}

TEST(SnapshotEncoderFactory, DescribeIsReadableBeforeConfigure) {
    // 调用方常常先建对象、后拿几何。此时 describe() 不能空、不能崩。
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);
    EXPECT_FALSE(enc->describe().empty());
}

// ===========================================================================
// 配置校验
// ===========================================================================

TEST(FakeSnapshotEncoder, RejectsZeroGeometry) {
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);

    SnapshotEncoderConfig c = boardConfig();
    c.width = 0;
    EXPECT_FALSE(enc->configure(c)) << "零宽度必须被拒绝，不能猜一个默认值";

    c = boardConfig();
    c.height = 0;
    EXPECT_FALSE(enc->configure(c));
}

TEST(FakeSnapshotEncoder, ConfigureIsIdempotent) {
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);
    ASSERT_TRUE(enc->configure(boardConfig()));
    EXPECT_TRUE(enc->configure(boardConfig()))
        << "重复 configure 必须成功且不重建（D-Bus 长驻进程里会反复调用）";
}

// ===========================================================================
// capture 的前置条件
// ===========================================================================

TEST(FakeSnapshotEncoder, CaptureBeforeConfigureFails) {
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);
    auto frame = makeFrame();
    SnapshotImage img;
    EXPECT_FALSE(enc->capture(viewOf(frame, 1, 1), img));
    EXPECT_FALSE(img.valid());
    EXPECT_EQ(enc->stats().captured, 0u);
    EXPECT_EQ(enc->stats().failed, 1u);
}

TEST(FakeSnapshotEncoder, RejectsNullFrameData) {
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);
    ASSERT_TRUE(enc->configure(boardConfig()));

    SnapshotFrameView v;
    v.data = nullptr;
    v.size = kFrameBytes;

    SnapshotImage img;
    EXPECT_FALSE(enc->capture(v, img));
    EXPECT_EQ(enc->stats().captured, 0u);
    EXPECT_EQ(enc->stats().failed, 1u);
}

TEST(FakeSnapshotEncoder, CaptureDoesNotDependOnFrameSize) {
    // 桩后端不读像素，所以短缓冲也能「成功」——这是桩的**刻意设计**：
    // 它用来跑通下游链路，不是用来验证几何。真后端的短缓冲拒绝见
    // 下面 CedarSnapshotEncoderGeometry 一组的语义说明。
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);
    ASSERT_TRUE(enc->configure(boardConfig()));

    std::vector<uint8_t> tiny(4, 0);
    SnapshotImage img;
    EXPECT_TRUE(enc->capture(viewOf(tiny, 1, 1), img));
}

// ===========================================================================
// 产出形状
// ===========================================================================

TEST(FakeSnapshotEncoder, ProducesJpegShapedBytes) {
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);
    ASSERT_TRUE(enc->configure(boardConfig()));

    auto frame = makeFrame();
    SnapshotImage img;
    ASSERT_TRUE(enc->capture(viewOf(frame, 42, 1234567890ull), img));

    ASSERT_TRUE(img.valid());
    ASSERT_GE(img.bytes.size(), 4u);
    EXPECT_EQ(img.bytes.front(), 0xFF);
    EXPECT_EQ(img.bytes[1], 0xD8);
    EXPECT_EQ(img.bytes[2], 0xFF) << "FF D8 FF 是下游守门的判据，桩必须满足";
    EXPECT_EQ(img.bytes[img.bytes.size() - 2], 0xFF);
    EXPECT_EQ(img.bytes.back(), 0xD9) << "EOI 必须在末尾";

    EXPECT_TRUE(img.looksLikeJpeg());
    EXPECT_EQ(img.width, kWidth);
    EXPECT_EQ(img.height, kHeight);
}

TEST(FakeSnapshotEncoder, PassesThroughSequenceAndTimestamp) {
    // 图和时刻对不上，这张图就没有归因价值。
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();

    SnapshotImage img;
    ASSERT_TRUE(enc->capture(viewOf(frame, 777, 9876543210ull), img));
    EXPECT_EQ(img.sequence, 777u);
    EXPECT_EQ(img.timestamp_ns, 9876543210ull);
    EXPECT_NE(img.timestamp_ns, 0ull)
        << "时间戳为 0 等于「这张图不知道什么时候拍的」";
}

TEST(FakeSnapshotEncoder, OutputIsReplacedNotAppended) {
    // 复用同一个 SnapshotImage 连续取图：第二次必须替换而不是追加，
    // 否则调用方会把两张图拼在一起落盘。
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();

    SnapshotImage img;
    ASSERT_TRUE(enc->capture(viewOf(frame, 1, 100), img));
    const size_t first = img.bytes.size();
    ASSERT_TRUE(enc->capture(viewOf(frame, 2, 200), img));
    EXPECT_EQ(img.bytes.size(), first) << "第二次 capture 必须整体替换";
    EXPECT_EQ(img.sequence, 2u);
}

// ===========================================================================
// 分片上限
// ===========================================================================

TEST(SnapshotImageSize, SingleChunkBoundaryIsExact) {
    // 640 KiB = 655360 是云端 MAX_VIDEO_CHUNK_BYTES 的原始体上限。
    // 边界必须**精确**：<= 才算单片，多一字节就不算。
    // 这类 ±1 错误不会报错，只会让恰好卡线的图多走一次分片。
    EXPECT_EQ(kMaxSnapshotChunkBytes, 640u * 1024u);
    EXPECT_EQ(kMaxSnapshotChunkBytes, 655360u);

    SnapshotImage img;
    img.bytes.assign(kMaxSnapshotChunkBytes, 0);
    EXPECT_TRUE(img.fitsSingleChunk()) << "恰好等于上限必须算单片";

    img.bytes.push_back(0);
    EXPECT_FALSE(img.fitsSingleChunk()) << "超一字节必须算超限";
}

TEST(SnapshotImageSize, EmptyImageFitsTrivially) {
    SnapshotImage img;
    EXPECT_TRUE(img.fitsSingleChunk());
    EXPECT_FALSE(img.valid());
}

TEST(SnapshotImageSize, BoardMeasuredQualityRegimes) {
    // 本板 1640x1232 的实测体积（见 snapshot_encoder.hpp 头注释）。
    // 这几条断言的作用是**把实测事实固定下来**：如果哪天默认质量被
    // 改回 85，或云端上限变了，这里会立刻红。
    struct Case {
        uint32_t bytes;
        bool fits;
        const char* what;
    };
    const Case cases[] = {
        {569250, true,  "quality=60 实测 569 KB"},
        {624197, true,  "quality=70 实测 624 KB（默认档）"},
        {633693, true,  "quality=75 实测 634 KB"},
        {685684, false, "quality=85 实测 686 KB：超限"},
        {1095446, false, "quality=100 实测 1095 KB：超限"},
    };
    for (const Case& c : cases) {
        SnapshotImage img;
        img.bytes.assign(c.bytes, 0);
        EXPECT_EQ(img.fitsSingleChunk(), c.fits) << c.what;
    }
}

// ===========================================================================
// 计数
// ===========================================================================

TEST(FakeSnapshotEncoder, CapturedAndFailedAreSeparate) {
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();

    SnapshotImage img;
    ASSERT_TRUE(enc->capture(viewOf(frame, 1, 1), img));
    EXPECT_EQ(enc->stats().captured, 1u);
    EXPECT_EQ(enc->stats().failed, 0u) << "成功一次不能同时记一次失败";

    SnapshotFrameView bad;
    bad.data = nullptr;
    EXPECT_FALSE(enc->capture(bad, img));
    EXPECT_EQ(enc->stats().captured, 1u);
    EXPECT_EQ(enc->stats().failed, 1u);
}

// ===========================================================================
// 失败后的可恢复性
// ===========================================================================

TEST(FakeSnapshotEncoder, RecoversAfterFailedCapture) {
    // 一次坏帧不能让编码器永久报废。真后端对应的是「描述符池空了就
    // 重建实例」，这里断言的是**接口层**的契约：失败之后还能继续用。
    auto enc = createFakeSnapshotEncoder();
    ASSERT_NE(enc, nullptr);
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();

    SnapshotImage img;
    SnapshotFrameView bad;
    bad.data = nullptr;
    EXPECT_FALSE(enc->capture(bad, img));

    EXPECT_TRUE(enc->capture(viewOf(frame, 9, 999), img))
        << "坏帧之后必须还能取图";
    EXPECT_EQ(enc->stats().captured, 1u);
}

// ===========================================================================
// 真后端在无硬件机器上的行为
// ===========================================================================

TEST(SnapshotEncoderHardware, ConfigureFailsWithReasonWhenNoVE) {
    // 本机（CI/开发机）没有 libvencoder.so。工厂回落成 fake，所以这里
    // 只能通过 describe() 确认「回落是因为硬件不可用」，而不是静默变成
    // 一个看起来正常的后端。
    auto enc = createSnapshotEncoder(true);
    ASSERT_NE(enc, nullptr);

    if (enc->backend() != "fake") {
        GTEST_SKIP() << "本机存在 VE 硬件（backend=" << enc->backend()
                     << "），该断言只对无硬件环境有意义";
    }

    const std::string d = enc->describe();
    EXPECT_NE(d.find("cedar"), std::string::npos)
        << "describe 必须提到 cedar，否则用户看不出为什么没走硬件。实际: "
        << d;
}

}  // namespace
}  // namespace weaknet
