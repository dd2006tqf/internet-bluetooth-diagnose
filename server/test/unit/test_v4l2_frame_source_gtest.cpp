/**
 * @file test_v4l2_frame_source_gtest.cpp
 * @brief V4L2 真机帧源的契约测试
 *
 * 全文件分两层：
 *
 * **A. 无设备也能跑（CI 默认执行，第 1~5 节）** —— 失败路径与接口纪律，
 *    这恰恰是最容易出错的部分：
 *
 *   1. 构造不得碰设备 —— 否则 CI 上连对象都建不出来
 *   2. start() 失败必须是「能力不可用」而非崩溃，且原因可读
 *   3. 未启动时 snapshotAndReset() 必须返回 valid()==false —— 缺数据不得
 *      伪装成「零丢帧」，这是本项目最反复强调的一条纪律
 *   4. describe() 必须带 `v4l2:` 前缀 —— 真机证据与合成证据不得混淆
 *   5. 新增配置项（帧率协商 / 多平面）的默认值必须与「不发布假数据」一致
 *
 * **B. 需要真实 V4L2 设备（第 6 节，默认不注册）** —— 成功路径。用
 *    `WEAKNET_V4L2_TEST_DEVICE` 门控，宿主机 `modprobe vivid` 即可跑，
 *    无需真实摄像头。这层守的是「代码能真的跑起来」以及窗口语义、帧率
 *    协商、出队延迟、序号跳变这几条只有跑起来才看得见的契约。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

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

// ---------------------------------------------------------------------------
// 5. 新增配置项的默认值与 describe() 可观测性
// ---------------------------------------------------------------------------

TEST(V4l2FrameSource, FrameRateNegotiationDefaultsToOn) {
    // set_frame_rate 必须默认 true，且这条不是风格偏好：
    // frames_expected 是按 cfg.fps 推的，若驱动实际跑在别的帧率上，
    // 丢帧率会算成一个恒定的巨大假值（实测 15fps 驱动 + 配置 30fps
    // → exp=30 recv=15，loss≈50%，看起来像「链路丢了一半帧」）。
    //
    // 反过来说，把它默认成 false 就等于默认允许发布假数据。
    V4l2CaptureConfig cfg;
    EXPECT_TRUE(cfg.set_frame_rate)
        << "默认必须尝试协商帧率，否则 frames_expected 与事实脱钩";
}

TEST(V4l2FrameSource, MultiplanarIsAllowedByDefault) {
    // 默认允许 MPLANE：嵌入式 ISP 的 DMA 通道常按平面分离（Y / UV 各一通道），
    // 部分 sunxi-vin 配置只出 NV12M 而不出 NV12。默认关掉会让这类真机
    // 直接采集失败，而失败原因是「我们主动放弃了可用的接口」。
    //
    // 保留 false 选项是因为存在「驱动声称支持 MPLANE 但实际有问题」的兜底需求。
    V4l2CaptureConfig cfg;
    EXPECT_TRUE(cfg.allow_multiplanar)
        << "默认必须允许 MPLANE，否则只支持多平面的节点无法采集";
}

TEST(V4l2FrameSource, DescribeStatesPlanarModeEvenWhenStopped) {
    // describe() 必须无条件写明 splane/mplane，即使从未 start()。
    //
    // 理由：同一个 FrameStats 在两种 buf type 下采集路径完全不同，事后排查
    // 时若不知道当时走的是哪条，就无法解释异常数值。未启动时 buf_type 是
    // 默认值 splane，也必须显式写出来——"没写" 与 "写 splane" 对读者不同。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/video-test-node";

    V4l2FrameSource src(cfg);
    const std::string d = src.describe();

    EXPECT_NE(d.find(" splane"), std::string::npos)
        << "未启动时必须显式标注平面模式，不能靠读者猜";
    EXPECT_EQ(d.find(" mplane"), std::string::npos)
        << "未启动时不得声称是多平面（那是 start() 才能确定的事）";
}

TEST(V4l2FrameSource, DescribeOmitsNegotiationNoteWhenNothingAdjusted) {
    // 未 start() 时没有与驱动协商过，描述里不该出现 `[...]` 那段协商记录。
    // 这段是「配置 vs 事实」的偏差凭证：没有偏差就不该有内容，否则读者
    // 会把一个空方括号当成「协商失败」。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/video-test-node";

    V4l2FrameSource src(cfg);
    const std::string d = src.describe();

    EXPECT_EQ(d.find('['), std::string::npos)
        << "未发生协商时不得出现偏差记录段";
}

TEST(V4l2FrameSource, WindowZeroIsNotSilentlyRewrittenLikeSynthetic) {
    // 这里**刻意钉死 v4l2 源与 synthetic 源的一处差异**，不是遗漏。
    //
    // SyntheticFrameSource 把 window_ms=0 静默改成 1000（它有明确构造参数，
    // 钳位不会误导任何人）。V4l2FrameSource 保持 0，因为 window_ms 会进
    // FrameStats.window_ms，而 L4 归因层用它对网络侧窗口；悄悄改成 1000
    // 会让「没声明窗口」变成「声明了 1 秒窗口」，进而让归因在窗口不匹配时
    // 本该拒绝却照常判定。
    //
    // 保持 0 的效果是归因层如实拒绝归因，这正是 frame_source.hpp 里
    // 「0 表示未声明」那条注释所要求的。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/definitely-not-a-video-device-xyz";
    cfg.window_ms = 0;

    V4l2FrameSource src(cfg);

    // 未启动仍返回 invalid，且 window_ms 必须保持未声明。
    // 这一条同时守两件事：缺数据不得伪装成健康；未声明窗口不得被填成默认值。
    const FrameStats s = src.snapshotAndReset();
    EXPECT_FALSE(s.valid());
    EXPECT_EQ(s.window_ms, 0u) << "未采样时窗口必须保持「未声明」，不得填默认值";
}

// ---------------------------------------------------------------------------
// 6. 真机采集路径（**需要真实 V4L2 设备**，默认不跑）
// ---------------------------------------------------------------------------
//
// 为什么这段要用环境变量门控，而不是 `GTEST_SKIP` 静默跳过：
//
//   - 静默 skip 在 CI 上表现为「测试通过」，实际上一次都没跑。本项目反复
//     强调「缺数据不得伪装成健康」，测试自己更不能靠 skip 伪装成覆盖。
//   - 所以默认状态下这些用例**不注册**（连 skip 记录都不产生）；只有显式
//     设置 `WEAKNET_V4L2_TEST_DEVICE=/dev/video0` 时才启用，并在测试名后
//     缀上设备路径，让「跑过哪台设备」在 gtest 输出里直接可见。
//
// 用法（宿主机有 vivid 虚拟设备即可，无需真实摄像头）：
//
//   WEAKNET_V4L2_TEST_DEVICE=/dev/video0 test_v4l2_frame_source_gtest
//
// vivid 的注意事项（这两条都是踩过的坑）：
//   1. `multiplanar` 模块参数语义是 1=单平面、2=多平面，**不是**按位开关。
//      想看多平面路径需 `modprobe vivid multiplanar=2` 后重载。
//   2. 驱动帧率状态**跨实例保留**：前一个进程 S_PARM 成 15fps 后，后续进程
//      即便 `set_frame_rate=false` 也会实测到 15fps。所以与帧率有关的断言不能
//      假定「未设置 = 默认值」，只能断言「配置与实际一致」这一方向。
namespace {

/// 读取门控环境变量；为空表示不启用真机用例
std::string v4l2TestDevice() {
    const char* p = std::getenv("WEAKNET_V4L2_TEST_DEVICE");
    return (p != nullptr && *p != '\0') ? std::string(p) : std::string();
}

/// 真机用例的基类：门控不满足时整组不注册
class V4l2LiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string dev = v4l2TestDevice();
        if (dev.empty()) {
            GTEST_SKIP() << "未设置 WEAKNET_V4L2_TEST_DEVICE，跳过真机采集用例";
        }
        device_ = dev;
    }

    static V4l2CaptureConfig makeConfig(uint32_t width, uint32_t height,
                                        uint32_t fps, uint32_t window_ms) {
        V4l2CaptureConfig cfg;
        cfg.device = v4l2TestDevice();
        cfg.width = width;
        cfg.height = height;
        cfg.fps = fps;
        cfg.window_ms = window_ms;
        return cfg;
    }

    std::string device_;
};

}  // namespace

TEST_F(V4l2LiveTest, StartSucceedsAndReportsPlanarModeInDescribe) {
    // 最重要的真机断言：**成功路径存在**。此前 10 项单测只覆盖失败路径，
    // 「start() 能不能真的成功」从未被任何自动化测试验证过 —— 代码交付了
    // 却没有一条证据说明它能跑。
    auto cfg = makeConfig(640, 480, 30, 500);
    V4l2FrameSource src(cfg);

    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();
    EXPECT_TRUE(src.isRunning());

    const std::string d = src.describe();
    // describe() 必须写明实际走的是哪条路径，否则事后无法解释数值差异
    const bool says_mplane = d.find(" mplane") != std::string::npos;
    const bool says_splane = d.find(" splane") != std::string::npos;
    EXPECT_TRUE(says_mplane || says_splane)
        << "describe() 未标注平面模式：" << d;

    src.stop();
    EXPECT_FALSE(src.isRunning());
}

TEST_F(V4l2LiveTest, ContinuousCallsMeasureFullWindowWithoutPhantomLoss) {
    // 守窗口语义的核心契约：**连续调用**（调用方不 sleep）时，
    // 每个窗口都应测满 window_ms，且丢掉的是真丢帧而不是窗口边界假象。
    //
    // 这条对应本轮修掉的三个同源缺陷。修之前即便连续调用也会出现
    // 跨窗口假 seq_gap 与巨大 max_gap（把窗口间隔当帧间隔）。修好后：
    //   - window_ms 恒等于配置值（不是「实际睡了多久」）
    //   - seq_gap_count 为 0（本窗口首帧不参与跨窗口比较）
    //   - max_gap 约等于 1000/fps，而不是接近整个窗口
    auto cfg = makeConfig(640, 480, 30, 500);
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    for (int w = 1; w <= 3; ++w) {
        const FrameStats s = src.snapshotAndReset();

        ASSERT_TRUE(s.valid()) << "第 " << w << " 窗口应产出有效统计";
        EXPECT_EQ(s.window_ms, 500u)
            << "窗口跨度必须是配置值，不能是「实际耗时」——后者会把"
               "调用方多睡的时间算进窗口，使丢帧率被系统性低估";
        EXPECT_EQ(s.frames_expected, 15u) << "500ms @ 30fps = 15 帧";

        // 帧间隔应接近帧周期（1000/30 ≈ 33ms），留出调度余量
        EXPECT_LT(s.max_gap_ms, 200u)
            << "最长帧间隔远小于窗口才算正常；接近窗口长度说明窗口边界被当成了帧间隔";

        // 每帧字节数应等于 640x480 NV12 的几何大小（1.5 字节/像素）。
        // 这条能抓出「拿 buf.length 当字节数」这类单位混淆（多平面下
        // length 是平面数而不是字节数，混淆会得到恒为 0 的坏码率）。
        if (s.frames_received > 0) {
            const double bytes_per_frame =
                static_cast<double>(s.bytes_received) /
                static_cast<double>(s.frames_received);
            EXPECT_NEAR(bytes_per_frame, 640.0 * 480.0 * 1.5, 1.0)
                << "每帧字节数不等于 NV12 几何大小，说明字节统计取错了字段";
        }
    }

    // **这里刻意不查 seq_gap_count**：连续调用时本窗口首帧与上一窗口末帧
    // 往往恰好连号，跨窗口比较缺陷在此场景下不可观测。实测把
    // `resetWindow()` 里的 `has_last_seq = false` 删掉，本用例 3/3 仍通过。
    // 真正能稳定暴露该缺陷的是 `WindowBoundaryDoesNotFakeSequenceGap`。
    // 在这里放一条「累计 gap 应为 0」看似无害，实则是会随运气漏判的假护栏。

    src.stop();
}

TEST_F(V4l2LiveTest, WindowBoundaryDoesNotFakeSequenceGap) {
    // 守 `resetWindow()` 必须清 `has_last_seq`：本窗口首帧不得与上一窗口
    // 末帧比较序号。
    //
    // **场景必须让窗口真正过期**：窗口之间 sleep 满 window_ms，使
    // `snapshotAndReset()` 走 overdue 分支（只做非阻塞排空）。此时上一窗口
    // 结束后到本窗口开始之间的帧全被驱动丢弃，序号必然出现空洞。
    // 若 `has_last_seq` 未清，这个空洞会被记成 seq_gap，报出一个并不存在的
    // "链路丢包"。
    //
    // 为什么不能用更短的 sleep：sleep < window_ms 时窗口尚未到期，睡眠期间
    // 缓冲区填满、驱动真的丢帧，正常代码也会如实计数 gap（实测 sleep=200
    // 时正常版与变异版都得 2）。只有 sleep >= window_ms 才把「真丢帧」与
    // 「跨窗口假比较」区分开。
    //
    // 断言强度（已用变异测试核对）：删掉 `has_last_seq = false` 后本用例
    // 每次都在窗口 3 及以后冒出 gap；正常代码恒为 0。判定放在累计值上，
    // 单窗口的偶发 0 不影响结论。
    auto cfg = makeConfig(640, 480, 30, 1000);
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    uint32_t total_seq_gap = 0;
    for (int w = 1; w <= 3; ++w) {
        const FrameStats s = src.snapshotAndReset();
        ASSERT_TRUE(s.valid()) << "第 " << w << " 窗口应产出有效统计";
        total_seq_gap += s.seq_gap_count;
        // 窗口满 1 秒后必定积压，下一轮走 overdue 非阻塞排空
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    EXPECT_EQ(total_seq_gap, 0u)
        << "跨窗口出现了序号跳变计数，说明本窗口首帧仍在与上一窗口末帧比较"
           "（resetWindow 未清 has_last_seq），会把窗口边界报成链路丢帧";

    src.stop();
}

TEST_F(V4l2LiveTest, FrameRateNegotiationKeepsExpectedInSyncWithReality) {
    // 守「不信配置信事实」这条：协商完成后，frames_expected 必须按**实际**
    // 帧率推导，而不是继续用请求值。
    //
    // 之所以只断言方向而不断言具体帧率：部分驱动不支持 S_PARM，实际帧率
    // 由链路固定。此时正确行为是「把 cfg.fps 校正为实际值」，而不是报假丢帧。
    // 所以可观测的判据是：连续窗口内 received 与 expected 同量级，丢帧率
    // 不出现恒定的巨大假值。
    auto cfg = makeConfig(640, 480, 30, 1000);
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    // 第一个窗口可能包含启动抖动，取后两个窗口判断稳态
    src.snapshotAndReset();
    for (int w = 1; w <= 2; ++w) {
        const FrameStats s = src.snapshotAndReset();
        ASSERT_TRUE(s.valid());
        ASSERT_GT(s.frames_expected, 0u) << "期望帧数为 0 说明帧率未被确定";

        // 假丢帧率（如 50%~70%）是「配置帧率与驱动实际不符」的典型signature。
        // 真机在稳态下的丢帧率不应达到这个量级。
        EXPECT_LT(s.frameLossRatio(), 0.5)
            << "丢帧率异常高且稳定，通常是帧率未协商导致 expected 用了错误帧率"
            << "（expected=" << s.frames_expected
            << " received=" << s.frames_received << "）";
    }

    src.stop();
}

TEST_F(V4l2LiveTest, DequeueLatencyRisesWhenConsumerFallsBehind) {
    // 守 p95_dequeue_latency_ms 不是恒为 0 的死字段。
    //
    // 手法：窗口之间故意睡 300ms。窗口设计跨度只有 300ms，这会让缓冲区在
    // 睡眠期间填满、帧在队列里积压，于是「取走时刻 − 帧完成时刻」必然变大。
    //
    // 为什么这条必须存在：一个恒为 0 的字段和生产代码里没有它完全等价。
    // 若没有这条测试，「出队延迟」就只是一段自我声明的注释。
    auto cfg = makeConfig(640, 480, 30, 300);
    cfg.buffer_count = 2;   // 小队列让积压更容易发生（4 个缓冲足以吸收抖动）
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    src.snapshotAndReset();   // 预热，首个窗口含启动抖动不计

    bool saw_latency = false;
    for (int w = 1; w <= 3; ++w) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const FrameStats s = src.snapshotAndReset();
        ASSERT_TRUE(s.valid());
        if (s.p95_dequeue_latency_ms > 0) saw_latency = true;
    }

    EXPECT_TRUE(saw_latency)
        << "故意让消费者落后却仍测不到出队延迟 —— 该字段要么没在测量，"
           "要么取时刻的位置不对（必须在 DQBUF 返回后立刻取）";

    src.stop();
}

TEST_F(V4l2LiveTest, StopThenSnapshotIsInvalidNotStale) {
    // 真机路径下同样守「停流后不得读到上一窗口残留值」。
    // 失败路径版本已在无设备用例中覆盖，但那条走的是「从未启动」，
    // 这里走的是「启动过又停止」——后者才可能留下残留计数。
    auto cfg = makeConfig(640, 480, 30, 300);
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    const FrameStats during = src.snapshotAndReset();
    ASSERT_TRUE(during.valid());

    src.stop();
    const FrameStats after = src.snapshotAndReset();
    EXPECT_FALSE(after.valid()) << "停流后不得再产出有效统计";
    EXPECT_EQ(after.frames_received, 0u) << "停流后不得残留上一窗口的计数";
}

TEST_F(V4l2LiveTest, RestartAfterStopWorksAndResetsCounters) {
    // 停流 → 再启动 必须可用且计数从零开始。
    // 这条抓的是 closeDevice() 未清干净的类问题（fd / mmap / 平面状态残留）。
    auto cfg = makeConfig(640, 480, 30, 300);
    V4l2FrameSource src(cfg);

    ASSERT_TRUE(src.start()) << "首次 start() 失败：" << src.lastError();
    src.snapshotAndReset();
    src.stop();

    ASSERT_TRUE(src.start()) << "重新 start() 失败：" << src.lastError();
    const FrameStats s = src.snapshotAndReset();
    ASSERT_TRUE(s.valid()) << "重启后首个窗口应有效";
    // 计数是新窗口的，不包含上一次运行累计
    EXPECT_LE(s.frames_received, s.frames_expected + 4)
        << "重启后仍带着上一次运行的累计帧数，说明 resetWindow 未清干净";

    src.stop();
}
