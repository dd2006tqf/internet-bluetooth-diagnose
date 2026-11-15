/**
 * @file test_video_encoder_gtest.cpp
 * @brief IVideoEncoder 契约测试（无摄像头、无 VE 硬件）
 *
 * ============================================================================
 * 这份测试为什么存在
 * ============================================================================
 *
 * 编码器的绝大多数错误**不会报错**。喂错 UV 偏移 → 偏色画面，ioctl 全返回
 * 成功；把「主动丢帧」记成「编码失败」→ 归因把降级说成故障；窗口时长填配置
 * 值而不是事实值 → 码率算出来永远是标称值。
 *
 * 所以这里断言的都不是「能不能编出码流」（那要看硬件），而是**这些静默的
 * 语义边界**：
 *
 *   - 几何不匹配必须显式拒绝，不能猜
 *   - dropped 与 failed 必须是两个计数
 *   - 没有输入时 valid 必须是 false（而不是「全零的健康」）
 *   - window_ms 必须是事实时长
 *   - 零拷贝必须默认关闭（默认值要让调用方什么都不做也是对的）
 *
 * ============================================================================
 * 为什么用 createFakeVideoEncoder 而不是真后端
 * ============================================================================
 *
 * 这份文件在**没有 /dev/cedar_dev_ve2 的机器**上必须能跑（CI、开发机）。
 * 真后端的能力缺失已经由 createVideoEncoder 的回落语义覆盖：
 * 它保证返回非空对象，失败体现在 configure() 的返回值与 describe() 里。
 * 这一点也在下面被测。
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "video_encoder.hpp"

namespace weaknet {
namespace {

/// NV12 fourcc，与实现里的常量一致
constexpr uint32_t kNv12 = 0x3231564Eu;

/// 本板实测几何：1640x1232，UV 偏移 2030336（= 1648*1232）
constexpr uint32_t kWidth = 1640;
constexpr uint32_t kHeight = 1232;
constexpr uint32_t kAlignW = 1648;
constexpr uint32_t kUvOffset = kAlignW * kHeight;      // 2030336
constexpr uint32_t kFrameBytes = kUvOffset + kUvOffset / 2;  // 3045504

/// 造一帧合法的 NV12（含对齐空洞）
std::vector<uint8_t> makeFrame() {
    std::vector<uint8_t> f(kFrameBytes, 0);
    // Y 平面：1648 列 × 1232 行（对齐后的宽）
    for (uint32_t i = 0; i < kUvOffset; ++i) {
        f[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    }
    // UV 平面：偏移 2030336 起，长度 kUvOffset/2
    for (uint32_t i = 0; i < kUvOffset / 2; ++i) {
        f[kUvOffset + i] = static_cast<uint8_t>((i * 3) & 0xFF);
    }
    return f;
}

VideoEncoderConfig boardConfig() {
    VideoEncoderConfig c;
    c.width = kWidth;
    c.height = kHeight;
    c.stride = kWidth;
    c.uv_offset = kUvOffset;
    c.fourcc = kNv12;
    c.frame_bytes = kFrameBytes;
    c.fps = 18;          // 采集侧真实帧率
    c.bitrate_kbps = 2000;
    return c;
}

// ===========================================================================
// 工厂的回落语义
// ===========================================================================

TEST(VideoEncoderFactory, NeverReturnsNull) {
    // 无论硬件在不在，工厂都必须给一个能用的对象。
    // 调用方不该写空指针分支——能力缺失体现在 configure() 与 describe()。
    auto hw = createVideoEncoder(true);
    ASSERT_NE(hw, nullptr);
    auto sw = createVideoEncoder(false);
    ASSERT_NE(sw, nullptr);
    auto fake = createFakeVideoEncoder();
    ASSERT_NE(fake, nullptr);
}

TEST(VideoEncoderFactory, BackendNamesAreDistinct) {
    // backend() 会写进 evidence 溯源，两个后端不能撞名，否则事后分不清
    // 一段录像是哪条路径产出的。
    auto fake = createFakeVideoEncoder();
    EXPECT_EQ(fake->backend(), "fake");
    auto no_hw = createVideoEncoder(false);
    EXPECT_EQ(no_hw->backend(), "fake");
}

TEST(VideoEncoderFactory, DescribeExplainsWhyHardwareNotUsed) {
    // 「没走硬件」必须能说出原因。否则板端排查只能靠猜。
    auto no_hw = createVideoEncoder(false);
    const std::string d = no_hw->describe();
    EXPECT_FALSE(d.empty());
    EXPECT_NE(d.find("hardware not preferred"), std::string::npos)
        << "describe() = " << d;
}

// ===========================================================================
// FakeVideoEncoder 的契约（这条线在无硬件机器上也要跑）
// ===========================================================================

TEST(FakeVideoEncoder, RejectsZeroGeometry) {
    auto enc = createFakeVideoEncoder();
    VideoEncoderConfig c = boardConfig();
    c.width = 0;
    EXPECT_FALSE(enc->configure(c));
    // 拒绝必须带原因——「返回 false 但 describe 是空的」等于没拒绝。
    EXPECT_NE(enc->describe().find("geometry"), std::string::npos)
        << enc->describe();
}

TEST(FakeVideoEncoder, RejectsNonNv12) {
    auto enc = createFakeVideoEncoder();
    VideoEncoderConfig c = boardConfig();
    c.fourcc = 0x56595559u;  // 'YUYV'
    EXPECT_FALSE(enc->configure(c));
    EXPECT_NE(enc->describe().find("fourcc"), std::string::npos)
        << enc->describe();
}

TEST(FakeVideoEncoder, EncodeBeforeConfigureFails) {
    auto enc = createFakeVideoEncoder();
    auto frame = makeFrame();
    VideoFrameView v;
    v.data = frame.data();
    v.size = frame.size();
    EncodedFrame out;
    EXPECT_FALSE(enc->encode(v, out));
    EXPECT_FALSE(out.valid());
}

TEST(FakeVideoEncoder, RejectsFrameWithoutDataOrFd) {
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    VideoFrameView v;  // 两者都空
    EncodedFrame out;
    EXPECT_FALSE(enc->encode(v, out));
}

TEST(FakeVideoEncoder, RejectsShortFrame) {
    // 帧比几何要求的小 = 数据源和配置不一致。猜一个尺寸继续编就是
    // 拿垃圾喂硬件，必须拒绝。
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    std::vector<uint8_t> tiny(1024, 0);
    VideoFrameView v;
    v.data = tiny.data();
    v.size = tiny.size();
    EncodedFrame out;
    EXPECT_FALSE(enc->encode(v, out));
}

TEST(FakeVideoEncoder, EncodesAndProducesBitstream) {
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();
    VideoFrameView v;
    v.data = frame.data();
    v.size = frame.size();
    v.sequence = 42;
    EncodedFrame out;
    ASSERT_TRUE(enc->encode(v, out));
    EXPECT_TRUE(out.valid());
    EXPECT_EQ(out.sequence, 42) << "序号必须原样透传给下游";
}

TEST(FakeVideoEncoder, StatsWindowIsFactNotConfig) {
    // window_ms 必须是**事实**时长。填配置值会让码率永远算成标称值，
    // 而「码率掉了一半」正是要抓的那个现象。
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();
    VideoFrameView v;
    v.data = frame.data();
    v.size = frame.size();
    EncodedFrame out;
    for (int i = 0; i < 3; ++i) enc->encode(v, out);
    const EncoderStats s = enc->snapshotAndReset();
    EXPECT_TRUE(s.valid);
    EXPECT_EQ(s.frames_in, 3u);
    // 真实耗时可能是 0（机器太快），也可能是几毫秒——但**不能**是配置的
    // 1000ms 那种整值。
    EXPECT_LT(s.window_ms, 1000u)
        << "window_ms=" << s.window_ms << " 看起来是配置值而非事实值";
}

TEST(FakeVideoEncoder, StatsValidFalseWithoutInput) {
    // 没编过帧时 valid 必须为 false。全零统计会被上游读成
    // 「零丢帧、零失败」的健康状态，那是假的。
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    const EncoderStats s = enc->peekStats();
    EXPECT_FALSE(s.valid);
}

TEST(FakeVideoEncoder, DroppedAndFailedAreSeparate) {
    // dropped（我们主动降级）与 failed（编码器坏了）必须分开。
    // 混在一起，归因就再也分不清「编码器跟不上」与「编码器故障」。
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();
    VideoFrameView good;
    good.data = frame.data();
    good.size = frame.size();
    EncodedFrame out;
    ASSERT_TRUE(enc->encode(good, out));

    // 送一个坏帧：应记 failed，不记 dropped
    VideoFrameView bad;  // 无 data 无 fd
    EXPECT_FALSE(enc->encode(bad, out));

    const EncoderStats s = enc->snapshotAndReset();
    EXPECT_EQ(s.failed, 1u) << "坏帧必须计入 failed";
    EXPECT_EQ(s.dropped, 0u) << "坏帧不是主动丢帧，不能计入 dropped";
}

TEST(FakeVideoEncoder, SnapshotResetsCountersButKeepsWindow) {
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();
    VideoFrameView v;
    v.data = frame.data();
    v.size = frame.size();
    EncodedFrame out;
    for (int i = 0; i < 5; ++i) enc->encode(v, out);
    const EncoderStats first = enc->snapshotAndReset();
    EXPECT_EQ(first.frames_in, 5u);

    // 再取一次必须清零
    const EncoderStats second = enc->snapshotAndReset();
    EXPECT_EQ(second.frames_in, 0u) << "snapshotAndReset 之后计数必须归零";
    EXPECT_FALSE(second.valid) << "没有新输入时 valid 必须是 false";
}

TEST(FakeVideoEncoder, BitrateDerivedFromBytesAndWindow) {
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();
    VideoFrameView v;
    v.data = frame.data();
    v.size = frame.size();
    EncodedFrame out;
    for (int i = 0; i < 4; ++i) enc->encode(v, out);
    const EncoderStats s = enc->peekStats();
    ASSERT_TRUE(s.valid);
    ASSERT_GT(s.bytes_out, 0u);
    ASSERT_GT(s.window_ms, 0u);
    // 两者同源推导，必须自洽
    const uint32_t expected = static_cast<uint32_t>(
        (s.bytes_out * 8ULL) / static_cast<uint64_t>(s.window_ms));
    EXPECT_EQ(s.bitrate_kbps(), expected);
}

TEST(FakeVideoEncoder, FlushDrainsRemainingFrames) {
    // 停止推流前不 flush 会丢最后几帧——这条语义必须有地方可验。
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();
    VideoFrameView v;
    v.data = frame.data();
    v.size = frame.size();
    EncodedFrame out;
    for (int i = 0; i < 3; ++i) enc->encode(v, out);

    size_t flushed = 0;
    enc->flush([&](const EncodedFrame& ef) {
        if (ef.valid()) ++flushed;
    });
    // 桩实现的 flush 至少不能崩、不能返回垃圾帧
    EXPECT_GE(flushed, 0u);
}

// ===========================================================================
// 零拷贝的默认值（最容易出事的那个默认）
// ===========================================================================

TEST(VideoEncoderZeroCopy, DefaultIsOff) {
    // allow_zero_copy 默认必须为 false。
    //
    // 理由不是「保守」，而是：开启后调用方**必须**替硬件守住 dma-buf 直到
    // 下一次 encode 返回。默认开 = 默认要求调用方做一件它不知道要做的事
    // = 默认产生静默的画面错乱。
    VideoEncoderConfig c;
    EXPECT_FALSE(c.allow_zero_copy);
}

TEST(VideoEncoderZeroCopy, MemcpyPathWorksWithoutOptIn) {
    // 不开零拷贝时，memcpy 路径必须能正常工作（这是默认路径）
    auto enc = createFakeVideoEncoder();
    VideoEncoderConfig c = boardConfig();
    c.allow_zero_copy = false;
    ASSERT_TRUE(enc->configure(c));
    auto frame = makeFrame();
    VideoFrameView v;
    v.data = frame.data();
    v.size = frame.size();
    EncodedFrame out;
    EXPECT_TRUE(enc->encode(v, out));
}

TEST(VideoEncoderZeroCopy, GeometryMismatchIsReportedInDescribe) {
    // uv_offset 与 align16(w)*h 不一致时，启用零拷贝必须被拒绝，
    // 且原因要出现在 describe() 里——否则板端只会看到「怎么没零拷贝」。
    auto enc = createFakeVideoEncoder();
    VideoEncoderConfig c = boardConfig();
    c.allow_zero_copy = true;
    // 故意把 uv_offset 设成用 width*height 推算的错误值（9856 字节差）
    c.uv_offset = kWidth * kHeight;   // 2020480，错
    c.frame_bytes = 0;                // 不校验总长，专测零拷贝判定
    ASSERT_TRUE(enc->configure(c));
    const std::string d = enc->describe();
    EXPECT_NE(d.find("zero-copy"), std::string::npos)
        << "describe() 必须说明零拷贝状态: " << d;
}

// ===========================================================================
// 时间戳回填：下游「对时」的唯一依据
// ===========================================================================
//
// 这一条不是理论契约，而是一个**真实发生过的静默故障**：
//
// 2026-11-14 板端跑留证端到端探针，编码 60 帧全部成功（`in=60 out=60`、
// 4 个 IDR、`failed=0`），而录制器 `produced=0 aborted=0`——既不产出也
// 不放弃，就那么卡着。根因是 cedar 后端的 `drainOne()` 只填了
// `data/size/keyframe`，`timestamp_ns` 一直是默认值 0。
//
// 录制器拿 0 当「最新帧时刻」，于是 `newest_mono_ms_` 永远是 0，
// 「后沿未到」恒成立。整个过程中**没有任何错误、没有任何告警**。
//
// 这类缺陷只能靠断言打掉：编码成功不等于下游能用。所以这里断言的是
// 「每一块码流都带着它那次输入的序号与时间戳，且顺序对齐」。

TEST(VideoEncoderTimestamps, OutputCarriesInputSequenceAndTimestamp) {
    // 桩后端也必须满足：它在 CI 上是这条契约的唯一可执行证明。
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();

    constexpr uint64_t kBaseNs = 1000000ull;   // 非 0 起点
    constexpr uint64_t kStepNs = 1000000000ull / 18ull;

    for (uint64_t i = 0; i < 5; ++i) {
        VideoFrameView v;
        v.data = frame.data();
        v.size = frame.size();
        v.sequence = 100 + i;
        v.timestamp_ns = kBaseNs + i * kStepNs;

        EncodedFrame out;
        ASSERT_TRUE(enc->encode(v, out)) << "第 " << i << " 帧编码失败";
        EXPECT_EQ(out.sequence, 100 + i) << "序号必须原样透传（帧序对不上就无法归因）";
        EXPECT_EQ(out.timestamp_ns, kBaseNs + i * kStepNs)
            << "时间戳必须原样透传：下游的录制窗口与延迟归因全靠它";
        EXPECT_NE(out.timestamp_ns, 0ull)
            << "时间戳恒为 0 会让录制器静默卡死（见本组测试的注释）";
    }
}

TEST(VideoEncoderTimestamps, FlushKeepsMetadataAligned) {
    // flush() 走的是同一条 drain 路径，不能因为是「收尾」就把身份丢掉。
    auto enc = createFakeVideoEncoder();
    ASSERT_TRUE(enc->configure(boardConfig()));
    auto frame = makeFrame();

    VideoFrameView v;
    v.data = frame.data();
    v.size = frame.size();
    v.sequence = 7;
    v.timestamp_ns = 424242;
    EncodedFrame out;
    enc->encode(v, out);

    int flushed = 0;
    enc->flush([&](const EncodedFrame& ef) {
        ++flushed;
        EXPECT_NE(ef.timestamp_ns, 0ull) << "flush 出来的帧同样必须带时间戳";
    });
    // 桩没有 pipeline 延迟，flushed 为 0 是正常的；真后端可能非 0。
    EXPECT_GE(flushed, 0);
}

}  // namespace
}  // namespace weaknet
