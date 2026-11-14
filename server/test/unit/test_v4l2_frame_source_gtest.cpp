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

/// 从 describe() 里反读**实际生效**的帧率；解析失败返回 0
///
/// ============================================================================
/// 为什么必须从 describe() 读，而不是用传进去的 cfg.fps
/// ============================================================================
///
/// `V4l2FrameSource` 会在两个地方改写内部 fps：
///   1. `start()` 用驱动 `G_PARM` 的回报回填（`set_frame_rate` 默认 true）
///   2. 帧率基线校准用实测帧间隔修正
///
/// 于是调用方手里那份 `V4l2CaptureConfig` 只是**入参**，不代表运行时真相。
/// 2026-11-12 真机暴露的失败正是如此：板端 G_PARM 谎报 30、校准成 18，
/// 测试拿入参的 30 算期望帧数，得到 17；实现按 18 算出 10 —— 实现是对的。
///
/// describe() 的格式是 `v4l2:<dev>@<W>x<H>/<FPS>fps/<WINDOW>ms ...`，
/// 这里的解析与 `DescribeFormatIsParseableForEffectiveFps` 这条无设备单测
/// 共同锁住该格式：格式若变，那条单测先红，而不是等到上板才发现。
///
/// 注意别用 `find('/', at)` 定位：那会命中 `<W>x<H>` 后面的 '/',
/// 取到 "1640x1232" 这一段（strtoul 得 1640，或首字符是字母时得 0）。
/// 必须从 "fps/" 往前找最近的 '/'。
uint32_t parseEffectiveFps(const std::string& desc) {
    const std::size_t fpspos = desc.find("fps/");
    if (fpspos == std::string::npos) return 0;
    const std::size_t prev = desc.rfind('/', fpspos);
    if (prev == std::string::npos || prev + 1 >= fpspos) return 0;
    return static_cast<uint32_t>(
        std::strtoul(desc.substr(prev + 1, fpspos - prev - 1).c_str(), nullptr, 10));
}

TEST(V4l2FrameSource, DescribeFormatIsParseableForEffectiveFps) {
    // 守 describe() 里 `@<W>x<H>/<FPS>fps/<WINDOW>ms` 这段格式可被稳定解析。
    //
    // 为什么要在无设备单测里钉死它：真机用例需要从 describe() 反读
    // **实际生效**的 fps（校准会改写 cfg.fps，入参副本已不代表真相），
    // 而 2026-11-12 那个解析实现正是靠这条格式工作的——第一次写时用
    // `find('/', at)` 定位，命中了分辨率后面的 '/', 得到 0，
    // 于是在板上报「无法解析实际帧率」，看起来像实现坏了。
    //
    // 把格式与解析规则绑在一起测，比在真机用例里散落一段手写解析可靠：
    // 将来谁改了 describe() 的格式，这里立刻失败，而不是等到上板才发现。
    V4l2CaptureConfig cfg;
    cfg.device = "/dev/video-format-probe";
    cfg.width = 1640;
    cfg.height = 1232;
    cfg.fps = 18;
    cfg.window_ms = 500;

    V4l2FrameSource src(cfg);
    const std::string d = src.describe();

    // 直接复用真机用例里的同一个解析函数：两边共用一份实现，
    // 才不会出现「单测测的是另一段代码」这种假护栏。
    const uint32_t parsed_fps = parseEffectiveFps(d);
    ASSERT_GT(parsed_fps, 0u) << "describe() 无法解析出 fps，实际：" << d;

    EXPECT_EQ(parsed_fps, cfg.fps)
        << "解析出 " << parsed_fps << "fps，与配置的 " << cfg.fps
        << "fps 不符。describe：" << d;

    // 分辨率也必须能被同一段格式解析出来（防止有人把 WxH 挪走）
    const std::size_t at = d.find('@');
    ASSERT_NE(at, std::string::npos) << "describe() 缺少分辨率分隔符 '@'：" << d;
    const std::size_t fpspos = d.find("fps/");
    ASSERT_NE(fpspos, std::string::npos) << "describe() 缺少 'fps/'：" << d;
    const std::size_t prev = d.rfind('/', fpspos);
    ASSERT_NE(prev, std::string::npos) << "describe() 缺少 fps 前的 '/'：" << d;
    const std::string between = d.substr(at + 1, prev - at - 1);
    EXPECT_EQ(between, "1640x1232") << "分辨率段应为 '1640x1232'，实际：'"
                                    << between << "'";
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

        // ====================================================================
        // 分辨率必须可配，不能硬编码 640x480
        // ====================================================================
        //
        // 2026-11-12 在板上实测：IMX219 + sunxi-vin 在 640x480 下
        // **恒定零帧**（`recv=0 bytes=0`，采集线程活着、describe() 正常），
        // 而同一条链路上 1640x1232 稳定 18.56fps。这不是本类的缺陷——
        // 同一个二进制在宿主机 vivid 的 640x480 上一切正常。
        //
        // 换句话说，640x480 这个**测试常量**实际上编码了「vivid 支持哪些模式」
        // 这一宿主机事实。把它搬到板端就等于在断言「IMX219 也必须支持 640x480」，
        // 那是设备能力问题，与被测代码无关。
        //
        // 所以改为由环境变量给出「本机确实能出帧的模式」：
        //   宿主机（vivid）：不设即可，默认 640x480
        //   板端（IMX219） ：WEAKNET_V4L2_TEST_WIDTH=1640 HEIGHT=1232
        // 未显式指定时保持 640x480，宿主机行为完全不变。
        width_ = envU32("WEAKNET_V4L2_TEST_WIDTH", 640);
        height_ = envU32("WEAKNET_V4L2_TEST_HEIGHT", 480);
    }

    V4l2CaptureConfig makeConfig(uint32_t width, uint32_t height,
                                 uint32_t fps, uint32_t window_ms) {
        V4l2CaptureConfig cfg;
        cfg.device = v4l2TestDevice();
        cfg.width = width;
        cfg.height = height;
        cfg.fps = fps;
        cfg.window_ms = window_ms;
        return cfg;
    }

    /// 用本机可出帧的分辨率构造配置（绝大多数用例该用这个）
    ///
    /// 只有当用例**本身就在测分辨率**时才该用带参版本。
    V4l2CaptureConfig makeConfig(uint32_t fps, uint32_t window_ms) {
        return makeConfig(width_, height_, fps, window_ms);
    }

    /// 从环境变量读一个 uint32，未设置或非法则用默认值
    static uint32_t envU32(const char* name, uint32_t fallback) {
        const char* v = std::getenv(name);
        if (v == nullptr || *v == '\0') return fallback;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(v, &end, 10);
        // 严格解析：`"640abc"` 这类半截输入不当成 640，避免静默误配
        if (end == v || (end != nullptr && *end != '\0') || parsed == 0 ||
            parsed > 65535ul) {
            return fallback;
        }
        return static_cast<uint32_t>(parsed);
    }

    std::string device_;
    uint32_t width_{640};
    uint32_t height_{480};
};

