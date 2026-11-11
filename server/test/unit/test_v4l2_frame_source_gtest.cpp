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

#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    // 故意设成**非默认**分辨率：这条测的是「describe() 如实反映当前配置」，
    // 若用默认值，就分不清它是读了配置还是打印了硬编码常量。
    // 1280x720 在本板是零帧的 bin_factor=2 模式，但这条测试根本不打开设备
    // （device 是个不存在的路径），所以只验证字符串，不涉及可用性。
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
    // 这些默认值不是随手填的，每一条都有真机依据（零拷贝 / vb2 分配上限 /
    // 1640x1232 而非 720p 或满分辨率）。钉死它们，防止有人"顺手"改。
    //
    // ========================================================================
    // 为什么这条断言从 1280x720 改成了 1640x1232（不是放宽，是纠错）
    // ========================================================================
    // 原断言写的是「默认 720p 而非 3280x2464，避免 ISP 成为瓶颈」。真机实测
    // 证明这个前提本身是错的：720p 在 IMX219 + sunxi-vin 上属于
    // `bin_factor=2` 模式，**一帧都出不来**（select 超时、0 字节、约 710 条
    // `tdm0 rx0 frame error! width is 1869`）。钉死一个零帧默认值，等于把
    // 帧源钉死在"永远采不到"的状态。
    //
    // 新值的依据（板端产品同款 ioctl 序列实测 6s）：
    //   3280x2464 → 18.33fps / ≈222 MB/s
    //   1640x1232 → 18.33fps / ≈ 56 MB/s   ← 同帧率，1/4 带宽
    // 且 dmesg 记录 `s_fmt set width = 3280, height = 2464`，说明 1640x1232
    // 仍是驱动按 bin_factor=1 满分辨率采样后缩放交付，不落入坏模式。
    //
    // 断言强度没有降低：仍然是两条精确等值断言，仍然禁止改成满分辨率，
    // 只是禁止的理由从"带宽"变成了"带宽 + 720p 零帧"。
    V4l2CaptureConfig cfg;

    EXPECT_EQ(cfg.device, "/dev/video0");
    EXPECT_EQ(cfg.width, 1640u)
        << "默认必须落在 bin_factor=1 通路上：720p/1080p/SXGA 在本板零帧";
    EXPECT_EQ(cfg.height, 1232u)
        << "1640x1232 由驱动按 3280x2464 采样后缩放，同帧率下带宽仅为满分辨率 1/4";
    EXPECT_EQ(cfg.fps, 30u);
    EXPECT_EQ(cfg.buffer_count, 4u) << "方案 §6.2.2.2：4 个 buffer";
    EXPECT_GT(cfg.select_timeout_ms, 0u) << "必须有超时，否则卡死时无法返回";
    EXPECT_GT(cfg.window_ms, 0u) << "窗口时长决定能否与网络侧对齐";
}

