/**
 * @file test_video_clip_exporter_gtest.cpp
 * @brief VideoClipExporter 契约测试（无网络、无开发板、无摄像头）
 *
 * ============================================================================
 * 这份测试为什么存在
 * ============================================================================
 *
 * 留证上行器的绝大多数错误**不会当场报错**：
 *
 *   - URL 派生错了 → 把带签名的证据发到未知端点，或者静默 404；
 *   - 分片边界算错 → 收齐后拼出来的码流多一段或少一段，播放器「文件损坏」；
 *   - base64 少了 padding → 云端 `validate=True` 直接 422，而本地看起来很正常；
 *   - 清单字段名写错 → 云端 422，但错误码在几百个字段里淹掉；
 *   - 重试时重新序列化 → 云端把它判成「内容冲突」，片段永久卡死；
 *   - 队列满了静默丢弃 → 事故发生时才发现最关键那段没了。
 *
 * 所以这里断言的都是**这些静默的契约边界**，而不是「能不能真的传上去」
 * （那要靠开发板 + 云端联调）。
 *
 * ============================================================================
 * 为什么不需要网络
 * ============================================================================
 *
 * `runOnce()` 的网络失败路径用「地址不可达」覆盖：只要能确认失败时
 * **不推进进度、计数正确**，传输层本身的行为由 libcurl 保证。
 * 报文形状由静态构造函数直接断言——那才是对外契约的所在。
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include <unistd.h>

#ifdef WEAKNET_HAVE_TLS
#include <openssl/evp.h>
#include <openssl/pem.h>
#endif

#include "video_clip_exporter.hpp"

namespace weaknet {
namespace {

/// 生成临时 Ed25519 私钥 PEM，返回路径；失败返回空串（调用方 GTEST_SKIP）。
///
/// 为什么用**真实**密钥而不是打桩签名：这里要验的是「签名这一步能跑通，
/// 于是入队成功」。如果把它打桩，「路径/权限/格式」这几类真问题就永远
/// 测不出来，而它们的表现恰好是「所有静帧静默不上传」。
///
/// 用 mkstemp 而不是固定路径：并行跑测试时固定路径会互相踩。
std::string makeTempEd25519Key() {
#ifdef WEAKNET_HAVE_TLS
    char tmpl[] = "/tmp/gtest_snap_priv_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd < 0) return {};
    const std::string path = tmpl;

    EVP_PKEY* key = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    bool ok = false;
    if (ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &key) > 0) {
        FILE* fp = ::fdopen(fd, "w");
        if (fp) {
            ok = PEM_write_PrivateKey(fp, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
            std::fclose(fp);
        }
    }
    if (ctx) EVP_PKEY_CTX_free(ctx);
    if (key) EVP_PKEY_free(key);
    if (!ok) {
        ::unlink(path.c_str());
        return {};
    }
    return path;
#else
    return {};
#endif
}

/// 构造一份最小可用配置。只填派生 URL 与签名需要的字段；
/// 网络请求的成败在单测里不重要（见文件头「为什么不需要网络」）。
class VideoClipExporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.edge.url.set("http://127.0.0.1:9/api/v1/network/edge/telemetry");
        config_.edge.tenant.set("tenant-test");
        config_.edge.device_id.set("test-device");
        config_.edge.token.set("test-token");
        config_.edge.key_id.set("test-key-id");
        config_.edge.private_key_path.set("");
        config_.edge.interval_ms.store(10000);
        config_.edge.timeout_ms.store(2000);
    }

    weaknet_dbus::WeakNetConfig config_;
};

// ---------------------------------------------------------------------------
// URL 派生：形态不符必须显式失败，绝不猜地址
// ---------------------------------------------------------------------------

TEST_F(VideoClipExporterTest, DerivesClipsUrlFromTelemetrySuffix) {
    EXPECT_EQ(VideoClipExporter::deriveClipsUrl(
                  "http://host:8000/api/v1/network/edge/telemetry"),
              "http://host:8000/api/v1/network/edge/video-clips");
}

TEST_F(VideoClipExporterTest, DerivesChunkUrlWithIndex) {
    EXPECT_EQ(VideoClipExporter::deriveChunkUrl(
                  "http://h/api/v1/network/edge/telemetry", "clip-abc", 3),
              "http://h/api/v1/network/edge/video-clips/clip-abc/chunks/3");
}

TEST_F(VideoClipExporterTest, RejectsTelemetryUrlWithoutExpectedSuffix) {
    // 形态不符时返回空串。猜一个地址意味着把签名证据发到未知端点。
    EXPECT_EQ(VideoClipExporter::deriveClipsUrl("http://h/api/v1/network/edge"), "");
    EXPECT_EQ(VideoClipExporter::deriveClipsUrl("http://h/telemetry"), "");
    EXPECT_EQ(VideoClipExporter::deriveClipsUrl(""), "");
}

TEST_F(VideoClipExporterTest, RejectsClipIdThatWouldRewriteThePath) {
    // clip_id 会被拼进 URL 路径。云端契约已限制字符集，这里再守一次：
    // 一个含 '/' 或 '?' 的 clip_id 能把路径改写成另一个端点。
    const std::string telemetry = "http://h/api/v1/network/edge/telemetry";
    EXPECT_EQ(VideoClipExporter::deriveChunkUrl(telemetry, "a/b", 0), "");
    EXPECT_EQ(VideoClipExporter::deriveChunkUrl(telemetry, "a?b", 0), "");
    EXPECT_EQ(VideoClipExporter::deriveChunkUrl(telemetry, "a b", 0), "");
    EXPECT_EQ(VideoClipExporter::deriveChunkUrl(telemetry, "", 0), "");
}

// ---------------------------------------------------------------------------
// clip_id 生成：必须合规、跨调用不碰撞
// ---------------------------------------------------------------------------

TEST_F(VideoClipExporterTest, MakeClipIdMatchesCloudPattern) {
    const std::string id = VideoClipExporter::makeClipId("radxa-cubie-a7a", 1700000000123ULL);
    ASSERT_FALSE(id.empty());
    EXPECT_LE(id.size(), 128u);
    // 首字符必须是字母或数字（云端正则）
    EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(id.front())) != 0);
    for (char c : id) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                        c == '_' || c == '-';
        EXPECT_TRUE(ok) << "illegal char in clip id: " << c;
    }
    // 时间戳要可读可排序
    EXPECT_NE(id.find("1700000000123"), std::string::npos);
}

TEST_F(VideoClipExporterTest, MakeClipIdSanitizesHostileDeviceId) {
    // device_id 含 '/'、空格、前导符号时，产出的 clip_id 仍须合规。
    const std::string id = VideoClipExporter::makeClipId("  /dev/../x y", 42ULL);
    ASSERT_FALSE(id.empty());
    EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(id.front())) != 0);
    for (char c : id) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                        c == '_' || c == '-';
        EXPECT_TRUE(ok) << "illegal char in clip id: " << c;
    }
}

TEST_F(VideoClipExporterTest, MakeClipIdIsUniqueWithinSameMillisecond) {
    // 同一毫秒内连续生成也不能碰撞——重试队列里可能有多个片段同时准备好。
    std::string a = VideoClipExporter::makeClipId("dev", 1000ULL);
    std::string b = VideoClipExporter::makeClipId("dev", 1000ULL);
    EXPECT_NE(a, b);
}

// ---------------------------------------------------------------------------
// 分片规划：边界必须与云端同源
// ---------------------------------------------------------------------------

TEST_F(VideoClipExporterTest, PlanChunkCountHandlesBoundaries) {
    EXPECT_EQ(VideoClipExporter::planChunkCount(0), 0u);
    EXPECT_EQ(VideoClipExporter::planChunkCount(1), 1u);
    EXPECT_EQ(VideoClipExporter::planChunkCount(VIDEO_CLIP_MAX_CHUNK_BYTES), 1u);
    EXPECT_EQ(VideoClipExporter::planChunkCount(VIDEO_CLIP_MAX_CHUNK_BYTES + 1), 2u);
    EXPECT_EQ(VideoClipExporter::planChunkCount(VIDEO_CLIP_MAX_CHUNK_BYTES * 3), 3u);
    EXPECT_EQ(VideoClipExporter::planChunkCount(VIDEO_CLIP_MAX_CHUNK_BYTES * 3 + 1), 4u);
}

// ---------------------------------------------------------------------------
// base64：必须带 padding，且能被严格解码器接受
// ---------------------------------------------------------------------------

TEST_F(VideoClipExporterTest, Base64EncodesWithPadding) {
    // 长度 % 3 == 1 → 两个 '='；% 3 == 2 → 一个 '='。缺 padding 云端 422。
    const uint8_t one[1] = {'A'};
    EXPECT_EQ(VideoClipExporter::base64Encode(one, 1), "QQ==");

    const uint8_t two[2] = {'A', 'B'};
    EXPECT_EQ(VideoClipExporter::base64Encode(two, 2), "QUI=");

    const uint8_t three[3] = {'A', 'B', 'C'};
    EXPECT_EQ(VideoClipExporter::base64Encode(three, 3), "QUJD");
}

TEST_F(VideoClipExporterTest, Base64EncodesEmptyAsEmpty) {
    EXPECT_EQ(VideoClipExporter::base64Encode(nullptr, 0), "");
}

// ---------------------------------------------------------------------------
// sha256：必须是小写 hex、64 字符
// ---------------------------------------------------------------------------

TEST_F(VideoClipExporterTest, Sha256KnownVector) {
    // "abc" 的 sha256 是标准测试向量；校验它同时确认大小写与长度。
    const std::string input = "abc";
    const std::string digest = VideoClipExporter::sha256Hex(
        reinterpret_cast<const uint8_t*>(input.data()), input.size());
    EXPECT_EQ(digest,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// ---------------------------------------------------------------------------
// 清单报文：字段名与取值是对外契约
// ---------------------------------------------------------------------------

TEST_F(VideoClipExporterTest, BuildManifestBodyEmitsContractFields) {
    VideoClipMeta meta;
    meta.clip_id = "clip-1";
    meta.device_id = "dev-1";
    meta.site_id = "site-1";
    meta.gateway_id = "";
    meta.incident_id = "";
    meta.width = 1640;
    meta.height = 1232;
    meta.fps = 18;
    meta.started_at_ms = 1000;
    meta.ended_at_ms = 2000;

    std::string error;
    const std::string body = VideoClipExporter::buildManifestBody(
        meta, /*total_bytes=*/700000, /*expected_chunks=*/2,
        /*content_sha256_hex=*/std::string(64, 'a'), &error);

    ASSERT_FALSE(body.empty()) << error;
    // 信封标识必须与云端契约逐字一致
    EXPECT_NE(body.find("\"schema_version\":\"network.edge.video-clips.v1\""),
              std::string::npos);
    EXPECT_NE(body.find("\"clip_id\":\"clip-1\""), std::string::npos);
    EXPECT_NE(body.find("\"device_id\":\"dev-1\""), std::string::npos);
    EXPECT_NE(body.find("\"codec\":\"H264\""), std::string::npos);
    EXPECT_NE(body.find("\"width\":1640"), std::string::npos);
    EXPECT_NE(body.find("\"height\":1232"), std::string::npos);
    EXPECT_NE(body.find("\"total_bytes\":700000"), std::string::npos);
    EXPECT_NE(body.find("\"expected_chunks\":2"), std::string::npos);
    EXPECT_NE(body.find("\"started_at_ms\":1000"), std::string::npos);
    EXPECT_NE(body.find("\"ended_at_ms\":2000"), std::string::npos);
}