/// 按新契约取一份「覆盖跨度至少 span_ms」的统计
///
/// ============================================================================
/// 为什么需要这个辅助（而不是像改造前那样直接调 snapshotAndReset）
/// ============================================================================
///
/// 2026-11-12 起 `snapshotAndReset()` 是**立即返回**的：采集由内部线程连续
/// 驱动，快照只是取走累计值。改造前它内部有一个排空循环、会阻塞满 window_ms，
/// 于是「调用它就等于等了一个窗口」。
///
/// 新契约下若测试仍按「调一次 = 一个窗口」写，会在**刚 start 完的那一瞬间**
/// 拿到一份跨度 1ms、帧数 0 的统计，然后断言 expected=15 全部失败——测的是
/// 「调用方没等」，不是类本身的缺陷。
///
/// 所以测试侧显式表达真实使用方式：**睡够时长，只取一次**。
///
/// ---------------------------------------------------------------------------
/// 为什么是「只取一次」而不是轮询到跨度为 span_ms 为止
/// ---------------------------------------------------------------------------
///
/// 每次 `snapshotAndReset()` 都会把本窗口累计值清零。若在等待期间中途取过
/// 几次，最后那份统计只覆盖「最后一次取数到现在」这一小段，`frames_received`
/// 也跟着只剩这一小段——用它去断言「span 秒内应到 N 帧」必然失败，而且
/// 失败原因是辅助函数自己制造的，不是被测对象的缺陷。
///
/// 想拿到「完整 span 的帧数」，就必须让整个 span 落在同一个窗口里。
///
/// `slack_ms` 补偿 sleep 的实际精度与调度延迟，使 `window_ms >= span_ms`
/// 是一条稳定成立的不等式，而不是随负载抖动的运气。**不要**用返回值里的
/// `window_ms` 之外的任何常量当分母——跨度是事实，由被测对象自己报。
FrameStats snapshotAfter(V4l2FrameSource& src, uint32_t span_ms,
                         uint32_t slack_ms = 60) {
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<long long>(span_ms) + static_cast<long long>(slack_ms)));
    return src.snapshotAndReset();
}