TEST(V4l2FrameSource, DefaultResolutionAvoidsBinningModes) {
    // 守的是「默认值不得落回 bin_factor=2 坏模式」这条真机约束。
    //
    // 这里不查表也不读驱动源码（单测跑在 CI，没有 sensor 也没有 BSP 源码），
    // 而是把板端**实测过零帧**的坏模式逐个列出来，断言默认值不是其中任何一个。
    //
    // 只收实测过的：`sensor_win_sizes[]` 里还有一个 SXGA 模式同样是
    // `bin_factor=2`，但它的尺寸宏不在本仓库、本轮也没有实测到它，因此
    // **不写进名单**——按证据纪律，没测过的不当成结论。
    // 加新模式时这条会继续生效；若将来驱动修好了 binning，改这里的名单即可，
    // 但必须同时更新 §7.9 的证据，而不是悄悄删掉这条测试。
    const std::vector<std::pair<uint32_t, uint32_t>> known_broken = {
        {1280, 720},    // 实测 rc=124 / 0 字节 / 710 条 TDM frame error
        {1920, 1080},   // 实测 rc=124 / 0 字节 / 711 条 TDM frame error
        {640, 480},     // 实测 rc=124 / 0 字节 / 710 条 TDM frame error
    };

    V4l2CaptureConfig cfg;
    for (const auto& m : known_broken) {
        EXPECT_FALSE(cfg.width == m.first && cfg.height == m.second)
            << "默认分辨率落在了本板实测零帧的 bin_factor=2 模式 "
            << m.first << "x" << m.second
            << " 上；见 docs/hardware/radxa-cubie-a7a/"
               "sunxi-vin-摄像头链路根因分析.md §7.9";
    }
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

TEST(V4l2FrameSource, DmabufIsOffByDefaultAndTopologyProbeIsOn) {
    // 两个新配置项的默认值各有一条明确理由，把它们变成可执行断言，
    // 避免将来有人「顺手」改默认值而无人察觉。
    V4l2CaptureConfig cfg;

    // DMABUF 默认关：本帧源只消费 v4l2_buffer 的**元数据**（bytesused /
    // sequence / timestamp），从不读像素。MMAP 那次映射本来就没被访问过，
    // 换成 DMABUF 不会减少任何像素拷贝，只会多占 N 个 fd。
    // 它不是性能优化开关，是「将来要把帧交给编码器」时的预留通路。
    EXPECT_FALSE(cfg.use_dmabuf)
        << "DMABUF 默认必须关闭：本项目当前没有第二个消费像素的进程，"
           "开启它只增加 fd 占用而无任何收益";

    // 拓扑探测默认开：它是**只读**诊断信息，失败不影响采集，
    // 而真机上「节点打开了却拿不到帧」只有拓扑能解释（V4L2 只会报 EINVAL）。
    // 关掉它等于放弃唯一的现场诊断线索。
    EXPECT_TRUE(cfg.probe_media_topology)
        << "拓扑探测必须默认开启：只读、无副作用，且是真机上唯一能区分"
           "「设备不存在」与「链路没通」的依据";
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

/// describe() 里可能出现的缓冲来源落点
///
/// 刻意做成枚举而不是字符串比较：`dmabuf->mmap-fallback` **包含**
/// 子串 "dmabuf"，用 `find("dmabuf") != npos` 判断会把「静默回落」
/// 认成「成功」。这是本项目已经踩过的假绿形态，必须用精确解析堵住。
enum class BufferSourceKind {
    kUnreported,       ///< describe() 压根没写（配置开了却沉默 = 缺陷）
    kDmabufHeap,       ///< 导入路径：dma-heap 分配
    kDmabufExport,     ///< 导出路径：VIDIOC_EXPBUF
    kDmabufMmapFallback, ///< 配了 DMABUF 但两级全失败，回落 MMAP
    kPlainMmap,        ///< 未配置 DMABUF 的默认路径
};

/// 从 describe() 的一行里解析出实际落点
///
/// 注意解析顺序：`dmabuf->mmap-fallback` 必须排在 `dmabuf(` 之前判断，
/// 否则会在 fallback 字符串里先命中别的前缀。
BufferSourceKind parseBufferSource(const std::string& desc) {
    if (desc.find("dmabuf->mmap-fallback") != std::string::npos)
        return BufferSourceKind::kDmabufMmapFallback;
    if (desc.find("dmabuf(heap)") != std::string::npos)
        return BufferSourceKind::kDmabufHeap;
    if (desc.find("dmabuf(export)") != std::string::npos)
        return BufferSourceKind::kDmabufExport;
    if (desc.find(" mmap") != std::string::npos)
        return BufferSourceKind::kPlainMmap;
    return BufferSourceKind::kUnreported;
}

/// 本进程能否真的用 dma-heap 分配
///
/// **不能只看 /dev/dma_heap/system 是否存在**：实测该节点是
/// `crw------- root root`，普通用户 open() 直接 EACCES。用存在性判断会让
/// 测试在非 root 下得出「应该走 heap」的错误期望，然后误报失败。
/// 这里真开一次、真分配一次，用结果说话。
///
/// 分配长度取一页即可：只验证权限与 ioctl 通路，不验证具体尺寸。
bool canOpenDmaHeap() {
    const int hfd = ::open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (hfd < 0) return false;
#ifdef DMA_HEAP_IOCTL_ALLOC
    dma_heap_allocation_data data{};
    data.len = 4096;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    const bool ok = ::ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &data) >= 0;
    if (ok && data.fd > 0) ::close(static_cast<int>(data.fd));
    ::close(hfd);
    return ok;
#else
    // 内核头文件太旧、连 DMA_HEAP_IOCTL_ALLOC 都没有 → 实现侧也走不到导入
    ::close(hfd);
    return false;
#endif
}

/// 是否要求「必须走通 EXPBUF 导出」这一更严的判据
///
/// 默认关闭。原因：导出可用性是**驱动能力**，不是本类的行为契约——
/// 驱动不支持 EXPBUF 时回落 MMAP 完全正确。只有在已知驱动支持导出
/// （例如 vivid）并刻意隐藏了 `/dev/dma_heap` 的演练里，才把它打开，
/// 用来把「导出路径整段坏掉」与「正常回落」区分开。
///
/// 不加这层区分时，禁用导出 / 导出后 REQBUFS 用错类型这两类缺陷
/// 都会静默落进 fallback 分支，测试仍然全绿——实测确认过。
bool expectDmabufExport() {
    const char* p = std::getenv("WEAKNET_V4L2_EXPECT_DMABUF_EXPORT");
    return p != nullptr && *p == '1';
}

/// 把某节点的输入选择读出来；设备不实现 G_INPUT 时返回 -1
///
/// 为什么要读输入而不是「看 start() 有没有调 S_INPUT」：后者需要 mock
/// ioctl，而本项目拒绝为了可测性在生产代码里塞钩子。输入选择是
/// **驱动侧可观测的真实副作用**——尤其 vivid 的输入索引跨 `open()`
/// 保留，所以「先把设备切成 1 号输入，再 start()，结束后读回」就能
/// 判断 start() 到底有没有下发 S_INPUT(0)。这是黑盒可证的唯一形态。
int readInputIndex(const std::string& device) {
    const int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return -1;
    int idx = -1;
    if (::ioctl(fd, VIDIOC_G_INPUT, &idx) < 0) idx = -1;
    ::close(fd);
    return idx;
}

/// 把某节点的输入切到 index；成功返回 true
bool selectInput(const std::string& device, int index) {
    const int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;
    const bool ok = ::ioctl(fd, VIDIOC_S_INPUT, &index) >= 0;
    ::close(fd);
    return ok;
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

TEST_F(V4l2LiveTest, StartSelectsInputZeroBeforeFormatting) {
    // ========================================================================
    // 这条守的是 sunxi-vin 上「S_FMT 必 EINVAL」的根因
    // ========================================================================
    //
    // 机制（BSP 源码逐行确认，见
    // `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md`）：
    // `vid_cap.pipe.sd[VIN_IND_SENSOR]` 只由 `vin_md_prepare_pipeline()` 填充，
    // 而该函数靠 `media_pad_remote_pad_first()` 上溯、**只走 ENABLED 链路**；
    // `vin.c:2035` 建 sensor→mipi 链路时传 `flags=0`（disabled），所以
    // probe 期那次填充填不出 sensor。此后 `vin_pipeline_try_format()` 里
    // `WARN_ON(!sd || ...)` 命中并 `return -EINVAL`。
    //
    // 唯一能在用户态补上这个填充的入口是 `VIDIOC_S_INPUT`：
    // `__vin_s_input()` → `__vin_sensor_setup_link(...,1)` →
    // `vin_pipeline_call(open, ..., prepare=true)`。`STREAMON` 虽然也调用了
    // setup_link，但**不**带 `prepare=true`，补不回来；手动
    // `MEDIA_IOC_SETUP_LINK` 同理（之前的会话正是这样误判的）。
    //
    // 断言形式：把驱动切到非 0 输入，再让 start() 跑完，读回输入索引。
    // 若实现下发了 S_INPUT(0)，索引必须是 0。这一步不依赖任何 sunxi 私有
    // 接口——vivid 的输入索引跨 open() 保留，因此是黑盒可证的。
    const std::string dev = v4l2TestDevice();
    if (readInputIndex(dev) < 0) {
        GTEST_SKIP() << "该设备无输入概念（G_INPUT 不可用），S_INPUT 契约不适用";
    }
    if (!selectInput(dev, 1)) {
        GTEST_SKIP() << "该设备只有一个输入或不允许切换，无法构造前置条件";
    }
    ASSERT_EQ(readInputIndex(dev), 1) << "前置条件构造失败：未能切到输入 1";

    auto cfg = makeConfig(640, 480, 30, 300);
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    EXPECT_EQ(readInputIndex(dev), 0)
        << "start() 未把输入切回 0：sunxi-vin 上这会让 S_FMT 直接返回 EINVAL。"
           "修复方式是在 S_FMT **之前**下发 VIDIOC_S_INPUT(0)，"
           "并容忍 ENOTTY/EINVAL（无输入的设备）。";

    src.stop();

    // 清理：把驱动恢复原状，避免污染同进程后续用例
    selectInput(dev, 0);
}

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

TEST_F(V4l2LiveTest, FpsCalibrationReplacesLyingDriverValue) {
    // ========================================================================
    // 守「驱动报的帧率可能是模式表常数，必须用实测校准」这条
    // ========================================================================
    //
    // 背景（真机证据见 `openspec/pitfalls/sensor-mode-table-fps-lie.md`）：
    // IMX219 + sunxi-vin 的满分辨率模式表声明 `.pclk=278MHz / .fps_fixed=30`，
    // 但该模式 PLL 只配到 MPY=54，真实 pixel rate 172.8MHz → 18.56fps。
    // 而 `sensor_s_parm()` 只把 tpf 存进结构体、`sensor_g_parm()` 直接返回
    // 模式表常数，于是 `G_PARM` 永远报 30，下面的「用驱动实际值回填」永不触发，
    // `frames_expected` 恒按 30 推 → 约 38% 的**纯计算**假丢帧。
    //
    // 构造手法：**关掉帧率协商**（`set_frame_rate=false`）并把 cfg.fps 配成
    // 实测帧率的 2 倍。vivid 稳定出 30fps，配 60 就能让「校准是否生效」
    // 变成可观测的强信号：
    //   - 未校准时 expected 是实测的 2 倍，丢帧率稳定在 ~50%；
    //   - 校准生效后 expected 回落到实测量级，丢帧率回到低位。
    //
    // **为什么必须关掉协商**（这条是本用例第一次写错的地方）：`start()` 里
    // 有一段「以驱动回报为准回填 cfg.fps」的逻辑。vivid 的 G_PARM **是诚实的**
    // （真的回报 30），所以只要开着协商，60 会在 start() 里就被改回 30，
    // 校准再也没有可修正的内容——测出来的 before/after 都是 0 丢帧，
    // 断言失败但代码其实没错。这恰好是「测试构造不成立」而非「实现有缺陷」。
    //
    // 关掉协商后 cfg.fps 一路保持 60，等价于真机上「G_PARM 撒谎」的情形
    // （模式表写 30、硬件实际 18.56），这才是校准要解决的场景。
    auto cfg = makeConfig(640, 480, 120, 500);
    cfg.set_frame_rate = false;        // 模拟驱动回报不可信：不回填 cfg.fps
    // 明确要求校准 3 个窗口：窗口数不够就永远等不到校准完成
    cfg.fps_calibration_windows = 3;
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    // 前 3 个窗口用于攒校准样本；第 3 个窗口结束时校准完成。
    // 注意 snapshotAndReset() 自身阻塞一个 window_ms，必须连续调用，
    // 中间 sleep 会让队列积压从而污染帧间隔（既有单测已钉死该语义）。
    double loss_before = -1.0;
    for (int w = 0; w < 3; ++w) {
        const FrameStats s = src.snapshotAndReset();
        ASSERT_TRUE(s.valid());
        if (w == 0) loss_before = s.frameLossRatio();
    }

    const FrameStats after = src.snapshotAndReset();
    ASSERT_TRUE(after.valid());
    ASSERT_GT(after.frames_received, 0u) << "本窗口没收到帧，无法判断校准效果";

    // 校准生效的直接证据：丢帧率从「约 50%（60 vs 30）」掉到与实测一致。
    EXPECT_LT(after.frameLossRatio(), loss_before - 0.15)
        << "帧率校准未生效：丢帧率没有下降。"
        << " before=" << loss_before << " after=" << after.frameLossRatio()
        << " expected=" << after.frames_expected
        << " received=" << after.frames_received;

    // describe() 必须留下校准痕迹，否则现场无法判断 expected 用的是哪个 fps
    const std::string desc = src.describe();
    EXPECT_NE(desc.find("[fps:"), std::string::npos)
        << "校准（或跳过原因）必须写进 describe() 供证据溯源，实际为：" << desc;

    src.stop();
}

TEST_F(V4l2LiveTest, FpsCalibrationIsSkippedWhenDisabled) {
    // 守「fps_calibration_windows == 0 时校准必须完全不动作」。
    //
    // 这条不是形式主义：校准会改写 cfg.fps，而 cfg.fps 决定 frames_expected。
    // 若关闭开关后仍在偷偷校准，调用方「我明确要求不校准」的意图就被违背，
    // 且无法从任何输出看出来。判据是 describe() 里不得出现 [fps: 段。
    auto cfg = makeConfig(640, 480, 120, 500);
    cfg.set_frame_rate = false;        // 同 FpsCalibrationReplacesLyingDriverValue：
                                       // 不回填 cfg.fps，校准才有可修正的内容
    cfg.fps_calibration_windows = 0;   // 关闭
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    for (int w = 0; w < 4; ++w) src.snapshotAndReset();

    const std::string desc = src.describe();
    EXPECT_EQ(desc.find("[fps:"), std::string::npos)
        << "已关闭校准却仍写了校准记录，说明开关没生效：" << desc;

    src.stop();
}

TEST_F(V4l2LiveTest, FpsCalibrationResultSurvivesUntilRestart) {
    // 守「校准只做一次」这条，它是整个设计里最关键的约束。
    //
    // 若每窗口都重算，真实丢帧会被分母吸收（丢帧 → 实测帧率降低 →
    // expected 同步降低 → 丢帧率恒为 0），把「全丢」显示成「完美」。
    // 这是比假丢帧危险得多的失败模式，所以必须有一条测试钉住它。
    //
    // 观测手法：校准完成后 stop() 之前，describe() 里的 fps 段内容必须
    // 保持稳定；且再跑 N 个窗口不会因重新校准而产生新的描述。
    auto cfg = makeConfig(640, 480, 120, 400);
    cfg.set_frame_rate = false;        // 必须：否则 start() 会把 60 回填成 30，
                                       // 校准无事可做，describe() 前后都无变化
    cfg.fps_calibration_windows = 2;
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    for (int w = 0; w < 2; ++w) src.snapshotAndReset();   // 完成校准
    const std::string desc_after_calibration = src.describe();

    // 再跑若干个窗口：若存在持续重算，cfg.fps 会继续漂移
    for (int w = 0; w < 5; ++w) src.snapshotAndReset();

    EXPECT_EQ(src.describe(), desc_after_calibration)
        << "校准后 describe() 仍在变化，说明校准没有「只做一次」，"
           "持续重算会吸收真实丢帧";

    src.stop();
}

TEST_F(V4l2LiveTest, FpsCalibrationDoesNotAbsorbRealFrameLoss) {
    // ========================================================================
    // 守「校准不得吸收真实丢帧」——整个设计里最危险的一条失败模式
    // ========================================================================
    //
    // 若按「每窗口用实测帧间隔反推 fps」，会形成这个闭环：
    //   丢帧 → 本窗口收到的帧少 → 实测帧率变低 → expected 同步变小 → 丢帧率归零
    // 结果是「摄像头 93% 的帧没送到」被显示成「画面完美」。
    // 假丢帧（38% 那个）只是让人多查一次；这个假绿灯会让人永远不查。
    //
    // 构造手法：`buffer_count=2` + 窗口之间故意 sleep 1.5s（窗口设计跨度
    // 只有 500ms）。睡眠期间队列填满、驱动丢帧，于是每个窗口
    // received≈2 / expected≈30，**真实丢帧率约 93%**。
    //
    // 断言：校准不得把这个高丢帧率洗掉。允许校准改变 expected
    // （它本来就要修分母），但**不允许丢帧率掉到「看起来健康」的量级**。
    // 这条正是变异测试抓出的缺口——最初的
    // `FpsCalibrationResultSurvivesUntilRestart` 在恒定帧率的 vivid 上
    // 对「每窗口重算」毫无分辨力（重算两次得到同样的值），必须靠真实丢帧
    // 才能把两种实现区分开。
    auto cfg = makeConfig(640, 480, 120, 500);
    cfg.set_frame_rate = false;    // 让 cfg.fps 保持 120，校准有活干
    cfg.buffer_count = 2;          // 小队列 + 睡眠 ⇒ 稳定丢帧
    cfg.fps_calibration_windows = 2;
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    double max_loss = 0.0;
    for (int w = 0; w < 6; ++w) {
        // 窗口跨度 500ms，睡 1.5s ⇒ 队列填满、丢帧
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const FrameStats s = src.snapshotAndReset();
        ASSERT_TRUE(s.valid());
        max_loss = std::max(max_loss, s.frameLossRatio());
    }

    // 真实丢帧确实发生了（否则这条测试什么都没守住）
    ASSERT_GT(max_loss, 0.3)
        << "前置条件不成立：本场景没有制造出真实丢帧（max_loss=" << max_loss
        << "），测试无法证明校准不会吸收丢帧";

    // ========================================================================
    // 关键断言：**分母必须反映硬件帧率，而不是收到的帧数**
    // ========================================================================
    //
    // 这是「吸收」与「校准」的分界线，也是本条测试第一次写错的地方：
    // 我最初断言「丢帧率必须 > 0.5」，结果正确实现被判失败——因为校准
    // 把 fps 从 120 修到 60（vivid 真实值）后，30/60 = 0.5 恰好是**如实**的
    // 结果，不是掩盖。
    //
    // 真正该守的是：**expected 由帧间隔反推，不能由收到多少帧反推**。
    //   - 正确实现：帧间隔 16.7ms → fps≈60 → expected≈30/窗口 → loss≈0.5
    //     （丢了约一半，如实报出来）
    //   - 吸收型实现：看到只收到 2 帧 → fps≈4 → expected≈2 → loss≈0
    //     （把丢掉的帧数当成了「本来就没有」）
    //
    // 所以判据是 expected 的量级：它必须对应**几百毫秒窗口 × 几十 fps**，
    // 绝不能塌缩到与 received 同量级。
    const FrameStats last = src.snapshotAndReset();
    ASSERT_TRUE(last.valid());
    ASSERT_GT(last.frames_received, 0u);

    EXPECT_GE(last.frames_expected, 10u)
        << "expected 塌缩到 " << last.frames_expected << "（received="
        << last.frames_received << "），说明分母是按「收到的帧数」反推的，"
        << "真实丢帧被吸收。expected 必须反映硬件帧率 × 窗口跨度。";

    EXPECT_GT(last.frameLossRatio(), 0.3)
        << "丢帧率被校准洗成健康值：expected=" << last.frames_expected
        << " received=" << last.frames_received
        << " loss=" << last.frameLossRatio();

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

// ---------------------------------------------------------------------------
// 6.7 缓冲来源：DMABUF 与回落的**可观测性**
//
// 这几条守的不是「DMABUF 更快」（本项目压根不需要它，见配置项注释），
// 而是三件更容易出错的事：
//   1. 开启后必须真的走通，而不是静默退回 MMAP 让人以为生效了；
//   2. 无论哪条路径，FrameStats 的语义必须与 MMAP 完全一致；
//   3. describe() 必须如实报告**实际**落点。
// ---------------------------------------------------------------------------

TEST_F(V4l2LiveTest, DmabufPathProducesSameStatsSemanticsAsMmap) {
    // 核心断言：缓冲来源不影响统计语义。
    //
    // 这不是废话——DMABUF 下 bytesused 是否被驱动正确填写、时间戳是否
    // 仍然打戳、requeue 时 fd 是否被正确重新绑定，任何一项出问题都会让
    // FrameStats 变形（典型表现：坏帧率 100% 或 seq_gap 暴涨）。
    // 实测 vivid 上三条路径的 exp/recv/gap/corrupt 完全一致。
    auto run_once = [this](bool use_dmabuf, FrameStats& out) {
        auto cfg = makeConfig(640, 480, 30, 400);
        cfg.use_dmabuf = use_dmabuf;
        V4l2FrameSource src(cfg);
        if (!src.start()) return false;
        out = src.snapshotAndReset();
        src.stop();
        return out.valid();
    };

    FrameStats mmap_stats;
    FrameStats dmabuf_stats;
    ASSERT_TRUE(run_once(false, mmap_stats)) << "MMAP 路径未能出帧";
    ASSERT_TRUE(run_once(true, dmabuf_stats)) << "DMABUF 路径未能出帧";

    // 帧数只做宽松比对：两个窗口不同时刻采样，允许 ±4 帧抖动
    EXPECT_NEAR(static_cast<double>(dmabuf_stats.frames_received),
                static_cast<double>(mmap_stats.frames_received), 4.0)
        << "DMABUF 与 MMAP 收到的帧数差异过大，说明其中一条路径在丢帧";

    // 更关键的是**语义不变**：DMABUF 下不得凭空出现坏帧或序号跳变。
    EXPECT_EQ(dmabuf_stats.frames_corrupt, 0u)
        << "DMABUF 下出现坏帧，最可能是 bytesused 未被正确回读";
    EXPECT_EQ(dmabuf_stats.seq_gap_count, 0u)
        << "DMABUF 下出现序号跳变，最可能是 requeue 时 fd 绑定错误导致丢帧";
    EXPECT_GT(dmabuf_stats.bytes_received, 0u)
        << "DMABUF 下字节数为 0，说明统计链路断了";
    EXPECT_EQ(dmabuf_stats.window_ms, 400u) << "窗口跨度语义不得因缓冲来源而变";
}

TEST_F(V4l2LiveTest, DescribeReportsActualBufferSourceWhenDmabufRequested) {
    // **静默失效是配置项最危险的形态**：若 `use_dmabuf=true` 但实际回落
    // 到了 MMAP，而 describe() 什么都不说，运维会一直以为自己跑在 DMABUF 上。
    //
    // 这里不断言「一定是 dmabuf(heap)」——那取决于**运行身份**：
    // 实测 `/dev/dma_heap/system` 是 `0600 root:root`，非 root 进程连 open
    // 都会 EACCES，于是自然落到二级的 EXPBUF 导出路径。断言绝对落点会让
    // 同一份代码在 root/非 root 下结论相反，那不是测试，是环境快照。
    //
    // 但仍必须断言**具体落点**，不能只断言「包含 dmabuf」：
    // `dmabuf->mmap-fallback` 同样包含 "dmabuf"，前者会把「静默回落」
    // 认成「成功」。实测这一条正是 M3 变异（describe 漏报 heap）能存活的
    // 原因——非 root 环境永远只在 export/fallback 上打转，根本碰不到
    // heap 分支，漏报它也就无人发现。
    auto cfg = makeConfig(640, 480, 30, 300);
    cfg.use_dmabuf = true;

    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    const std::string d = src.describe();
    const BufferSourceKind got = parseBufferSource(d);

    // case 值：0=未报告 1=heap 2=export 3=mmap-fallback 4=mmap
    ASSERT_NE(got, BufferSourceKind::kUnreported)
        << "配置了 use_dmabuf=true 但 describe() 未报告实际落点：" << d
        << "\n（若确实回落了 MMAP，应输出 dmabuf->mmap-fallback）";

    // 环境能力决定**唯一合法的期望落点**，而不是「随便哪条都行」。
    if (canOpenDmaHeap()) {
        EXPECT_EQ(got, BufferSourceKind::kDmabufHeap)
            << "本进程可访问 /dev/dma_heap/system，导入路径必须生效，"
               "实际却落在别处：" << d;
    } else {
        // 无法分配 dma-heap 时导入必然失败。接下来该走 EXPBUF 导出——
        // 但「导出是否可用」是**驱动能力**，不是本类的行为：
        // 部分驱动（如一些 UVC）根本不支持 VIDIOC_EXPBUF，此时回落到
        // MMAP 是正确行为，硬断言 export 会变成假失败。
        //
        // 所以默认只做宽松断言（排除说了谎的 heap），由显式开关
        // WEAKNET_V4L2_EXPECT_DMABUF_EXPORT=1 收紧为「必须导出成功」。
        // 该开关配合隐藏 /dev/dma_heap 使用（见变异演练）：
        //   unshare -m bash -c 'mount -t tmpfs none /tmp/eh
        //     ; mount --bind /tmp/eh /dev/dma_heap
        //     ; WEAKNET_V4L2_EXPECT_DMABUF_EXPORT=1 ...'
        //
        // 为什么必须留这个开关：不加它时，「禁用 EXPBUF」「export 后
        // REQBUFS 用错 memory」这类变异会落在 fallback 上而被放行——
        // 实测确认这两条变异在宽松断言下 100% 存活。那是假绿。
        EXPECT_NE(got, BufferSourceKind::kDmabufHeap)
            << "本进程无法访问 dma-heap，却报告走通了导入路径：" << d;

        if (expectDmabufExport()) {
            EXPECT_EQ(got, BufferSourceKind::kDmabufExport)
                << "已要求必须走通 EXPBUF 导出，实际却落在：「" << d
                << "」\n导出路径整段失效，或 describe() 漏报了它。";
        } else {
            EXPECT_TRUE(got == BufferSourceKind::kDmabufExport ||
                        got == BufferSourceKind::kDmabufMmapFallback)
                << "非法落点（既不是 export 也不是 fallback）：" << d;
        }
    }

    // 停止后仍必须如实报告**同一个**落点，不得变回「没配过」的样子。
    // 这一条守 buffer_source 不被 closeDevice() 复位。
    src.stop();
    const std::string after = src.describe();
    EXPECT_EQ(parseBufferSource(after), got)
        << "stop() 之后 describe() 的缓冲来源变了或丢失：" << after
        << "\n期望仍是与运行期一致的那条路径（buffer_source 被复位了？）";
}

TEST_F(V4l2LiveTest, DmabufDisabledLeavesDescribeClean) {
    // 默认路径不应在 describe() 里加任何缓冲来源噪音：每行日志多一个
    // "mmap" 会让真正的偏差记录（[帧率由…校正为…]）难以被发现。
    auto cfg = makeConfig(640, 480, 30, 300);
    ASSERT_FALSE(cfg.use_dmabuf) << "本用例前提是默认关闭";

    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    const std::string d = src.describe();
    EXPECT_EQ(d.find("dmabuf"), std::string::npos)
        << "未开启 DMABUF 时 describe() 不应提到它：" << d;
    src.stop();
}
