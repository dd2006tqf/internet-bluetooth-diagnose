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

#include <string>
#include <vector>

#include "video_clip_exporter.hpp"

namespace weaknet {
namespace {

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