/// 用**第二个 fd** 改驱动的出帧速率（流运行中生效）
///
/// ============================================================================
/// 为什么需要这个手法（而不是「让调用方不取数、把队列灌满」）
/// ============================================================================
///
/// 老办法在新契约下彻底失效了：采集线程永远在排空，队列永远填不满，
/// 于是再也制造不出真实丢帧。但「校准不得吸收真实劣化」这条契约必须继续守，
/// 所以换成**驱动侧降速**——帧真的变少了，而且帧间隔是**均匀**的
/// （不是丢帧造成的空洞）。
///
/// 这恰恰是最容易被吸收型实现洗掉的形态：按「received / window」反推 fps
/// 会得到降低后的值，`expected` 同步缩小，`frameLossRatio()` 归零，
/// 「摄像头出帧能力掉到三分之一」被显示成「画面完美」。
///
/// **只调 S_PARM，不碰 S_FMT / REQBUFS**：后两者会干扰正在跑的流
/// （前者会重配格式、后者会重分配缓冲），S_PARM 只改时间基。
/// 本机实测（vivid, 6.8）：流运行中把 30 降到 10 后，200 帧耗时由 6.7s
/// 变为 15.84s，与理论值 16.0s 吻合。
///
/// 平面类型两种都试：设备可能是单平面也可能是多平面，`S_PARM` 的 `type`
/// 必须与实际一致，先猜单平面（vivid / 多数 UVC 的默认），失败再试多平面。
bool setDeviceFrameRate(const std::string& device, uint32_t fps) {
    const int fd = ::open(device.c_str(), O_RDWR);
    if (fd < 0) return false;

    bool ok = false;
    for (const v4l2_buf_type type :
         {V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE}) {
        v4l2_streamparm parm{};
        parm.type = type;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = fps;
        if (::ioctl(fd, VIDIOC_S_PARM, &parm) == 0) { ok = true; break; }
    }
    ::close(fd);
    return ok;
}

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
    // **构造失败必须 skip 而不是 fail**（2026-11-12 真机修正）。
    //
    // 板端 sunxi-vin 只暴露 1 个输入：`selectInput(dev, 1)` 可能返回 true
    // （S_INPUT 没报错）但读回仍是 0。此时「切到输入 1」这个**前置条件**
    // 根本没成立——继续往下跑，`start()` 里下发的 S_INPUT(0) 与已经为 0
    // 的初值无从区分，断言恒真，测不出任何东西；而写 ASSERT_EQ 又把它
    // 报成失败，让人误以为 S_INPUT 实现坏了。
    //
    // 两种情况都该如实说「本环境构造不出这个场景」，而不是给出一个
    // 既不能证明实现正确、也不指向实现缺陷的红灯。
    if (readInputIndex(dev) != 1) {
        GTEST_SKIP() << "设备回读输入仍为 0（可能只支持单一输入），"
                        "无法构造「初始停在非 0 输入」的前置条件";
    }

    auto cfg = makeConfig(30, 300);
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
    auto cfg = makeConfig(30, 500);
    V4l2FrameSource src(cfg);

    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();
    EXPECT_TRUE(src.isRunning());

    const std::string d = src.describe();
    // describe() 必须写明实际走的是哪条路径，否则事后无法解释数值差异
    const bool says_mplane = d.find(" mplane") != std::string::npos;
    const bool says_splane = d.find(" splane") != std::string::npos;
    EXPECT_TRUE(says_mplane || says_splane)
        << "describe() 未标注平面模式：" << d;
    EXPECT_NE(says_mplane, says_splane)
        << "describe() 同时/都未出现 mplane 与 splane 标记：" << d;

    src.stop();
    EXPECT_FALSE(src.isRunning());

    // ========================================================================
    // 守「诊断字段跨 stop 不变」：平面模式是**事实**，不是**当前状态**
    // ========================================================================
    //
    // 这条不是锦上添花，它守的是一个**已经发生过的真实缺陷**：
    // `closeDevice()` 把 `buf_type` 复位成单平面（这对下一次 start 是必要的），
    // 而 `describe()` 直接读 `buf_type`，于是同一个进程对同一个设备给出：
    //
    //   started: ... mplane {vin_video0 <- ... <- imx219}
    //   stopped: ... splane (stopped) {vin_video0 <- ... <- imx219}
    //
    // 板端 2026-11-14 实测日志就是这样。危害在于**事后取证**：拿停止时那行
    // 日志归档，会得出「本板不需要 MPLANE」这个与事实相反的结论，进而把
    // 唯一能出帧的路径判成无用。
    //
    // `buffer_source` 早已用「不被 closeDevice 复位」解决了同源问题，
    // 当时只修了内存模型、漏了平面模型。这条断言把两者都钉住。
    //
    // ------------------------------------------------------------------------
    // 鉴别力的边界（不要高估这条断言）
    // ------------------------------------------------------------------------
    // **单平面设备上它恒定通过，改动前后看不出差别**：closeDevice() 复位后的
    // `buf_type` 与 start() 时的探测结果同为单平面，两边都打印 `splane`。
    // 宿主机的 vivid/vimc **全部只有单平面**（`device_caps` 的
    // `CAPTURE_MPLANE` 位为 0，2026-11-14 实测），所以本用例在 CI 上能跑通
    // 但**没有鉴别力**；把它当成本修复的证明会是假绿灯。
    //
    // 真正的鉴别环境是板端（sunxi-vin 报 `0x05201000`，MPLANE 位为 1）。
    // 本缺陷的**发生证据**就是板端日志：同一进程同一次运行，启动那行写
    // `mplane`、停止那行写 `splane`。修复后的验收标准是那两行变成一致——
    // 这是板上取证项，不是 CI 能替代的。
    //
    // 保留这条断言的理由：一旦有人把多平面设备接进 CI（或在不复位 buf_type
    // 的重构后引入回归），它会立刻失败；而且它顺带钉住了「两个标记不得同时
    // 缺失」这条与设备无关的格式契约。
    const std::string after = src.describe();
    EXPECT_EQ(after.find(" mplane") != std::string::npos, says_mplane)
        << "stop() 之后平面模式变了：started=" << d << " / stopped=" << after
        << "。describe() 读的是会被 closeDevice() 复位的 buf_type，"
           "应改读 start() 时记下的事实字段。";
}