TEST_F(VideoClipExporterTest, BuildManifestBodyRejectsInvalidMeta) {
    VideoClipMeta meta;  // 全空
    std::string error;
    EXPECT_EQ(VideoClipExporter::buildManifestBody(meta, 0, 0, "", &error), "");
    EXPECT_FALSE(error.empty());
}

TEST_F(VideoClipExporterTest, ChunklessClipCannotCarryBytes) {
    // 云端契约：expected_chunks == 0 时 total_bytes 必须为 0。
    // 本地方先拒绝，免得传完才发现 422。
    VideoClipMeta meta;
    meta.clip_id = "c";
    meta.device_id = "d";
    meta.width = 16;
    meta.height = 16;
    meta.started_at_ms = 0;
    meta.ended_at_ms = 0;

    std::string error;
    EXPECT_EQ(VideoClipExporter::buildManifestBody(meta, /*total_bytes=*/100,
                                                   /*expected_chunks=*/0, "", &error),
              "");
    EXPECT_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// 分片报文：size_bytes / content_sha256 / payload_b64 三者必须自洽
// ---------------------------------------------------------------------------

TEST_F(VideoClipExporterTest, BuildChunkBodyIsSelfConsistent) {
    const std::string raw = "hello video chunk";
    std::string error;
    const std::string body = VideoClipExporter::buildChunkBody(
        "dev-1", "clip-1", 2,
        reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), &error);

    ASSERT_FALSE(body.empty()) << error;
    EXPECT_NE(body.find("\"clip_id\":\"clip-1\""), std::string::npos);
    EXPECT_NE(body.find("\"chunk_index\":2"), std::string::npos);
    EXPECT_NE(body.find("\"size_bytes\":17"), std::string::npos);

    // 声明的 sha256 必须等于实际上传字节的 sha256（云端逐片校验）
    const std::string expected_digest = VideoClipExporter::sha256Hex(
        reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    EXPECT_NE(body.find(expected_digest), std::string::npos);

    // payload 必须以 base64 形态出现
    const std::string expected_b64 = VideoClipExporter::base64Encode(
        reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    EXPECT_NE(body.find(expected_b64), std::string::npos);
}

TEST_F(VideoClipExporterTest, BuildChunkBodyRejectsEmptyIdentity) {
    const uint8_t buf[4] = {1, 2, 3, 4};
    std::string error;
    EXPECT_EQ(VideoClipExporter::buildChunkBody("", "clip", 0, buf, 4, &error), "");
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_EQ(VideoClipExporter::buildChunkBody("dev", "", 0, buf, 4, &error), "");
    EXPECT_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// 黄金样本：整串逐字比对
//
// 上面那些 EXPECT_NE(find(...)) 只能证明「字段在、值对」，证不了另外三件事：
//   1. 字段**顺序**——云端解析器按名字取值，顺序其实无关，但报文是排他的
//      契约面，顺序一变就意味着有人动过序列化代码，值得被看见；
//   2. 字段**不多**——云端 `extra="forbid"`，多一个就直接 422；
//   3. 字段**不少且整体形态没变**——少一个必填字段同样 422。
//
// 这三类偏差在单字段断言下**全部通过**，只有到真机上传才暴露成
// 「云端 422」——而错误码淹在几百个字段里，极难定位。整串比对是唯一能在
// 本地拦住它的形式。
//
// 为什么可以逐字比对：这两个构造函数的输出是**确定性**的（无时间戳、无随机、
// 无浮点），同一输入必得同一字符串。若哪天引入随机或顺序依赖，这个测试会
// 立刻失败——那正是它该做的事。
// ---------------------------------------------------------------------------

TEST_F(VideoClipExporterTest, ManifestBodyMatchesGoldenSample) {
    VideoClipMeta meta;
    meta.clip_id = "clip-1";
    meta.device_id = "dev-1";
    meta.site_id = "site-1";
    meta.gateway_id = "gw-1";
    meta.incident_id = "inc-1";
    meta.width = 1640;
    meta.height = 1232;
    meta.fps = 18;
    meta.started_at_ms = 1000;
    meta.ended_at_ms = 2000;

    std::string error;
    const std::string body = VideoClipExporter::buildManifestBody(
        meta, /*total_bytes=*/700000, /*expected_chunks=*/2,
        /*content_sha256_hex=*/std::string(64, 'a'), &error);
    ASSERT_FALSE(body.empty()) << error;

    // 与云端 VideoClipManifest（extra="forbid"）逐字段对应，顺序固定。
    const std::string expected =
        "{"
        "\"schema_version\":\"network.edge.video-clips.v1\","
        "\"clip_id\":\"clip-1\","
        "\"device_id\":\"dev-1\","
        "\"site_id\":\"site-1\","
        "\"gateway_id\":\"gw-1\","
        "\"incident_id\":\"inc-1\","
        "\"codec\":\"H264\","
        "\"width\":1640,"
        "\"height\":1232,"
        "\"fps\":18,"
        "\"started_at_ms\":1000,"
        "\"ended_at_ms\":2000,"
        "\"total_bytes\":700000,"
        "\"expected_chunks\":2,"
        "\"declared_content_sha256\":\"" + std::string(64, 'a') + "\""
        "}";
    EXPECT_EQ(body, expected);
}

TEST_F(VideoClipExporterTest, ChunkBodyMatchesGoldenSample) {
    const std::string raw = "hello video chunk";
    std::string error;
    const std::string body = VideoClipExporter::buildChunkBody(
        "dev-1", "clip-1", 2,
        reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), &error);
    ASSERT_FALSE(body.empty()) << error;

    // 两个派生量由被测代码自己算出来再拼进样本：本测试守的是**报文结构**，
    // 不是 sha256 或 base64 的算法正确性——那两件事各有独立测试覆盖。
    // 把算法结果硬编码进黄金样本，换一台机器上跑就可能因算法实现差异而失败，
    // 而那不是本测试要发现的问题。
    const std::string digest = VideoClipExporter::sha256Hex(
        reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    const std::string payload = VideoClipExporter::base64Encode(
        reinterpret_cast<const uint8_t*>(raw.data()), raw.size());

    const std::string expected =
        "{"
        "\"schema_version\":\"network.edge.video-clips.v1\","
        "\"device_id\":\"dev-1\","
        "\"clip_id\":\"clip-1\","
        "\"chunk_index\":2,"
        "\"size_bytes\":17,"
        "\"content_sha256\":\"" + digest + "\","
        "\"payload_b64\":\"" + payload + "\""
        "}";
    EXPECT_EQ(body, expected);
}

// ---------------------------------------------------------------------------
// 入队：拒绝必须是可观测的，不能静默
// ---------------------------------------------------------------------------

TEST_F(VideoClipExporterTest, EnqueueRejectsOversizeClip) {
    VideoClipExporter exporter(config_, config_.edge.url.get());

    VideoClipMeta meta;
    meta.clip_id = "c";
    meta.device_id = "d";
    meta.width = 16;
    meta.height = 16;
    meta.started_at_ms = 0;
    meta.ended_at_ms = 0;

    // 超过单片段上限的码流不该进队列：让本地方立刻知道「这段永远不会被接受」，
    // 而不是传了几十 MB 之后在最后一片上拿到 422。
    std::vector<uint8_t> too_big(VIDEO_CLIP_MAX_BYTES + 1, 0);
    EXPECT_FALSE(exporter.enqueue(meta, std::move(too_big)));
    EXPECT_EQ(exporter.stats().clips_dropped, 1u);
    EXPECT_EQ(exporter.queueDepth(), 0u);
}

TEST_F(VideoClipExporterTest, EnqueueRejectsInvalidMeta) {
    VideoClipExporter exporter(config_, config_.edge.url.get());

    VideoClipMeta bad;  // clip_id / device_id 为空，宽高为 0
    std::vector<uint8_t> bytes{1, 2, 3};
    EXPECT_FALSE(exporter.enqueue(bad, std::move(bytes)));
    EXPECT_EQ(exporter.stats().clips_dropped, 1u);
    EXPECT_EQ(exporter.queueDepth(), 0u);
}

TEST_F(VideoClipExporterTest, EnqueueAcceptsValidClip) {
    VideoClipExporter exporter(config_, config_.edge.url.get());

    VideoClipMeta meta;
    meta.clip_id = "c1";
    meta.device_id = "d1";
    meta.width = 1640;
    meta.height = 1232;
    meta.started_at_ms = 10;
    meta.ended_at_ms = 20;

    std::vector<uint8_t> bytes(1024, 0x42);
    EXPECT_TRUE(exporter.enqueue(meta, std::move(bytes)));
    EXPECT_EQ(exporter.queueDepth(), 1u);
    EXPECT_EQ(exporter.stats().clips_dropped, 0u);
}

// ---------------------------------------------------------------------------
// 网络不可达：失败必须不推进进度，且计数正确
// ---------------------------------------------------------------------------

TEST_F(VideoClipExporterTest, RunOnceFailsCleanlyOnUnreachableEndpoint) {
    // 127.0.0.1:9 是 discard 端口，不会有服务监听。重点不是它失败，
    // 而是失败后**队列里那个片段仍在**（不能因为一次失败就把证据丢了），
    // 且失败计数涨了。
    VideoClipExporter exporter(config_, config_.edge.url.get());

    // 生产重试是 8 轮指数退避（累计约 2 分钟）。本用例断言的是失败语义，
    // 不是退避曲线；真等满会让这条用例从毫秒级变成分钟级，还会把
    // 「网络不可达」淹没在退避日志里。这里改成 1 次、零退避。
    VideoClipExporter::RetryPolicy fast;
    fast.max_attempts = 1;
    fast.base_backoff_ms = 0;
    fast.max_backoff_ms = 0;
    exporter.setRetryPolicyForTest(fast);

    VideoClipMeta meta;
    meta.clip_id = "c1";
    meta.device_id = "d1";
    meta.width = 1640;
    meta.height = 1232;
    meta.started_at_ms = 10;
    meta.ended_at_ms = 20;

    std::vector<uint8_t> bytes(2048, 0x11);
    ASSERT_TRUE(exporter.enqueue(meta, std::move(bytes)));

    // 无签名能力或不可达都应是失败，但都不能把片段从队列里抹掉。
    std::string error;
    const bool ok = exporter.runOnce(&error);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());

    // 片段还在队列里（重试语义）或已被计入明确的重试状态。
    // 本实现的选择是：失败后保留在队列头部，直到尝试次数用尽。
    EXPECT_GE(exporter.queueDepth() + exporter.stats().clips_dropped, 1u);
}

// ---------------------------------------------------------------------------
// 静帧上行：与片段共用同一条通路，但契约完全不同
// ---------------------------------------------------------------------------
//
// 这段之所以必须存在：静帧的字段名、`schema_version`、端点路径**任何一处**
// 写错都只会表现为「云端 422 / 404」，而本地日志只有「上传失败」。那类
// 错误在真机联调时才暴露，且极难从几百行日志里定位。
//
// 另一个静默错误是 base64/摘要与**实际发送的字节**不一致：云端先解 base64
// 再比长度、最后比摘要，三者只要有一处对不上就是 422，而本地看起来一切正常。

namespace {

EdgeSnapshotUpload makeShot(uint32_t w = 1640, uint32_t h = 1232,
                           size_t bytes = 128, const std::string& id = "snap-1",
                           const std::string& incident = "inc-1") {
    EdgeSnapshotUpload shot;
    shot.image_id = id;
    shot.incident_id = incident;
    shot.width = w;
    shot.height = h;
    shot.captured_at_ms = 1'795'155'756'003ULL;
    shot.bytes.assign(bytes, 0xFF);
    // 做成一个形状像 JPEG 的载荷（首尾标记在位），这样若哪天有人把字节
    // 换掉，断言能看出「换的不是同一份」。
    if (shot.bytes.size() >= 2) {
        shot.bytes[0] = 0xFF;
        shot.bytes[1] = 0xD8;
        shot.bytes[shot.bytes.size() - 2] = 0xFF;
        shot.bytes[shot.bytes.size() - 1] = 0xD9;
    }
    return shot;
}

}  // namespace

TEST_F(VideoClipExporterTest, DerivesSnapshotsUrlFromTelemetrySuffix) {
    EXPECT_EQ(VideoClipExporter::deriveSnapshotsUrl(
                  "http://127.0.0.1:8000/api/v1/network/edge/telemetry"),
              "http://127.0.0.1:8000/api/v1/network/edge/snapshot-images");
}

TEST_F(VideoClipExporterTest, SnapshotUrlRejectsUnknownSuffixInsteadOfGuessing) {
    // 猜地址等于把带签名的证据发到未知端点，比失败糟得多。
    EXPECT_TRUE(VideoClipExporter::deriveSnapshotsUrl("http://127.0.0.1:8000/x").empty());
    EXPECT_TRUE(VideoClipExporter::deriveSnapshotsUrl("").empty());
    // 只差一个字符也必须拒绝：`/edge/telemetr` 不是 `/edge/telemetry`。
    EXPECT_TRUE(
        VideoClipExporter::deriveSnapshotsUrl("http://h/edge/telemetr").empty());
}

TEST_F(VideoClipExporterTest, BuildSnapshotBodyEmitsEveryContractField) {
    // 云端的 `SnapshotImage` 是 extra="forbid"：多一个字段 422，少一个必填
    // 字段也 422。所以这里逐个字段检查，而不是只看「报文非空」。
    const auto shot = makeShot();
    std::string error;
    const std::string body =
        VideoClipExporter::buildSnapshotBody(shot, "dev-1", "site-1", "gw-1", &error);
    ASSERT_FALSE(body.empty()) << error;

    const std::vector<std::string> required = {
        "\"schema_version\":\"network.edge.snapshot-images.v1\"",
        "\"image_id\":\"snap-1\"",
        "\"device_id\":\"dev-1\"",
        "\"site_id\":\"site-1\"",
        "\"gateway_id\":\"gw-1\"",
        "\"incident_id\":\"inc-1\"",
        "\"codec\":\"JPEG\"",
        "\"width\":1640",
        "\"height\":1232",
        "\"captured_at_ms\":1795155756003",
        "\"size_bytes\":128",
        "\"content_sha256\":",
        "\"payload_b64\":",
    };
    for (const auto& needle : required) {
        EXPECT_NE(body.find(needle), std::string::npos)
            << "缺少契约字段 " << needle << " → 云端 422。报文: " << body << "";
    }

    // 片段契约的字段绝对不能混进来：`fps` 在静帧上没有意义，一旦发出去
    // 云端 422（forbid），而本地只会看到「上传失败」。
    EXPECT_EQ(body.find("\"fps\""), std::string::npos)
        << "静帧不得携带 fps 字段";
    EXPECT_EQ(body.find("\"expected_chunks\""), std::string::npos)
        << "静帧不分片，不得携带 expected_chunks";
}

TEST_F(VideoClipExporterTest, SnapshotBodyDigestMatchesTheBytesItCarries) {
    // 云端按「解码 base64 → 比长度 → 比摘要」校验。三者必须来自同一份字节，
    // 否则任何一处对不上都是 422，而本地看起来完全正常。
    const auto shot = makeShot();
    std::string error;
    const std::string body =
        VideoClipExporter::buildSnapshotBody(shot, "dev-1", "", "", &error);
    ASSERT_FALSE(body.empty()) << error;

    const std::string payload_b64 = VideoClipExporter::base64Encode(
        shot.bytes.data(), shot.bytes.size());
    const std::string digest =
        VideoClipExporter::sha256Hex(shot.bytes.data(), shot.bytes.size());
    ASSERT_EQ(digest.size(), 64u);
    ASSERT_FALSE(payload_b64.empty());

    EXPECT_NE(body.find("\"content_sha256\":\"" + digest + "\""), std::string::npos);
    EXPECT_NE(body.find("\"payload_b64\":\"" + payload_b64 + "\""), std::string::npos);
    EXPECT_NE(body.find("\"size_bytes\":" + std::to_string(shot.bytes.size())),
              std::string::npos);
}

TEST_F(VideoClipExporterTest, SnapshotBodyRejectsInvalidSelfDescription) {
    std::string error;

    auto no_id = makeShot();
    no_id.image_id.clear();
    EXPECT_TRUE(
        VideoClipExporter::buildSnapshotBody(no_id, "dev-1", "", "", &error).empty());

    auto no_bytes = makeShot();
    no_bytes.bytes.clear();
    EXPECT_TRUE(
        VideoClipExporter::buildSnapshotBody(no_bytes, "dev-1", "", "", &error).empty());

    auto no_geometry = makeShot(0, 0);
    EXPECT_TRUE(
        VideoClipExporter::buildSnapshotBody(no_geometry, "dev-1", "", "", &error).empty());

    // device_id 来自 edge.device_id。它为空说明配置不全，此时发出去也会被
    // 云端拒绝（或归到错误的资产上），本地拒绝更早、更清楚。
    auto ok = makeShot();
    EXPECT_TRUE(VideoClipExporter::buildSnapshotBody(ok, "", "", "", &error).empty());
}

TEST_F(VideoClipExporterTest, SnapshotBodyRejectsOversizedImageLocally) {
    // 超限只可能来自质量参数被抬高或编码器异常。本地拒绝能让日志立刻指向
    // 真正的原因，而不是传上去换一个 422。
    auto shot = makeShot();
    shot.bytes.assign(SNAPSHOT_MAX_BYTES + 1, 0xFF);
    std::string error;
    EXPECT_TRUE(
        VideoClipExporter::buildSnapshotBody(shot, "dev-1", "", "", &error).empty());
    EXPECT_FALSE(error.empty());
}

TEST_F(VideoClipExporterTest, SnapshotEnqueueFailsWithoutSigningKey) {
    // 本测试配置里 private_key_path 为空 → 签名必失败。这是**配置问题**，
    // 不该排进队列等重试（重试也不会成功），必须立刻拒绝并计入 dropped。
    VideoClipExporter exporter(config_, config_.edge.url.get());
    EXPECT_FALSE(exporter.enqueueSnapshot(makeShot()));
    EXPECT_EQ(exporter.stats().snapshots_dropped, 1u);
    EXPECT_EQ(exporter.stats().snapshots_enqueued, 0u);
}

TEST_F(VideoClipExporterTest, SnapshotEnqueueRejectsInvalidSelfDescription) {
    VideoClipExporter exporter(config_, config_.edge.url.get());

    auto no_id = makeShot();
    no_id.image_id.clear();
    EXPECT_FALSE(exporter.enqueueSnapshot(no_id));

    auto no_bytes = makeShot();
    no_bytes.bytes.clear();
    EXPECT_FALSE(exporter.enqueueSnapshot(no_bytes));

    EXPECT_EQ(exporter.stats().snapshots_dropped, 2u);
}

TEST_F(VideoClipExporterTest, SnapshotRunOnceOnEmptyQueueReportsWhy) {
    // 空队列返回 false 是对的，但**必须给出原因**：上传循环靠这条消息区分
    // 「没东西可传」与「传失败了」，两者在停机日志里含义完全不同。
    VideoClipExporter exporter(config_, config_.edge.url.get());
    std::string error;
    EXPECT_FALSE(exporter.runSnapshotOnce(&error));
    EXPECT_FALSE(error.empty()) << "空队列也必须给出原因";
    EXPECT_EQ(exporter.stats().snapshots_uploaded, 0u);
    EXPECT_EQ(exporter.stats().snapshots_dropped, 0u)
        << "队列空不是丢弃，不能计入 dropped";
}

TEST_F(VideoClipExporterTest, SnapshotEnqueuesWithARealSigningKey) {
    // 这条是**正向**路径：有了可用的 Ed25519 私钥，入队就必须成功。
    // 少了它，「签名失败」那条用例会掩盖一个更糟的可能——签名对**任何**
    // 输入都失败（比如路径分支写错），那样所有静帧都会静默不上传。
    const std::string key_path = makeTempEd25519Key();
    if (key_path.empty()) {
        GTEST_SKIP() << "无法生成临时 Ed25519 密钥（测试环境缺 OpenSSL）";
    }
    struct Cleanup {
        ~Cleanup() { ::unlink(path.c_str()); }
        std::string path;
    } cleanup{key_path};

    config_.edge.private_key_path.set(key_path);
    VideoClipExporter exporter(config_, config_.edge.url.get());
    ASSERT_TRUE(exporter.enqueueSnapshot(makeShot()));

    EXPECT_EQ(exporter.stats().snapshots_enqueued, 1u);
    EXPECT_EQ(exporter.stats().snapshots_dropped, 0u);
}

TEST_F(VideoClipExporterTest, SnapshotRetryReusesTheSameBodyAndSignature) {
    // 云端按内容判幂等：同 image_id 异内容 = 409，而客户端只会看到
    // 「上传失败」。所以报文与签名必须在**入队时**算好一次，重试时复用。
    //
    // 断言方式是间接的但足够判别：不可达端点 + 1 次尝试后，队列已空且
    // 计数为 1 dropped；若实现是「每次重试重算」，这条仍会过——所以再加
    // 一条更直接的：同一份自述两次构造的报文必须逐字相同。
    auto shot = makeShot();
    std::string e1, e2;
    const std::string b1 =
        VideoClipExporter::buildSnapshotBody(shot, "dev-1", "site-1", "gw-1", &e1);
    const std::string b2 =
        VideoClipExporter::buildSnapshotBody(shot, "dev-1", "site-1", "gw-1", &e2);
    ASSERT_FALSE(b1.empty()) << e1;
    ASSERT_FALSE(b2.empty()) << e2;
    EXPECT_EQ(b1, b2) << "同一份自述必须产生逐字相同的报文，否则重试会被判内容冲突";
}

TEST_F(VideoClipExporterTest, SnapshotQueueDropsOldestWhenFull) {
    // 静帧按**条数**限流（kMaxQueuedSnapshots = 4），不是字节。理由见头文件：
    // 共用字节配额时，一个待传的 22 MB 片段会把配额吃光，于是事故现场那张
    // 唯一的图反而被丢。
    const std::string key_path = makeTempEd25519Key();
    if (key_path.empty()) {
        GTEST_SKIP() << "无法生成临时 Ed25519 密钥（测试环境缺 OpenSSL）";
    }
    struct Cleanup {
        ~Cleanup() { ::unlink(path.c_str()); }
        std::string path;
    } cleanup{key_path};
    config_.edge.private_key_path.set(key_path);

    VideoClipExporter exporter(config_, config_.edge.url.get());
    // 不 start()：只要入队语义，不需要后台线程真的去发。
    for (int i = 0; i < 6; ++i) {
        EXPECT_TRUE(exporter.enqueueSnapshot(
            makeShot(1640, 1232, 128, "snap-" + std::to_string(i))))
            << "第 " << i << " 张";
    }
    // 前两张被挤掉，后四张留下。
    EXPECT_EQ(exporter.stats().snapshots_enqueued, 6u);
    EXPECT_EQ(exporter.stats().snapshots_dropped, 2u)
        << "满了必须是丢最旧，并且计入 dropped（不静默）";
}

TEST_F(VideoClipExporterTest, SnapshotBodyEscapesHostileIdentifiers) {
    // image_id / incident_id 会被拼进 JSON。含引号或反斜杠时不转义就会
    // 产生**合法但语义错误**的报文（或多一个字段 → 云端 422）。这类输入
    // 来自板端自己的编号逻辑，正常不会出现，但一旦出现就是最难查的那种。
    auto shot = makeShot();
    shot.image_id = "snap-\"quote";
    shot.incident_id = "inc\\back";
    std::string error;
    const std::string body = VideoClipExporter::buildSnapshotBody(
        shot, "dev-1", "", "", &error);
    ASSERT_FALSE(body.empty()) << error;
    EXPECT_NE(body.find("\\\"quote"), std::string::npos)
        << "引号必须被转义，否则报文被提前截断: " << body;
    EXPECT_NE(body.find("inc\\\\back"), std::string::npos)
        << "反斜杠必须被转义: " << body;
}

// ---------------------------------------------------------------------------
// 片段录制器：三条「错了不会报错」的规则
// ---------------------------------------------------------------------------
//
// 这三条如果不单独验证，只会以「云端收到的片段播不了 / 时间对不上 / 内存被打满」
// 的形式在真机联调时暴露，而那时已很难判断是录制错了还是传输错了。

namespace {

VideoClipRecorderConfig makeRecorderConfig(uint32_t pre_ms = 1000,
                                           uint32_t post_ms = 1000,
                                           uint64_t max_bytes = 1 << 20) {
    VideoClipRecorderConfig cfg;
    cfg.pre_roll_ms = pre_ms;
    cfg.post_roll_ms = post_ms;
    cfg.max_bytes = max_bytes;
    cfg.device_id = "dev-1";
    cfg.site_id = "site-1";
    cfg.gateway_id = "gw-1";
    cfg.width = 1640;
    cfg.height = 1232;
    return cfg;
}

/// 每帧 10 字节，内容按帧序填充，便于断言「拼出来了哪些帧」。
void pushFrame(VideoClipRecorder* rec, uint8_t tag, bool key, uint64_t mono_ms) {
    std::vector<uint8_t> buf(10, tag);
    rec->push(buf.data(), buf.size(), key, mono_ms);
}

}  // namespace

TEST(VideoClipRecorderTest, ProducesNothingBeforePostRollElapses) {
    VideoClipRecorder rec(makeRecorderConfig());

    pushFrame(&rec, 1, /*key=*/true, 1000);
    ASSERT_TRUE(rec.startClip("inc-1"));

    // 触发点 = 最新帧的 1000。后沿是 2000，未到点不该产出。
    // 若这里就产出，说明后沿判定用了「调用时刻」而不是「触发点 + post_roll」，
    // 症状是片段尾巴永远是空的。
    std::vector<uint8_t> bytes;
    EXPECT_FALSE(rec.poll(1100, 1700000000000ULL, &bytes).has_value());
    EXPECT_TRUE(rec.isRecording());
}

TEST(VideoClipRecorderTest, ClipStartsAtKeyframeAndCoversPreRoll) {
    VideoClipRecorder rec(makeRecorderConfig(/*pre_ms=*/1000, /*post_ms=*/1000));

    // 时间轴：0 帧是 IDR，1..3 是 P 帧，4 又是 IDR，5..6 是 P 帧。
    pushFrame(&rec, 0xA0, /*key=*/true, 0);
    pushFrame(&rec, 0xA1, /*key=*/false, 100);
    pushFrame(&rec, 0xA2, /*key=*/false, 200);
    pushFrame(&rec, 0xA3, /*key=*/false, 300);
    pushFrame(&rec, 0xA4, /*key=*/true, 400);   // ← 触发点之前最后一个 IDR
    pushFrame(&rec, 0xA5, /*key=*/false, 500);
    pushFrame(&rec, 0xA6, /*key=*/false, 600);

    ASSERT_TRUE(rec.startClip("inc-1"));   // 触发点 = 600

    // 到后沿 1600 才产出
    pushFrame(&rec, 0xA7, /*key=*/false, 1600);

    std::vector<uint8_t> bytes;
    auto meta = rec.poll(1600, 1700000000000ULL, &bytes);
    ASSERT_TRUE(meta.has_value());
    ASSERT_FALSE(bytes.empty());

    // 必须从 tag 0xA4 起（最后一个早于触发点的 IDR），不能从 0xA0 起
    // （那会多带一段与事故无关的旧画面），也不能从 0xA5 起（P 帧起头 → 花屏）。
    EXPECT_EQ(bytes.front(), 0xA4);
    EXPECT_EQ(bytes.back(), 0xA7);
    // 4 帧 × 10 字节
    EXPECT_EQ(bytes.size(), 40u);
    // 每帧必须是完整连续的 10 字节同值块
    for (size_t i = 0; i < bytes.size(); i += 10) {
        for (size_t j = 0; j < 10; ++j) EXPECT_EQ(bytes[i + j], bytes[i]);
    }

    EXPECT_EQ(meta->incident_id, "inc-1");
    EXPECT_EQ(meta->device_id, "dev-1");
    EXPECT_EQ(meta->site_id, "site-1");
    EXPECT_EQ(meta->gateway_id, "gw-1");
    EXPECT_EQ(meta->width, 1640u);
    EXPECT_EQ(meta->height, 1232u);
    EXPECT_TRUE(meta->valid());
    EXPECT_LE(meta->started_at_ms, meta->ended_at_ms);

    // 帧数与帧率必须是**实测值**，不能沿用配置里的名义 25。
    //
    // 片段覆盖 tag 0xA4..0xA7 共 4 帧，单调跨度为 1600-400 = 1200 ms，
    // 于是真实帧率是 4 / 1.2 ≈ 3.33 fps，四舍五入为 3。
    //
    // 若这里读到 25，说明帧率又退回了「配置里写的数字」，而那会让封装出来的
    // MP4 时间轴按 25/3.33 ≈ 7.5 倍拉长——一段 1.2 秒的现场会被标成 9 秒，
    // 「事发那一刻在第几秒」这个留证唯一要回答的问题就答错了，且不报任何错。
    EXPECT_EQ(meta->frame_count, 4u);
    EXPECT_EQ(meta->fps, 3u);
    EXPECT_FALSE(rec.isRecording());
    EXPECT_EQ(rec.clipsProduced(), 1u);
}

TEST(VideoClipRecorderTest, AbandonsClipWhenNoKeyframeInWindow) {
    // 回溯窗口里全是 P 帧（GOP 比窗口长）。此时**不能**从最旧帧开始拼——
    // 那会产出一个不可解码的片段，而它是"已完成"状态，运维只会以为留证坏了。
    VideoClipRecorder rec(makeRecorderConfig(/*pre_ms=*/1000, /*post_ms=*/1000));

    pushFrame(&rec, 0xB0, /*key=*/false, 0);
    pushFrame(&rec, 0xB1, /*key=*/false, 100);
    pushFrame(&rec, 0xB2, /*key=*/false, 200);
    pushFrame(&rec, 0xB3, /*key=*/false, 300);

    ASSERT_TRUE(rec.startClip("inc-2"));   // 触发点 = 300
    pushFrame(&rec, 0xB4, /*key=*/false, 1400);

    std::vector<uint8_t> bytes;
    EXPECT_FALSE(rec.poll(1400, 1700000000000ULL, &bytes).has_value());
    EXPECT_TRUE(bytes.empty());
    EXPECT_EQ(rec.clipsProduced(), 0u);
    EXPECT_EQ(rec.clipsAborted(), 1u);
    EXPECT_FALSE(rec.isRecording());
}

TEST(VideoClipRecorderTest, SecondTriggerWhileRecordingIsIgnored) {
    // 一次事故只产出一个片段。若允许重复触发各自开一段，云端会收到
    // 若干互相重叠的碎片，且 clip_id 各不相同——看起来像多起事故。
    VideoClipRecorder rec(makeRecorderConfig());
    pushFrame(&rec, 1, /*key=*/true, 0);

    ASSERT_TRUE(rec.startClip("inc-a"));
    EXPECT_FALSE(rec.startClip("inc-b"));
    EXPECT_EQ(rec.currentIncident(), "inc-a");
}

TEST(VideoClipRecorderTest, PreRollIsTrimmedSoBufferStaysBounded) {
    // 未触发时缓冲只保留回溯窗口。若这里不裁剪，长期运行会把内存吃满，
    // 而症状是「跑几天后 OOM」，与视频功能看起来毫无关系。
    VideoClipRecorder rec(makeRecorderConfig(/*pre_ms=*/1000, /*post_ms=*/1000));

    for (uint64_t t = 0; t <= 10000; t += 100) {
        pushFrame(&rec, 1, /*key=*/true, t);
    }
    // 10 字节/帧，1s 窗口 = 11 帧（含两端）
    EXPECT_LE(rec.bufferedFrames(), 12u);
}

TEST(VideoClipRecorderTest, OversizeBufferAbortsInsteadOfTrimmingTheKeyframe) {
    // 上限必须导致整段放弃，而不是继续裁最旧——裁掉的若正好是起始 IDR，
    // 片段就不可解码了。
    VideoClipRecorderConfig cfg = makeRecorderConfig(/*pre_ms=*/100000, /*post_ms=*/1000,
                                                    /*max_bytes=*/100);
    VideoClipRecorder rec(cfg);

    pushFrame(&rec, 1, /*key=*/true, 0);
    ASSERT_TRUE(rec.startClip("inc-3"));
    // 每帧 10 字节 → 第 11 帧起累计超过 100 字节上限。
    // 回溯窗口设成 100s，所以裁剪不会先替我们兜住——超限必须由上限逻辑处置。
    for (uint64_t t = 100; t <= 1500; t += 100) {
        pushFrame(&rec, 1, /*key=*/false, t);
    }

    EXPECT_FALSE(rec.isRecording());
    std::vector<uint8_t> bytes;
    EXPECT_FALSE(rec.poll(99999, 1700000000000ULL, &bytes).has_value());
    EXPECT_TRUE(bytes.empty());
}

}  // namespace
}  // namespace weaknet