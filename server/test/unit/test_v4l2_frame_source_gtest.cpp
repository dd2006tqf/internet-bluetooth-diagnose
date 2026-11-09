/**
 * @file test_v4l2_frame_source_gtest.cpp
 * @brief V4L2 真机帧源的契约测试（无摄像头环境可跑）
 *
 * 真机取帧路径只能在板上验证，但**失败路径与接口纪律可以在 CI 上验证**，
 * 而这恰恰是最容易出错的部分。这份测试守四条：
 *
 *   1. 构造不得碰设备 —— 否则 CI 上连对象都建不出来
 *   2. start() 失败必须是「能力不可用」而非崩溃，且原因可读
 *   3. 未启动时 snapshotAndReset() 必须返回 valid()==false —— 缺数据不得
 *      伪装成「零丢帧」，这是本项目最反复强调的一条纪律
 *   4. describe() 必须带 `v4l2:` 前缀 —— 真机证据与合成证据不得混淆
 *
 * 刻意**不测**的：真实出帧、格式协商、时间戳来源。那需要摄像头在位，
 * 属于阶段 3 的真机验证，写在这里只会变成永远 skip 的空壳。
 */

#include <gtest/gtest.h>

#include <string>

#include "v4l2_frame_source.hpp"

using namespace weaknet;

// ---------------------------------------------------------------------------
// 1. 构造与生命周期：不碰设备
// ---------------------------------------------------------------------------

TEST(V4l2FrameSource, ConstructionDoesNotTouchDevice) {
    // 构造函数不 open /dev/video*，所以就算路径完全不存在也能建对象。
    // 这条是 CI 可测性的前提：否则任何包含本类的测试在没有摄像头的机器上
    // 都会在构造阶段就失败。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/definitely-not-a-video-device-xyz";

    EXPECT_NO_THROW({
        V4l2FrameSource src(cfg);
        EXPECT_FALSE(src.isRunning()) << "刚构造完不应处于运行态";
    });
}

TEST(V4l2FrameSource, StartFailsGracefullyOnMissingDevice) {
    // 「摄像头没插」在本项目里是**正常状态**，不是错误。
    // start() 返回 false，上层报 unavailable，业务链路照常跑。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/definitely-not-a-video-device-xyz";

    V4l2FrameSource src(cfg);

    EXPECT_FALSE(src.start()) << "设备不存在时 start 必须返回 false";
    EXPECT_FALSE(src.isRunning());
    EXPECT_FALSE(src.lastError().empty())
        << "失败必须留下可读原因，否则现场无法判断是权限还是没插";
    // 原因里应能看到路径，便于运维直接定位
    EXPECT_NE(src.lastError().find(cfg.device), std::string::npos);
}

TEST(V4l2FrameSource, StopIsIdempotent) {
    // 接口契约要求可重入。未 start 时 stop 不得崩溃，也不得改动状态。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/definitely-not-a-video-device-xyz";

    V4l2FrameSource src(cfg);

    EXPECT_NO_THROW(src.stop());
    EXPECT_NO_THROW(src.stop()) << "重复 stop 不得崩溃";
    EXPECT_FALSE(src.isRunning());
}

// ---------------------------------------------------------------------------
// 2. 缺数据不得伪装成健康（最核心的一条）
// ---------------------------------------------------------------------------

TEST(V4l2FrameSource, SnapshotBeforeStartIsInvalidNotHealthy) {
    // FrameStats 默认值全零，而 frameLossRatio() 对 expected==0 返回 0.0。
    // 「从未采样」与「完美运行」在数值上完全一样，只有 valid() 能区分。
    //
    // 若真机源在这里返回一个"看起来正常"的统计，未接摄像头的板子就会
    // 报「画面与网络一致正常」——这正是方案 §6.2.4 点名要防的失效。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/definitely-not-a-video-device-xyz";

    V4l2FrameSource src(cfg);
    auto s = src.snapshotAndReset();

    EXPECT_FALSE(s.valid()) << "未启动就返回有效统计 = 伪造观测数据";
    EXPECT_EQ(s.frames_expected, 0u);
    EXPECT_EQ(s.frames_received, 0u);
    EXPECT_DOUBLE_EQ(s.frameLossRatio(), 0.0);
}