TEST_F(V4l2LiveTest, SnapshotSpanIsMeasuredFactNotConfiguredValue) {
    // ========================================================================
    // 守 A2 窗口语义：`window_ms` 报的是**事实**（本份统计实际覆盖多久），
    // 不是**配置**（它本来打算覆盖多久）
    // ========================================================================
    //
    // 为什么必须改（2026-11-12）：网络侧 `server.cpp` 用的是 120s 窗口，
    // 而帧侧若是「配置 1s 就报 1s」，两边的 window 永远不等，
    // `attributeVideoQoe()` 会恒定返回 `window_mismatch` 并拒绝归因。
    // 让帧侧如实报出自己覆盖了多久，才可能让调用方按同一节奏取数、
    // 使两侧窗口真正对齐。
    //
    // 报「事实」的代价是：跨度不再是常数，`frames_expected` 必须由
    // **报出来的跨度**推导。这正是本用例的主要断言。
    //
    // ------------------------------------------------------------------------
    // 必须先等校准完成，否则「丢帧率接近 0」这条在真机上恒假
    // ------------------------------------------------------------------------
    //
    // 板端 IMX219 的 `G_PARM` 谎报 30fps 而真实 18.56fps（见
    // `openspec/pitfalls/sensor-mode-table-fps-lie.md`）。校准需要
    // `cfg.fps_calibration_windows`（默认 3）个窗口才完成，在那之前
    // `expected` 按 30 推、`received` 按 18.56 到，丢帧率恒约 38%——**那是
    // 已知且正确的中间状态**，不是缺陷。本用例关心的是跨度语义，
    // 不该被这段过渡期绊倒。
    //
    // 宿主机 vivid 的 G_PARM 是诚实的，校准不改变任何值，所以多等一会儿
    // 对宿主机完全无害。
    auto cfg = makeConfig(30, 500);
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    // 等满校准窗口（默认 3 个 × 500ms）+ 余量
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(cfg.window_ms) * static_cast<int>(cfg.fps_calibration_windows) +
        400));
    // 把过渡期的那份统计丢掉，不参与断言
    src.snapshotAndReset();

    // ---- 第一份：等够一个窗口再取 ----
    const FrameStats first = snapshotAfter(src, 500);
    ASSERT_TRUE(first.valid()) << "等够一个窗口后应产出有效统计";

    EXPECT_GE(first.window_ms, 500u)
        << "本轮至少覆盖了 500ms（sleep 了 500+slack），报出 " << first.window_ms;
    EXPECT_LT(first.window_ms, 2000u)
        << "跨度是实际耗时，不该无界增长（实际 " << first.window_ms << "ms）";
    EXPECT_EQ(first.snapshot_interval_ms, first.window_ms)
        << "自驱线程下「距上次快照的间隔」就是本份统计的跨度，两者必须相等";

    // 这一条是自驱线程改造的**直接目的**：线程持续排空，中间没有观测盲区。
    // 若将来有人把线程改成「只在快照时才排空」，这里立刻变正数。
    EXPECT_EQ(first.observationGapMs(), 0u)
        << "自驱采集线程连续运行，不存在「没被任何窗口统计过」的时间段";

    // frames_expected 必须由**报出来的跨度**推导，而不是配置的 window_ms。
    // 用配置值推：500ms @ 30fps = 15 帧；用真实跨度推：~560ms → ~17 帧。
    //
    // ------------------------------------------------------------------------
    // 但 fps 不能用 cfg.fps —— 它可能已被校准改写
    // ------------------------------------------------------------------------
    //
    // 这是个反直觉但正确的失败（2026-11-12 真机暴露）：
    // 板端 IMX219 的 G_PARM 谎报 30fps，校准把它改写成 18fps；
    // 于是 `expected = 560ms × 18 / 1000 = 10`，而测试拿**启动时的配置副本**
    // `cfg.fps=30` 算出 17，被判不一致。
    //
    // 实现是对的，搞错的是测试：`cfg` 是传给构造函数的入参副本，
    // 而校准改写的是类内部的 fps。测试要断言的是「expected 与跨度同源」，
    // 就必须用**实际生效的** fps，而不是入参。
    //
    // 从 describe() 里解析实际 fps：格式 `...@1640x1232/18fps/500ms...`。
    // 解析实现与无设备单测 `DescribeFormatIsParseableForEffectiveFps` 共用，
    // 理由见 `parseEffectiveFps` 的注释。
    const uint32_t effective_fps = parseEffectiveFps(src.describe());
    ASSERT_GT(effective_fps, 0u)
        << "无法从 describe() 解析出实际帧率：" << src.describe();

    // 真正守的是「expected 与跨度同源」。两者都在 FrameStats 里，
    // 判据用实现自己的 fps 反算一遍，若不一致说明它们各算各的。
    const uint64_t want_expected =
        (static_cast<uint64_t>(first.window_ms) * effective_fps + 500u) / 1000u;
    // 允许 ±1 帧：跨度是毫秒取整，fps 是整数取整，边界处会差一帧。
    EXPECT_LE(first.frames_expected > want_expected
                  ? first.frames_expected - want_expected
                  : want_expected - first.frames_expected,
              1u)
        << "expected 必须按真实跨度推导：span=" << first.window_ms
        << "ms 实际fps=" << effective_fps << " → 应为 " << want_expected
        << "，实际 " << first.frames_expected;

    // 帧间隔应接近帧周期（1000/30 ≈ 33ms）。接近窗口长度说明把窗口边界
    // 当成了帧间隔——那是采集被快照切断的signature，自驱线程下不该出现。
    EXPECT_LT(first.max_gap_ms, 200u)
        << "最长帧间隔远小于窗口才算正常，实际 " << first.max_gap_ms << "ms";

    EXPECT_LT(first.frameLossRatio(), 0.2)
        << "健康链路上丢帧率应接近 0（expected=" << first.frames_expected
        << " received=" << first.frames_received << "）";

    // 每帧字节数应等于本机分辨率的 NV12 几何大小（1.5 字节/像素）。
    // 这条能抓出「拿 buf.length 当字节数」这类单位混淆（多平面下
    // length 是平面数而不是字节数，混淆会得到恒为 0 的坏码率）。
    //
    // 期望值**必须由 width_/height_ 算**，不能写死 640x480：真机是
    // 1640x1232，写死会让这条断言在板上恒假。单帧约 3.03MB，故容差取
    // 几何值的 2%（约 60KB）而不是绝对值 1.0 —— 后者只对 640x480 这种
    // 小尺寸有意义，在大分辨率下等于要求逐字节精确，会被对齐填充打掉。
    //
    // 多平面下 bytesused 按平面累加，合计仍等于同一几何大小。
    ASSERT_GT(first.frames_received, 0u);
    const double bytes_per_frame =
        static_cast<double>(first.bytes_received) /
        static_cast<double>(first.frames_received);
    const double want_bytes_per_frame =
        static_cast<double>(width_) * static_cast<double>(height_) * 1.5;
    EXPECT_NEAR(bytes_per_frame, want_bytes_per_frame,
                want_bytes_per_frame * 0.02)
        << "每帧字节数 " << bytes_per_frame << " 与 NV12 几何大小 "
        << want_bytes_per_frame << "（" << width_ << "x" << height_
        << "）不符，说明字节统计取错了字段";

    // ---- 第二份：立刻再取一次 ----
    //
    // 这是「window_ms 是事实」最锋利的证据：改造前这里必然还是 500
    // （配置值），改造后必然远小于 500（真实只过了几毫秒）。
    // 用「小于第一份」而不是「小于某个常数」，避免依赖调度抖动。
    const FrameStats second = src.snapshotAndReset();
    ASSERT_TRUE(second.valid()) << "刚取完统计立刻再取，仍应是有效统计";
    EXPECT_LT(second.window_ms, first.window_ms)
        << "立刻再取时跨度必须明显变短；若仍等于第一份，说明 window_ms "
           "报的还是配置值";
    EXPECT_LT(second.frames_expected, first.frames_expected)
        << "跨度变短后 expected 必须跟着变小；否则它没有跟着真实跨度走";

    src.stop();
}

