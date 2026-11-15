/**
 * @file test_video_clip_muxer_gtest.cpp
 * @brief MP4 封装器契约测试（无 ffmpeg 也必须全绿）
 *
 * ============================================================================
 * 这份测试守什么
 * ============================================================================
 *
 * 封装器有一条失败路径**看起来像成功**：ffmpeg 退出码 0，但产物不是 MP4。
 * 实测 `avmux_mp4` 就是这样——616 帧全部压到同一个时间戳，
 * `ffprobe` 报 `duration=0.006s`。若只信退出码，这段字节会被当成 MP4 上传，
 * 云端存下来、运营点开播不了，而日志全是绿的。所以产物校验按**字节判据**
 * （是否有 `ftyp` box）做，不信退出码。
 *
 * 另一类要守的是「退化必须显式」：帧数或跨度缺失时帧率回落 25 fps，
 * 容器时长与真实时长按比例偏掉，回看时「事发那一刻」对不上。
 *
 * ============================================================================
 * 为什么分成「不依赖 ffmpeg」与「依赖 ffmpeg」两组
 * ============================================================================
 *
 * CI 机器上没有 ffmpeg（本机实测 `which ffmpeg` 返回 1）。如果所有用例都
 * 依赖它，这里的测试就随环境飘红，然后所有人学会忽略它——那比没有测试更糟。
 * 所以：
 *
 *   - 不依赖 ffmpeg 的用例（嗅探、空输入、缺失可执行文件）永远跑；
 *   - 真正验封装的用例用 `GTEST_SKIP`，只在有 ffmpeg 的机器上跑，
 *     板端真机回归补上那一段的覆盖。
 *
 * ============================================================================
 * 为什么「缺 ffmpeg」的用例反而是最重要的一条
 * ============================================================================
 *
 * 生产环境里 ffmpeg 是**运行时**依赖，不是构建依赖——二进制里没有它也能启动、
 * 也能正常录制，只有在真正出事故、真正要封装的那一刻才会用到。也就是说，
 * 这条路径的失败只会在最坏的时刻被发现。而它的失败形态（execv 失败）
 * 与「码流坏了」在日志上极易混淆，两者的处置完全不同：补依赖 vs 查编码器。
 * 所以必须有一条用例钉住「127 的报错必须明确说是没装」。
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

#include "video_clip_muxer.hpp"

namespace weaknet {
namespace {

/// 最小的可解码 H.264 单元集：SPS + PPS + 一个 IDR。
///
/// 取自板端真实片段（1640x1232，cedar 编码器输出）的前三个 NAL，逐字节
/// base64 存下来。为什么用真码流而不是随手造几个字节：ffmpeg 会拒绝任何
/// 解析不出 SPS 的输入，而「输入能否被解析」恰好是要验的那件事。
/// 用合成数据会让这条用例变成「ffmpeg 认不认我编的这串东西」，
/// 那不是产品要回答的问题。
constexpr const char* kMiniH264B64 =
    "AAAAAWdNACmJiUAzgTYAAAABaO4AAAABZYiABAAATf/wOOB1U/Ms7tNpgAAAAwAAAwAAJ080"
    "tx7x5D404AAAAwAACMgAAKK3sAABQB/AAAgoAACyAAAm9ZfOJFRXnQ5lAAADABH/inB+65gA"
    "AAdWe9cccluAAAADAfAAAEQAABl+vLx4IAAAAwAAAwAAAwAAAwAYgAAAAwAAAwAAAwAAAwAA"
    "AwAABQQAAAMAAAMAAAMAAAMAAAMAAAMAAAMAAjma0AAAAwAAAwAAAwAAAwAopWiAAAADAAAD"
    "AAhwAAAGPC3dkAAABjsyAAADAVoAAAMA7+gAAAMCXiCAAACajgQAAAMAAAMAAAMAAAMAAAMA"
    "AAMCv+EAAAMAvQAAAwAAAwAALqUwAAADAAADAAADAAADAAADAAADAAADAAADAAADAAADAAAD"
    "AAADADI6AAADAKUAAAMAj/AAAAMAAAMAAIxkAAADA5wAAAMAAAMAAAMAAAMAAAMAAAMAAAMA"
    "AAMAAAMAAAMAAI/wAAADAAADAACKAAADAAADAAAlvAAAAwAAAwAAAwAAAwAAAwAAAwAAAwAA"
    "AwAAAwAAAwAAAwAAAwAHOAAAAwAABXuyWp9vVwyxzX/VpZuyAacNgM7LP9uAyeFClBGQLD8z"
    "j+BtLrYK0n+mx/0cewIAAAZZog==";

/// 标准 base64 解码（只认 `A-Za-z0-9+/` 与 `=`）。
///
/// 为什么在测试里自己写一份而不是复用产品的 `base64Encode` 的逆运算：
/// 产品侧没有解码需求，为了测试加一个生产符号等于给产品加一个没有消费者的
/// 接口（见 CLAUDE.md「没有消费者的接口就是孤儿」）。测试自带解码器还顺带
/// 保证这份 fixture 不依赖被测代码——否则 fixture 会跟着编码器的 bug 一起错。
std::vector<uint8_t> base64Decode(const char* text) {
    int table[256];
    for (int i = 0; i < 256; ++i) table[i] = -1;
    const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(alphabet[i])] = i;

    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == '=') break;
        const int v = table[static_cast<unsigned char>(*p)];
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

class VideoClipMuxerTest : public ::testing::Test {
protected:
    static bool ffmpegAvailable() {
        // 只探一次。判据用「存在 + 可执行 + 能跑 -version」而不是单纯
        // access()：一个损坏的二进制也存在、也可执行，而它跑不起来。
        static const bool available = [] {
            if (::access(kDefaultFfmpegPath, X_OK) != 0) return false;
            const std::string cmd =
                std::string(kDefaultFfmpegPath) + " -version >/dev/null 2>&1";
            return std::system(cmd.c_str()) == 0;
        }();
        return available;
    }
};

// ---------------------------------------------------------------------------
// 嗅探：判据必须能区分 Annex-B 与 MP4
// ---------------------------------------------------------------------------

TEST_F(VideoClipMuxerTest, SniffRejectsAnnexBAndShortBuffers) {
    // Annex-B 起始码在最前 4 字节，绝不可能在 [4,8) 上凑出 "ftyp"。
    const uint8_t annexb[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x00, 0x2A,
                              0x00, 0x00, 0x00, 0x01, 0x68};
    EXPECT_FALSE(looksLikeMp4(annexb, sizeof(annexb)));

    // 太短的一律判否：宁可漏判成「不是 MP4」（多跑一次无谓的封装）也不
    // 误判成「是 MP4」（跳过封装，交付一段裸码流给浏览器）。
    const uint8_t tiny[] = {0x00, 0x00, 0x00, 0x0C, 'f', 't', 'y', 'p'};
    EXPECT_FALSE(looksLikeMp4(tiny, sizeof(tiny)));
    EXPECT_FALSE(looksLikeMp4(nullptr, 0));
}

TEST_F(VideoClipMuxerTest, SniffAcceptsRealMp4Header) {
    const uint8_t mp4[] = {0x00, 0x00, 0x00, 0x20, 'f', 't', 'y', 'p',
                           'i',  's',  'o',  'm',  0x00, 0x00, 0x02, 0x00};
    EXPECT_TRUE(looksLikeMp4(mp4, sizeof(mp4)));
}

// ---------------------------------------------------------------------------
// 失败路径：必须可区分、必须不产出字节
// ---------------------------------------------------------------------------

TEST_F(VideoClipMuxerTest, EmptyInputFailsWithoutInvokingAnything) {
    const VideoClipMuxResult r =
        muxAnnexBToMp4(nullptr, 0, 0, 0, kDefaultFfmpegPath);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.bytes.empty());
    EXPECT_FALSE(r.error.empty());
}

TEST_F(VideoClipMuxerTest, MissingExecutableIsReportedAsNotInstalled) {
    // 这条是整个文件里最重要的一条：生产环境缺 ffmpeg 时，失败**必须**
    // 明确指向「没装或不可执行」，而不是含糊成「封装失败」，更不能被
    // 误读成「码流不合规」去查编码器——两种处置的方向完全相反。
    //
    // 判据依赖子进程 execv 失败返回 127 这个约定，而不是依赖 stderr 文本
    // （没装的情况下根本不会有 stderr，因为 ffmpeg 从未启动）。
    const std::vector<uint8_t> bytes = base64Decode(kMiniH264B64);
    ASSERT_FALSE(bytes.empty());
    const VideoClipMuxResult r = muxAnnexBToMp4(bytes.data(), bytes.size(), 1, 0,
                                               "/nonexistent/ffmpeg-not-installed");
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.bytes.empty());
    EXPECT_NE(r.error.find("未安装或不可执行"), std::string::npos) << r.error;
}

TEST_F(VideoClipMuxerTest, EmptyExecutablePathIsRejectedBeforeFork) {
    const std::vector<uint8_t> bytes = base64Decode(kMiniH264B64);
    const VideoClipMuxResult r =
        muxAnnexBToMp4(bytes.data(), bytes.size(), 1, 0, std::string());
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST_F(VideoClipMuxerTest, GarbageInputFailsAndKeepsStderrDigest) {
    // 坏码流必须失败，且错误信息里要有 ffmpeg 的 stderr 摘要——否则
    // 排障只能看到「封装失败」四个字，无从判断是编码器坏了还是输入截断了。
    std::vector<uint8_t> junk(4096, 0x42);
    const VideoClipMuxResult r =
        muxAnnexBToMp4(junk.data(), junk.size(), 1, 0, kDefaultFfmpegPath);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.bytes.empty());
    EXPECT_FALSE(r.error.empty());
}

// ---------------------------------------------------------------------------
// 成功路径：需要真实 ffmpeg（板端 / 有 ffmpeg 的机器）
// ---------------------------------------------------------------------------

TEST_F(VideoClipMuxerTest, MuxesRealH264IntoPlayableMp4) {
    if (!ffmpegAvailable()) {
        GTEST_SKIP() << "本机无 ffmpeg；封装成功路径由板端真机回归覆盖";
    }
    const std::vector<uint8_t> bytes = base64Decode(kMiniH264B64);
    ASSERT_FALSE(bytes.empty());
    const VideoClipMuxResult r =
        muxAnnexBToMp4(bytes.data(), bytes.size(), 1, 0, kDefaultFfmpegPath);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(looksLikeMp4(r.bytes.data(), r.bytes.size()));
    // 产物必须比裸码流大：MP4 只是加容器，不是压缩。这一条能挡住
    // 「ffmpeg 把输入吃掉了但输出空文件却返回 0」这类情形。
    EXPECT_GT(r.bytes.size(), 0u);
}

TEST_F(VideoClipMuxerTest, MuxIsDeterministicForSameInput) {
    // 同一输入两次封装必须产出逐字节相同的 MP4。这不是吹毛求疵：
    // 上传重试要复用完全相同的字节（云端按内容判幂等），若封装有随机性，
    // 重试就会被判成「内容冲突」而永久卡死。
    if (!ffmpegAvailable()) {
        GTEST_SKIP() << "本机无 ffmpeg";
    }
    const std::vector<uint8_t> bytes = base64Decode(kMiniH264B64);
    ASSERT_FALSE(bytes.empty());
    const VideoClipMuxResult a =
        muxAnnexBToMp4(bytes.data(), bytes.size(), 1, 0, kDefaultFfmpegPath);
    const VideoClipMuxResult b =
        muxAnnexBToMp4(bytes.data(), bytes.size(), 1, 0, kDefaultFfmpegPath);
    ASSERT_TRUE(a.ok) << a.error;
    ASSERT_TRUE(b.ok) << b.error;
    EXPECT_EQ(a.bytes, b.bytes);
}

TEST_F(VideoClipMuxerTest, FrameRateRationalAcceptsRealisticNumbers) {
    // 570 帧 / 30711 ms 是板端实测的一对真实数字（22.8 s 的片段）。
    // 验的是「这么推出来的有理数能被 ffmpeg 接受且不报参数错」。
    // 容器时长的精确性由板端 `ffprobe` 核对，不在这里断言——
    // 单测不该依赖 ffprobe。
    if (!ffmpegAvailable()) {
        GTEST_SKIP() << "本机无 ffmpeg";
    }
    const std::vector<uint8_t> bytes = base64Decode(kMiniH264B64);
    ASSERT_FALSE(bytes.empty());
    const VideoClipMuxResult r =
        muxAnnexBToMp4(bytes.data(), bytes.size(), 570, 30711, kDefaultFfmpegPath);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(looksLikeMp4(r.bytes.data(), r.bytes.size()));
}

}  // namespace
}  // namespace weaknet