TEST(V4l2FrameSource, SnapshotAfterFailedStartStaysInvalid) {
    // start() 失败后即便反复取快照，也必须一直是 invalid。
    // 否则「启动失败但偶尔能取到数」会让上游误判能力已就绪。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/definitely-not-a-video-device-xyz";

    V4l2FrameSource src(cfg);
    ASSERT_FALSE(src.start());

    for (int i = 0; i < 3; ++i) {
        auto s = src.snapshotAndReset();
        EXPECT_FALSE(s.valid()) << "第 " << (i + 1) << " 次快照仍应为 invalid";
    }
}

TEST(V4l2FrameSource, SnapshotAfterStopIsInvalid) {
    // stop() 之后回到「不可用」。这防止上层在停流后仍读到上一窗口的残留值。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/definitely-not-a-video-device-xyz";

    V4l2FrameSource src(cfg);
    src.stop();

    auto s = src.snapshotAndReset();
    EXPECT_FALSE(s.valid());
}

// ---------------------------------------------------------------------------
// 3. 来源标识：真机证据不得与合成证据混淆
// ---------------------------------------------------------------------------

TEST(V4l2FrameSource, DescribeCarriesV4l2Prefix) {
    // synthetic_frame_source.cpp 用 `synthetic:` 前缀，真机源用 `v4l2:`。
    // 这个前缀是 evidence 纪律的一部分：事后必须能区分「真的观测到了」
    // 与「注入了测试数据」。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/video-test-node";
    cfg.width = 1280;
    cfg.height = 720;
    cfg.fps = 30;

    V4l2FrameSource src(cfg);
    const std::string d = src.describe();

    EXPECT_EQ(d.rfind("v4l2:", 0), 0u) << "真机源必须以 v4l2: 开头";
    EXPECT_NE(d.find(cfg.device), std::string::npos) << "必须写明实际设备路径";
    EXPECT_NE(d.find("1280x720"), std::string::npos) << "必须写明分辨率";
    EXPECT_EQ(d.find("synthetic:"), std::string::npos)
        << "真机源不得出现 synthetic 标识";
}

TEST(V4l2FrameSource, DescribeReflectsRunningState) {
    // 未运行时带 (stopped) 标记，避免把「配置好了」误读成「正在采」。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/video-test-node";

    V4l2FrameSource src(cfg);
    EXPECT_NE(src.describe().find("(stopped)"), std::string::npos);

    // 真的启动成功后不应再有 stopped 标记 —— 但 CI 无摄像头，无法验证。
    // 这里只钉死"未运行时必须有标记"这一半契约。
}

// ---------------------------------------------------------------------------
// 4. 配置默认值：与方案 §6.2.2.2 的取舍表一致
// ---------------------------------------------------------------------------

TEST(V4l2FrameSource, DefaultConfigMatchesPlan) {
    // 这些默认值不是随手填的，每一条都有方案里的依据（零拷贝 / vb2 分配上限 /
    // 720p 而非满分辨率以避免 ISP 成为归因噪声）。钉死它们，防止有人
    // "顺手"改成 sensor 满分辨率而让归因失真。
    V4l2CaptureConfig cfg;

    EXPECT_EQ(cfg.device, "/dev/video0");
    EXPECT_EQ(cfg.width, 1280u) << "默认 720p 而非 3280x2464，避免 ISP 成为瓶颈";
    EXPECT_EQ(cfg.height, 720u);
    EXPECT_EQ(cfg.fps, 30u);
    EXPECT_EQ(cfg.buffer_count, 4u) << "方案 §6.2.2.2：4 个 buffer";
    EXPECT_GT(cfg.select_timeout_ms, 0u) << "必须有超时，否则卡死时无法返回";
    EXPECT_GT(cfg.window_ms, 0u) << "窗口时长决定能否与网络侧对齐";
}

TEST(V4l2FrameSource, ZeroBufferCountIsClamped) {
    // 配置成 0 时实现内部会回退到 4。这里验证不会因此崩溃或死循环。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/definitely-not-a-video-device-xyz";
    cfg.buffer_count = 0;

    V4l2FrameSource src(cfg);
    EXPECT_FALSE(src.start()) << "设备不存在时仍应失败，而不是因 buffer_count=0 崩溃";
}