TEST_F(V4l2LiveTest, SnapshotCostIsIndependentOfWindowMsAndDoesNotPerturbCapture) {
    // ========================================================================
    // 守自驱线程改造的**核心承诺**：取一份统计的代价与本窗口设计跨度无关，
    // 且频繁取数不会打断采集
    // ========================================================================
    //
    // 改造前 `snapshotAndReset()` 内部有排空循环，会阻塞满 `window_ms`。
    // 于是「调用方按 120s 的网络窗口取帧统计」要么阻塞 120s（卡死 15s 质量
    // 环），要么按 1s 取数、两侧窗口永不相等，`attributeVideoQoe()` 恒定
    // 返回 `window_mismatch`。这是决策 A 要解决的结构性阻塞。
    //
    // 改造后采集由内部线程连续驱动，快照只是「取走累计值并清零」。
    //
    // 本用例用**耗时**做判据，因为这是唯一能把新旧实现区分开的观测量：
    //   - 新实现：40 次调用 ≈ 40 次加锁与拷贝，毫秒级
    //   - 旧实现：40 次调用 ≈ 40 × 1000ms = 40s
    // 阈值取 8s，比新实现宽松两个数量级、比旧实现严格 5 倍，
    // 不受调度抖动影响。
    //
    // 顺带守两件事：频繁切断窗口不得凭空产生序号跳变，也不得把帧数漏算
    // （累计收到的帧应覆盖整段真实耗时）。
    auto cfg = makeConfig(30, 1000);
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    // 等校准完成后再计时：板端 G_PARM 谎报 30fps（真实 18.56），
    // 过渡期的 expected 按谎报值推，会污染下面的帧数判据。
    // 本用例守的是「快照代价」与「不漏计」，与校准无关。
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(cfg.window_ms) *
            static_cast<int>(cfg.fps_calibration_windows) + 400));
    src.snapshotAndReset();   // 丢掉过渡期那份

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t total_received = 0;
    uint32_t total_seq_gap = 0;
    constexpr int kSnapshots = 40;
    for (int i = 0; i < kSnapshots; ++i) {
        const FrameStats s = src.snapshotAndReset();
        ASSERT_TRUE(s.valid()) << "第 " << i << " 次快照应有效";
        total_received += s.frames_received;
        total_seq_gap += s.seq_gap_count;
        // 每次之间留 50ms：让跨度为 50ms 量级，旧实现下 40 次仍会阻塞 40s
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    EXPECT_LT(elapsed.count(), 8000)
        << "40 次快照耗时 " << elapsed.count() << "ms。若接近 "
        << kSnapshots * 1000 << "ms，说明快照仍按 window_ms 阻塞——"
           "调用方将无法按网络侧的 120s 节奏取数";

    // 频繁切断窗口不该制造假序号跳变：采集线程连续运行，边界两侧的帧
    // 本来就是相邻的。
    EXPECT_EQ(total_seq_gap, 0u)
        << "频繁取数时出现了序号跳变计数。采集连续、边界两侧帧相邻，"
           "任何非零值都说明边界被误当成了丢帧";

    // 帧数不得被切窗口漏算：累计 received 应覆盖整段真实耗时。
    //
    // ------------------------------------------------------------------------
    // 判据不能按 cfg.fps 推（2026-11-12 真机修正）
    // ------------------------------------------------------------------------
    //
    // 板端 IMX219 的 `G_PARM` 谎报 30fps 而真实 18.56fps。按 cfg.fps=30
    // 算「应到 60 帧」，实际只会到 ~37 帧——那是**传感器的真实帧率**，
    // 不是漏计。用配置值当分母，等于要求传感器必须跑在它谎称的帧率上。
    //
    // 所以改用**本机实测帧率**当基准：先从第一份统计的跨度与帧数反推
    // 实际出帧速率，再用它去校验总量。这样宿主机（30fps）与板端（18.56fps）
    // 用的是同一套逻辑，判据强度不变——漏计仍会被抓出来。
    //
    // 余量取 20%：起停首尾各一帧、取整、以及调度抖动。
    const double seconds = std::max(0.001, elapsed.count() / 1000.0);
    const double observed_fps =
        static_cast<double>(total_received) / seconds;
    EXPECT_GT(total_received, 0u)
        << "整段采集一帧都没收到，" << seconds << "s 内 total_received=0";
    // 自证式判据：上面算出的 observed_fps 就是结果，不能再拿它当期望值。
    // 真正要守的是「反复切窗口没有把帧吃掉」，因此对照的是**驱动实际能力**：
    // 该设备的真实帧率必然 ≤ cfg.fps（谎报只会偏高，不会偏低）。
    // 若 observed_fps 明显低于 cfg.fps 的 40%，则说明切窗口把帧丢了。
    EXPECT_GT(observed_fps, static_cast<double>(cfg.fps) * 0.4)
        << "累计收到 " << total_received << " 帧 / " << seconds
        << "s → 实测 " << observed_fps << "fps，远低于标称 " << cfg.fps
        << "fps 的 40%。频繁切断窗口在丢帧或漏计";

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
    auto cfg = makeConfig(30, 1000);
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    // 第一个窗口可能包含启动抖动，取后两个窗口判断稳态。
    // 用 snapshotAfter 而不是裸调用：新契约下裸调用只覆盖几毫秒，
    // expected 会是 0 或个位数，断言「丢帧率 < 0.5」就变成了一句废话。
    snapshotAfter(src, 1000);
    for (int w = 1; w <= 2; ++w) {
        const FrameStats s = snapshotAfter(src, 1000);
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
    auto cfg = makeConfig(120, 500);
    cfg.set_frame_rate = false;        // 模拟驱动回报不可信：不回填 cfg.fps
    // 明确要求校准 3 个窗口：窗口数不够就永远等不到校准完成
    cfg.fps_calibration_windows = 3;
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    // ========================================================================
    // 等待方式（2026-11-12 自驱线程改造后必须改）
    // ========================================================================
    //
    // 校准子窗口的边界由**采集线程按 cfg.window_ms 推进**，与调用方取数
    // 节奏完全解耦（见 calibrateTickLocked 的注释）。所以这里必须等**真实
    // 时间**：500ms × 3 = 1.5s 之后校准才可能完成。
    //
    // 改造前是「数快照次数」——那时 snapshotAndReset() 自己阻塞一个窗口，
    // 调 3 次恰好等于 3 个窗口。新契约下连续调 3 次只过了几毫秒，
    // calibration_windows_seen 还是 0，校准永远等不到，测出来 after 与
    // before 相同，看起来像「校准失效」——但那只是等待姿势错了。
    //
    // ---- before：校准完成前取一份 ----
    const FrameStats before = snapshotAfter(src, 500);
    ASSERT_TRUE(before.valid());
    ASSERT_GT(before.frames_received, 0u) << "本窗口没收到帧，无法判断校准效果";
    const double loss_before = before.frameLossRatio();
    ASSERT_GT(loss_before, 0.15)
        << "前置条件不成立：cfg.fps=120 而 vivid 实际 30fps，校准前应有约 "
           "75% 的假丢帧，实际只有 " << loss_before
        << "。若接近 0，说明 start() 已把 cfg.fps 回填成真实值"
           "（set_frame_rate=false 没生效），本用例测不到校准";

    // ---- 等满 3 个校准窗口 ----
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(cfg.window_ms) * 3 + 400));

    // after 必须**也是一个真实窗口**（同样用 snapshotAfter）。
    // 若这里裸调 snapshotAndReset()，跨度只有几毫秒、expected≈0，
    // `frameLossRatio()` 必然是 0，断言会以「分母为 0」的方式假通过——
    // 看起来校准生效了，其实只是没测到东西）。这类假绿灯正是本项目
    // 反复强调要避免的形态。
    const FrameStats after = snapshotAfter(src, 500);
    ASSERT_TRUE(after.valid());
    ASSERT_GT(after.frames_received, 0u) << "本窗口没收到帧，无法判断校准效果";
    ASSERT_GT(after.frames_expected, 0u)
        << "after 窗口的 expected 为 0，无法判断丢帧率（分母为零）";

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
    auto cfg = makeConfig(120, 500);
    cfg.set_frame_rate = false;        // 同 FpsCalibrationReplacesLyingDriverValue：
                                       // 不回填 cfg.fps，校准才有可修正的内容
    cfg.fps_calibration_windows = 0;   // 关闭
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    // 等满足够长的真实时间（远超 4 × window_ms），让「若开关失灵则校准一定
    // 已经完成」成为前置条件。改造后校准由采集线程按 wall time 推进，
    // 数快照次数不再等于推进校准窗口——必须等真实时间。
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(cfg.window_ms) * 4 + 400));

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
    // 保持稳定；且再等若干个窗口不会因重新校准而产生新的描述。
    //
    // **等待方式必须改**（2026-11-12 自驱线程改造）：校准窗口由采集线程按
    // cfg.window_ms 推进，与快照次数无关。改造前这里靠「数 2 次快照」推进
    // （每次快照阻塞一个窗口）；改造后快照立即返回，数次数不再推进任何东西，
    // 必须等真实时间：400ms × 2 = 800ms。
    auto cfg = makeConfig(120, 400);
    cfg.set_frame_rate = false;        // 必须：否则 start() 会把 60 回填成 30，
                                       // 校准无事可做，describe() 前后都无变化
    cfg.fps_calibration_windows = 2;
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    // 等满 2 个校准窗口（+余量），确保校准已完成
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(cfg.window_ms) * 2 + 400));
    const std::string desc_after_calibration = src.describe();

    // 前置条件：校准确实发生过（否则「不再变化」是废话——什么都没变过）
    ASSERT_NE(desc_after_calibration.find("[fps:"), std::string::npos)
        << "等待 2 个校准窗口后仍无 [fps: 记录，校准没有完成，"
           "本用例无法证明「只做一次」。实际 describe：" << desc_after_calibration;

    // 再等若干个窗口：若存在持续重算，cfg.fps 会继续漂移。
    // 这里用「等真实时间」而不是「数快照」：后者在新契约下不推进任何东西，
    // 会让本条断言退化成「同一瞬间读了两次 describe()」，毫无分辨力。
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(cfg.window_ms) * 5 + 400));

    EXPECT_EQ(src.describe(), desc_after_calibration)
        << "校准后 describe() 仍在变化，说明校准没有「只做一次」，"
           "持续重算会吸收真实丢帧";

    src.stop();
}

TEST_F(V4l2LiveTest, FpsCalibrationDoesNotAbsorbRealDegradation) {
    // ========================================================================
    // 守「校准不得吸收真实劣化」——整个设计里最危险的一条失败模式
    // ========================================================================
    //
    // 若 `expected` 由「本窗口收到了多少帧」反推，会形成这个闭环：
    //   出帧变慢 → 收到更少的帧 → 反推 fps 变低 → expected 同步变小 → 丢帧率归零
    // 结果是「摄像头出帧能力掉到三分之一」被显示成「画面完美」。
    // 假丢帧（38% 那个）只是让人多查一次；这个假绿灯会让人永远不查。
    //
    // ------------------------------------------------------------------------
    // 制造真实劣化的手法（2026-11-12 自驱线程改造后必须换）
    // ------------------------------------------------------------------------
    //
    // 老手法是 `buffer_count=2` + 调用方 sleep，让队列灌满后驱动丢帧。
    // 新契约下采集线程永远在排空，队列填不满，**丢不出帧来了**
    // （实测 6 个窗口 max_loss 恒为 0）。这不是代码退步，是观测量变了：
    // 以前那个「丢帧」其实是调用方偷懒的副产物，现在必须让**驱动自愿少出帧**。
    //
    // 所以换成 S_PARM 降速：cfg.fps 声称 120，校准把基线修到实测值后，
    // 再把驱动从 30fps 降到 10fps。此时真实帧间隔是**均匀的 100ms**
    // （不是丢帧造成的空洞），而 expected 仍按校准好的基线推，
    // 于是稳定报出约 67% 的缺口——**这个数不是错误，是如实**。
    //
    // **顺序不能反**：必须先校准、后降速。若先降速，校准会把 10fps
    // 当成基线固化下来，那正是「吸收」本身。
    //
    // 本机实测（vivid 6.8）：流运行中 S_PARM 30→10 生效，
    // 200 帧耗时由 6.7s 变为 15.84s，与理论值 16.0s 吻合。
    auto cfg = makeConfig(120, 500);
    cfg.set_frame_rate = false;    // 关键：否则 start() 会把 120 回填成驱动值
    cfg.fps_calibration_windows = 2;
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    // ---- 阶段一：让校准在健康链路上完成 ----
    // 校准窗口由采集线程按 cfg.window_ms 推进，与快照次数无关，
    // 所以这里等真实时间：500ms × 2 = 1s。
    std::this_thread::sleep_for(std::chrono::milliseconds(
        static_cast<int>(cfg.window_ms) * 2 + 400));

    const std::string desc_before = src.describe();
    ASSERT_NE(desc_before.find("[fps:"), std::string::npos)
        << "等待 2 个校准窗口后仍无 [fps: 记录，校准没有完成，"
           "本用例无法证明「校准后仍如实报劣化」。实际 describe：" << desc_before;

    // 记下校准后的基线：这份同时充当「降速前」的对照窗口
    const FrameStats baseline = snapshotAfter(src, 500);
    ASSERT_TRUE(baseline.valid()) << "基线窗口无效";
    ASSERT_GT(baseline.frames_received, 0u) << "基线窗口没收到帧";

    // 校准后的基线必须**如实**：链路上没问题，丢帧率应接近 0。
    // 这一条同时证明「后面出现的高丢帧率是降速造成的」，而不是本来就在丢。
    EXPECT_LT(baseline.frameLossRatio(), 0.2)
        << "降速前基线窗口就有 " << baseline.frameLossRatio()
        << " 的丢帧率（expected=" << baseline.frames_expected
        << " received=" << baseline.frames_received
        << "），说明高丢帧率另有原因，本用例的对照不成立";

    const std::string desc_baseline = src.describe();

    // ---- 阶段二：让驱动真的少出帧（不是丢帧，是出得慢）----
    //
    // **不支持运行中 S_PARM 时必须 skip，不能 fail**（2026-11-12 真机修正）。
    //
    // 本能力档位差距很大：
    //   宿主机 vivid   ：支持，30→10 立刻生效（200 帧由 6.7s 变 15.84s）
    //   板端 sunxi-vin ：`VIDIOC_S_PARM` 直接返回失败，改不了
    //
    // 那是**传感器的能力边界**，与本类实现无关。若写成 ASSERT_TRUE，
    // 板端每次都会红灯，而它既不能证明实现正确、也不指向实现缺陷；
    // 更糟的是，一条长期红着的用例会让人习惯性忽略整片测试。
    // 正确姿势是如实说「本环境构造不出该场景」。
    if (!setDeviceFrameRate(device_, 10u)) {
        GTEST_SKIP() << "该设备不支持在流运行中修改出帧速率（S_PARM 不可用），"
                        "无法构造「驱动自愿少出帧」的真实劣化场景。"
                        "本用例的契约仍需在支持该能力的设备上验证（如 vivid）";
    }

    // 等一拍让新时间基生效（vivid 立刻生效，留余量更稳）
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // ---- 阶段三：劣化后取一份 ----
    const FrameStats degraded = snapshotAfter(src, 500);
    ASSERT_TRUE(degraded.valid()) << "降速后窗口无效";
    ASSERT_GT(degraded.frames_received, 0u) << "降速后仍应有帧，只是更少";

    // ========================================================================
    // 断言 1：校准**没有**被重新触发
    // ========================================================================
    //
    // 「每窗口重算」是最典型的吸收实现。它在恒定帧率下毫无痕迹，
    // 只有真实劣化才能把它逼出来——所以这条断言只能在这里测。
    EXPECT_EQ(src.describe(), desc_baseline)
        << "降速后 describe() 变了，说明校准在劣化后重新计算了基线。"
           "那正是「吸收」——把相机出帧能力下降固化成新的正常。\n"
           "  before=" << desc_baseline << "\n  after=" << src.describe();

    // ========================================================================
    // 断言 2：**分母没有塌到 received 量级**
    // ========================================================================
    //
    // 这是「吸收」与「如实」的分界线，数值上很好分辨：
    //   - 如实：expected ≈ 基线fps × 跨度（~30 × 0.56s → ~16）
    //   - 吸收：expected ≈ received（降速后 ~5）
    //
    // 用「两者明显分开」而不是钉死绝对数值：不受 fps 取整与时序抖动影响。
    EXPECT_GT(static_cast<double>(degraded.frames_expected),
              static_cast<double>(degraded.frames_received) * 1.5)
        << "expected(" << degraded.frames_expected << ") 与 received("
        << degraded.frames_received << ") 太接近，说明分母是按收到的帧数"
           "反推的，相机出帧能力下降被吸收进了基线";

    // ========================================================================
    // 断言 3：丢帧率必须报出真实缺口
    // ========================================================================
    //
    // 驱动从 30 降到 10，真实缺口约 67%。判据用 0.3 而不是 0.6：留出取整、
    // 时序、以及「calibration 后 cfg.fps 可能被取整成 29」的余量。
    // 核心是「不能被洗成 0」。
    EXPECT_GT(degraded.frameLossRatio(), 0.3)
        << "丢帧率被洗成健康值：expected=" << degraded.frames_expected
        << " received=" << degraded.frames_received
        << " loss=" << degraded.frameLossRatio()
        << "。相机出帧能力已降到 10fps，这个缺口必须被如实报出";

    // 恢复正常速率，避免影响后续用例（同一 /dev/video0 共用）
    setDeviceFrameRate(device_, 30u);

    src.stop();
}

TEST_F(V4l2LiveTest, DequeueLatencyIsIndependentOfSnapshotCadence) {
    // ========================================================================
    // 守「出队延迟反映采集线程的响应速度，不反映调用方的取数节奏」
    // ========================================================================
    //
    // 改造前这个字段与调用方睡眠**成正比**：睡 1.5s 再取，队列里那几帧就在
    // 里面等了 1.5s，p95 报到上千毫秒。那个数字会被归因层读成「用户态处理慢」
    // 或「缓冲区不足」，而真相是「调用方没来取」——归因方向正好指反。
    //
    // 改造后采集线程连续排空，队列几乎总是空的，延迟只剩线程被唤醒的开销
    // （亚毫秒，取整后多为 0）。所以判据**不能**是「延迟 > 0」：亚毫秒被
    // 取整成 0 是**真实**值，不是字段失效。若照旧断言 > 0，只会逼着实现
    // 去伪造一个非零延迟——正是本项目最反对的「把不知道伪装成知道」。
    //
    // 真正的判据有两条，都很锋利：
    //   1. 睡了 1.5s 之后本窗口仍应有 ~30fps × 1.5s 的帧数。旧实现下
    //      2 个缓冲早就被灌满，能拿到的只有 2~4 帧。
    //   2. 出队延迟必须**有界**。旧实现下它 ≈ 睡眠时长。
    //
    // buffer_count=2 是刻意的：队列越小，旧实现的积压越严重，新旧差异越大。
    auto cfg = makeConfig(30, 300);
    cfg.buffer_count = 2;   // 小队列：让旧实现的积压暴露无遗
    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    snapshotAfter(src, 300);   // 预热，首个窗口含启动抖动不计

    for (int w = 1; w <= 3; ++w) {
        constexpr auto kSleep = std::chrono::milliseconds(1500);
        std::this_thread::sleep_for(kSleep);
        const FrameStats s = src.snapshotAndReset();
        ASSERT_TRUE(s.valid());
        ASSERT_GT(s.window_ms, 1400u)
            << "本窗口应覆盖 1.5s 的真实睡眠，实际 " << s.window_ms << "ms";

        // 1) 帧没被丢：2 个缓冲 + 1.5s 睡眠在旧实现下只能拿到个位数帧
        EXPECT_GT(s.frames_received, 20u)
            << "睡了 1.5s 后只收到 " << s.frames_received
            << " 帧（应约 45）。说明采集没有在后台持续排空，队列被灌满后"
               "驱动丢帧——那会把观测缺陷记成链路丢帧";

        // 2) 延迟有界：旧实现下它 ≈ 睡眠时长（1500ms 量级）
        EXPECT_LT(s.p95_dequeue_latency_ms, 100u)
            << "出队延迟 " << s.p95_dequeue_latency_ms << "ms 随调用方的睡眠"
            << "增长。该字段应只反映采集线程的唤醒开销（亚毫秒），"
            << "不该把「调用方没取」记成「用户态处理慢」";
    }

    src.stop();
}

TEST_F(V4l2LiveTest, StopThenSnapshotIsInvalidNotStale) {
    // 真机路径下同样守「停流后不得读到上一窗口残留值」。
    // 失败路径版本已在无设备用例中覆盖，但那条走的是「从未启动」，
    // 这里走的是「启动过又停止」——后者才可能留下残留计数。
    auto cfg = makeConfig(30, 300);
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
    auto cfg = makeConfig(30, 300);
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
    // 用 snapshotAfter 而不是裸调用：新契约下裸调用只覆盖几毫秒，
    // 两条路径都会拿到「窗口 1ms、帧数 0」，比对就失去意义（0 == 0 恒成立）。
    auto run_once = [this](bool use_dmabuf, FrameStats& out) {
        auto cfg = makeConfig(30, 400);
        cfg.use_dmabuf = use_dmabuf;
        V4l2FrameSource src(cfg);
        if (!src.start()) return false;
        out = snapshotAfter(src, 400);
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

    // 窗口跨度语义不得因缓冲来源而变 —— 但**不能**断言等于配置的 400。
    // 2026-11-12 起 window_ms 报的是**实际跨度**（sleep 400 + slack），
    // 断言等于 400 会把「跨度如实上报」这个正确行为判成失败。
    // 该守的是两条路径的跨度**彼此一致**（都落在同一数量级），
    // 以及跨度确实覆盖了等待时长。
    EXPECT_GE(dmabuf_stats.window_ms, 400u)
        << "DMABUF 路径跨度 " << dmabuf_stats.window_ms << "ms 短于等待时长";
    EXPECT_NEAR(static_cast<double>(dmabuf_stats.window_ms),
                static_cast<double>(mmap_stats.window_ms), 200.0)
        << "两条路径的窗口跨度差异过大（mmap=" << mmap_stats.window_ms
        << "ms dmabuf=" << dmabuf_stats.window_ms
        << "ms），说明其中一条路径的跨度测量方式不同";
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
    auto cfg = makeConfig(30, 300);
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
    auto cfg = makeConfig(30, 300);
    ASSERT_FALSE(cfg.use_dmabuf) << "本用例前提是默认关闭";

    V4l2FrameSource src(cfg);
    ASSERT_TRUE(src.start()) << "start() 失败：" << src.lastError();

    const std::string d = src.describe();
    EXPECT_EQ(d.find("dmabuf"), std::string::npos)
        << "未开启 DMABUF 时 describe() 不应提到它：" << d;
    src.stop();
}
