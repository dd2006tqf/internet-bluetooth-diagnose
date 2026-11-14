/**
 * @file test_video_frame_hub_gtest.cpp
 * @brief 帧中枢的租约协议真值表（无摄像头依赖）
 *
 * 这份测试守的是「把像素交给别的进程」这条通路上那些**错了也不报错**的
 * 地方。它们不会崩，只会让面板显示旧画面、让画面撕裂、或让发布悄悄停摆：
 *
 *   1. 未 configure 的发布必须被记账 —— 否则"没画面"看起来像"没人要"
 *   2. 消费者持租约期间槽位不得被覆写 —— 否则面板看到撕裂画面
 *   3. 所有槽都被租用时要跳过并记账 —— 不能静默覆写，也不能假装成功
 *   4. 消费者崩溃必须超时回收 —— 否则发布永久停摆且无人知晓
 *   5. 句柄必须是 dup 出来的 —— 消费者 close 不能把中枢的 fd 一起废掉
 *   6. configure 期间的在途发布必须作废 —— 否则往已 munmap 的地址写
 *
 * 本文件不打开任何设备。dma-heap 不存在时自动回落 memfd，两条路都能跑，
 * 断言只依赖「拿到的是共享可读内存」，不依赖具体分配后端。
 */

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "video_frame_hub.hpp"

using namespace weaknet;

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 48;
constexpr uint32_t kStride = 64;
constexpr size_t kSize = static_cast<size_t>(kStride) * kH;

/// 不限速：绝大多数用例想验证的是租约语义，不是节流
VideoFrameHubConfig fastConfig() {
    VideoFrameHubConfig c;
    c.min_publish_interval_ms = 0;
    c.lease_timeout_ms = 0;   // 默认不回收，需要时单测自己打开
    return c;
}

std::vector<uint8_t> pattern(uint64_t seq, size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>((i * 37 + seq * 11 + 1) & 0xFF);
    }
    return v;
}

/// 把拿到的句柄读出来（不 close，由调用方负责）
std::vector<uint8_t> readLease(const VideoFrameLease& lease) {
    std::vector<uint8_t> out;
    if (!lease.valid()) return out;
    void* p = ::mmap(nullptr, lease.size, PROT_READ, MAP_SHARED, lease.fd, 0);
    if (p == MAP_FAILED) return out;
    out.resize(lease.size);
    std::memcpy(out.data(), p, lease.size);
    ::munmap(p, lease.size);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 缺数据语义
// ---------------------------------------------------------------------------

TEST(VideoFrameHub, PublishBeforeConfigureIsCountedNotSilent) {
    VideoFrameHub hub(fastConfig());
    std::vector<uint8_t> src(kSize, 0xAB);

    // 未 configure 时中枢不知道几何，理所当然不能拷。但必须记账：
    // 否则"没人 configure"和"没人 publish"在统计上无法区分。
    EXPECT_FALSE(hub.publish(src.data(), src.size(), 1, 100));

    const auto st = hub.stats();
    EXPECT_EQ(st.published, 0u);
    EXPECT_EQ(st.skipped_unconfigured, 1u);

    VideoFrameLease lease;
    EXPECT_FALSE(hub.acquire(lease));
    EXPECT_FALSE(lease.valid());
}

TEST(VideoFrameHub, AcquireBeforeFirstPublishReturnsFalse) {
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0x3231564E /*NV12*/, kSize));

    // 已经 configure 但一帧都没发布：最新槽位还不存在，不能交出去。
    // 交出去就是让消费者 mmap 一块未初始化内存当画面。
    VideoFrameLease lease;
    EXPECT_FALSE(hub.acquire(lease));
    EXPECT_FALSE(lease.valid());
}

TEST(VideoFrameHub, ConfigureRejectsZeroGeometry) {
    VideoFrameHub hub(fastConfig());
    EXPECT_FALSE(hub.configure(0, kH, kStride, 0, kSize));
    EXPECT_FALSE(hub.configure(kW, 0, kStride, 0, kSize));
    EXPECT_FALSE(hub.configure(kW, kH, kStride, 0, 0));
    EXPECT_EQ(hub.backend(), "none");
}

TEST(VideoFrameHub, UvOffsetSurvivesTheLeaseBoundary) {
    // ========================================================================
    // uv_offset 是**事实值**，必须原样跨过租约边界
    // ========================================================================
    //
    // 本板 sunxi-vin 的真实布局（2026-11-14 实测，1640x1232 NV12）：
    //   Y   [0, 2020480)          stride=1640
    //   空洞 [2020480, 2030336)    9856 字节，全零
    //   UV  [2030336, 3040576)    stride=1640
    //   尾部 [3040576, 3045504)    4928 字节，全零
    //
    // 中枢若把它丢掉（或按 stride*height 重算），消费者就会把对齐空洞
    // 当色度读——U=V=0，画面严重偏色且不报错。
    //
    // 用极小的比例复现同样的形状：stride==width，UV 偏移 > stride*height。
    constexpr uint32_t w = 8, h = 8;
    constexpr uint32_t stride = 8;
    constexpr uint32_t hole = 32;
    const size_t uv_off = static_cast<size_t>(stride) * h + hole;
    const size_t total = uv_off + static_cast<size_t>(stride) * (h / 2);

    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(w, h, stride, 0x3231564E, total,
                              static_cast<uint32_t>(uv_off)));

    std::vector<uint8_t> src(total, 0x33);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));

    VideoFrameLease lease;
    ASSERT_TRUE(hub.acquire(lease));
    EXPECT_EQ(lease.stride, stride);
    EXPECT_EQ(lease.uv_offset, uv_off)
        << "uv_offset 必须原样传出；丢了它下游就会把对齐空洞当色度";
    EXPECT_EQ(lease.size, total);

    hub.release(lease.slot);
    ::close(lease.fd);
}

TEST(VideoFrameHub, GeometryChangeOnUvOffsetRebuildsSlots) {
    // uv_offset 变化必须视为**几何变化**并重建槽位。
    //
    // 理由：uv_offset 参与「这块内存怎么解释」，只改它而不换缓冲，
    // 消费者会拿到新偏移配旧数据。复用判断里漏掉它，是一个只在
    // 「驱动改了对齐方式」时才炸的雷。
    VideoFrameHub hub(fastConfig());
    constexpr uint32_t w = 8, h = 8, stride = 8;
    const size_t total = static_cast<size_t>(stride) * h * 2;   // 留足空间

    ASSERT_TRUE(hub.configure(w, h, stride, 0x3231564E, total, 0));
    std::vector<uint8_t> src(total, 0x44);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));

    VideoFrameLease held;
    ASSERT_TRUE(hub.acquire(held));
    const uint32_t old_slot = held.slot;

    // 换一个 uv_offset：槽位应重建，旧数据不再可交付
    const uint32_t new_off = static_cast<uint32_t>(stride) * h + 16;
    ASSERT_TRUE(hub.configure(w, h, stride, 0x3231564E, total, new_off));

    VideoFrameLease fresh;
    EXPECT_FALSE(hub.acquire(fresh))
        << "换 uv_offset 后旧帧不能被当作新几何交付出去";

    ASSERT_TRUE(hub.publish(src.data(), src.size(), 2, 2));
    ASSERT_TRUE(hub.acquire(fresh));
    EXPECT_EQ(fresh.uv_offset, new_off);

    hub.release(old_slot);   // 旧槽已随重建消失，release 是幂等的
    hub.release(fresh.slot);
    ::close(held.fd);
    ::close(fresh.fd);
}

TEST(VideoFrameHub, DefaultUvOffsetIsZeroAndReported) {
    // 不传 uv_offset 的调用方（以及绝大多数既有单测）行为必须不变：
    // 偏移为 0，语义是「等于 stride*height」。
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0x3231564E, kSize));
    std::vector<uint8_t> src(kSize, 0x55);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));

    VideoFrameLease lease;
    ASSERT_TRUE(hub.acquire(lease));
    EXPECT_EQ(lease.uv_offset, 0u);
    hub.release(lease.slot);
    ::close(lease.fd);
}

TEST(VideoFrameHub, DescribeReportsUvOffset) {
    // describe() 是写进 evidence 的东西，几何必须完整可核。
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(16, 8, 16, 0x3231564E, 16 * 8 * 2, 400));
    const std::string d = hub.describe();
    EXPECT_NE(d.find("uvoff=400"), std::string::npos)
        << "describe 应带 uv 偏移，实际: " << d;
}

// ---------------------------------------------------------------------------
// 2. 交付正确性
// ---------------------------------------------------------------------------

TEST(VideoFrameHub, PublishedPixelsArriveIntactAcrossHandle) {
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0x3231564E, kSize));

    const auto src = pattern(7, kSize);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 7, 123456789));

    VideoFrameLease lease;
    ASSERT_TRUE(hub.acquire(lease));
    ASSERT_TRUE(lease.valid());

    // 几何必须原样带出：消费者用 width 当 stride 会得到逐行斜切的画面
    EXPECT_EQ(lease.width, kW);
    EXPECT_EQ(lease.height, kH);
    EXPECT_EQ(lease.stride, kStride);
    EXPECT_EQ(lease.size, kSize);
    EXPECT_EQ(lease.sequence, 7u);
    EXPECT_EQ(lease.timestamp_ns, 123456789u);

    const auto got = readLease(lease);
    ASSERT_EQ(got.size(), kSize);
    // 逐字节比对，不是抽样：拷贝越界/偏移一个字节都要被抓出来
    EXPECT_EQ(std::memcmp(got.data(), src.data(), kSize), 0);

    hub.release(lease.slot);
    ::close(lease.fd);
}

TEST(VideoFrameHub, BackendNameIsNeverSilentAboutFallback) {
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));

    const std::string b = hub.backend();
    // dma-heap 可用就是真 dma-buf（能交给编码器）；否则回落 memfd。
    // 两者都能跨进程 mmap，但能力不同。**必须**如实报出来，不许沉默。
    EXPECT_TRUE(b == "dma-heap" || b == "memfd") << "实际后端: " << b;
}

TEST(VideoFrameHub, ZeroStrideFallsBackToWidth) {
    VideoFrameHub hub(fastConfig());
    // 拿不到 stride 的驱动是存在的。按 width 兜底至少能出图（可能斜切）。
    ASSERT_TRUE(hub.configure(kW, kH, 0, 0, kSize));

    const auto src = pattern(1, kSize);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));

    VideoFrameLease lease;
    ASSERT_TRUE(hub.acquire(lease));
    EXPECT_EQ(lease.stride, kW);
    hub.release(lease.slot);
    ::close(lease.fd);
}

TEST(VideoFrameHub, ShortSourceIsTruncatedAndCounted) {
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));

    // 源比槽位短：只拷实际存在的部分，但必须记账。
    // 静默按槽位大小拷贝会把上次的残留像素留在尾部。
    std::vector<uint8_t> src(kSize / 2, 0x5A);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 2, 2));
    EXPECT_EQ(hub.stats().truncated, 1u);
}

TEST(VideoFrameHub, SegmentIsolatesConsumerClose) {
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));
    const auto src = pattern(3, kSize);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 3, 3));

    VideoFrameLease lease;
    ASSERT_TRUE(hub.acquire(lease));
    // 句柄是 dup 出来的：消费者关掉自己那份，中枢那份必须还活着，
    // 否则下一个消费者拿到的是一个已关闭的 fd 号。
    ::close(lease.fd);
    lease.fd = -1;

    VideoFrameLease second;
    ASSERT_TRUE(hub.acquire(second));
    const auto got = readLease(second);
    EXPECT_EQ(std::memcmp(got.data(), src.data(), kSize), 0);

    hub.release(lease.slot);
    hub.release(second.slot);
    ::close(second.fd);
}

// ---------------------------------------------------------------------------
// 3. 覆写保护（本项目最容易犯的错：观测缺陷栽给被观测对象）
// ---------------------------------------------------------------------------

TEST(VideoFrameHub, LeasedSlotIsNotOverwritten) {
    VideoFrameHubConfig cfg = fastConfig();
    cfg.slot_count = 3;
    VideoFrameHub hub(cfg);
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));

    const auto first = pattern(1, kSize);
    ASSERT_TRUE(hub.publish(first.data(), first.size(), 1, 1));

    VideoFrameLease held;
    ASSERT_TRUE(hub.acquire(held));

    // 消费者还没归还，中枢连着发三帧。三帧都必须绕开被租用的那个槽：
    // 覆写会让消费者读到「上半帧旧、下半帧新」的撕裂画面，而撕裂
    // 无法与解码错误区分。
    for (uint64_t seq = 2; seq <= 4; ++seq) {
        const auto next = pattern(seq, kSize);
        EXPECT_TRUE(hub.publish(next.data(), next.size(), seq, seq));
    }

    // 持有的那一帧内容必须一字未变
    const auto still = readLease(held);
    EXPECT_EQ(std::memcmp(still.data(), first.data(), kSize), 0);

    hub.release(held.slot);
    ::close(held.fd);
    EXPECT_EQ(hub.stats().skipped_busy, 0u);
}

TEST(VideoFrameHub, AllSlotsLeasedSkipsAndCounts) {
    VideoFrameHubConfig cfg = fastConfig();
    cfg.slot_count = 3;
    VideoFrameHub hub(cfg);
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));

    std::vector<uint8_t> src(kSize, 0x11);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));

    // 把三个槽全部租出去（acquire 总给 latest，所以每轮 release 前先
    // 把 latest 推到下一个槽）
    std::vector<VideoFrameLease> leases;
    std::vector<uint32_t> slots;
    for (int i = 0; i < 3; ++i) {
        VideoFrameLease l;
        ASSERT_TRUE(hub.acquire(l));
        slots.push_back(l.slot);
        leases.push_back(l);
        if (i < 2) {
            ASSERT_TRUE(hub.publish(src.data(), src.size(), 2 + i, 2 + i));
        }
    }

    // 三个槽都被持有：这一帧必须跳过，并且**不能**返回 true。
    // 返回 true 却什么都没写，会让上游以为画面在更新。
    EXPECT_FALSE(hub.publish(src.data(), src.size(), 9, 9));
    EXPECT_EQ(hub.stats().skipped_busy, 1u);

    for (auto& l : leases) {
        hub.release(l.slot);
        ::close(l.fd);
    }

    // 归还后立刻能再发
    EXPECT_TRUE(hub.publish(src.data(), src.size(), 10, 10));
}

TEST(VideoFrameHub, ReleaseIsIdempotent) {
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));
    std::vector<uint8_t> src(kSize, 0x22);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));

    VideoFrameLease l;
    ASSERT_TRUE(hub.acquire(l));
    hub.release(l.slot);
    hub.release(l.slot);      // 重复归还
    hub.release(UINT32_MAX);  // 未知槽位
    hub.release(9999);
    EXPECT_EQ(hub.stats().released, 1u);
    ::close(l.fd);
}

// ---------------------------------------------------------------------------
// 4. 消费者异常：超时回收
// ---------------------------------------------------------------------------

TEST(VideoFrameHub, StaleLeaseIsReclaimedSoPublishingNeverStalls) {
    VideoFrameHubConfig cfg = fastConfig();
    cfg.slot_count = 1;              // 只有一个槽，租约不还就彻底堵死
    cfg.lease_timeout_ms = 30;
    VideoFrameHub hub(cfg);
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));

    std::vector<uint8_t> src(kSize, 0x33);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));

    VideoFrameLease leaked;
    ASSERT_TRUE(hub.acquire(leaked));   // 模拟消费者崩溃，永不 release

    // 未超时：必须老老实实跳过，不能提前抢
    EXPECT_FALSE(hub.publish(src.data(), src.size(), 2, 2));
    EXPECT_EQ(hub.stats().skipped_busy, 1u);
    EXPECT_EQ(hub.stats().reclaimed, 0u);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // 超时后强制回收并记账。否则发布永久停摆，面板停在最后一帧，
    // 而且没有任何指标能说明为什么。
    EXPECT_TRUE(hub.publish(src.data(), src.size(), 3, 3));
    EXPECT_EQ(hub.stats().reclaimed, 1u);

    ::close(leaked.fd);
}

TEST(VideoFrameHub, ReclaimIsPerSlotNotGlobal) {
    // 这条是「按槽记录租约起始时刻」的判决性测试。
    //
    // 构造两个起始时刻相差很大的租约，并让它们落在「老的那个超时、
    // 新的那个还活着」的窗口里。此时两种实现的观测差异是可数的：
    //   - 按槽记录  → 只回收老的那个，reclaimed == 1
    //   - 全局一个戳 → 一旦判定过期就把所有租约一起清掉，reclaimed == 2
    //
    // 差这一次回收的后果是真实的面板黑屏：消费者刚拿到新帧，就因为
    // 另一个进程很久以前没还的租约被连坐回收，槽位随即被覆写。
    VideoFrameHubConfig cfg = fastConfig();
    cfg.slot_count = 2;
    cfg.lease_timeout_ms = 100;
    VideoFrameHub hub(cfg);
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));

    std::vector<uint8_t> src(kSize, 0x44);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));   // → slot0, latest=0

    VideoFrameLease old_lease;
    ASSERT_TRUE(hub.acquire(old_lease));                       // slot0 @t0

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // slot0 被占，这一帧必须落到 slot1，把 latest 推过去
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 2, 2));
    ASSERT_NE(old_lease.slot, UINT32_MAX);

    VideoFrameLease fresh;
    ASSERT_TRUE(hub.acquire(fresh));                           // slot1 @t1 = t0+80
    ASSERT_NE(fresh.slot, old_lease.slot);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    // 此刻 t0 已过 120ms（>100 超时），t1 只过了 40ms（<100 仍有效）

    const uint64_t before = hub.stats().reclaimed;
    const auto seq3 = pattern(3, kSize);
    EXPECT_TRUE(hub.publish(seq3.data(), seq3.size(), 3, 3));
    EXPECT_EQ(hub.stats().reclaimed - before, 1u)
        << "只应回收超时的那一个租约；回收了 2 个说明用的是全局时间戳";

    // 新租约保护的那一帧内容必须原封不动
    const auto still = readLease(fresh);
    EXPECT_EQ(std::memcmp(still.data(), src.data(), kSize), 0);

    hub.release(fresh.slot);
    hub.release(old_lease.slot);
    ::close(fresh.fd);
    ::close(old_lease.fd);
}

// ---------------------------------------------------------------------------
// 5. 限速
// ---------------------------------------------------------------------------

TEST(VideoFrameHub, PacingSkipsButIsNotAnError) {
    VideoFrameHubConfig cfg;
    cfg.min_publish_interval_ms = 100;   // 上限 10fps
    cfg.lease_timeout_ms = 0;
    VideoFrameHub hub(cfg);
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));

    std::vector<uint8_t> src(kSize, 0x55);
    EXPECT_TRUE(hub.publish(src.data(), src.size(), 1, 1));
    // 紧跟着的第二帧被节流：消费者不需要满帧率，白拷 3MB×18fps 纯属浪费
    EXPECT_FALSE(hub.publish(src.data(), src.size(), 2, 2));

    const auto st = hub.stats();
    EXPECT_EQ(st.published, 1u);
    EXPECT_EQ(st.skipped_rate, 1u);
    // 限速跳过不是故障，绝不能和 busy/unconfigured 混在一起
    EXPECT_EQ(st.skipped_busy, 0u);
    EXPECT_EQ(st.skipped_unconfigured, 0u);

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_TRUE(hub.publish(src.data(), src.size(), 3, 3));
    EXPECT_EQ(hub.stats().published, 2u);
}

TEST(VideoFrameHub, RateLimitDisabledPublishesEveryFrame) {
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));

    std::vector<uint8_t> src(kSize, 0x66);
    for (uint64_t i = 1; i <= 5; ++i) {
        ASSERT_TRUE(hub.publish(src.data(), src.size(), i, i));
    }
    EXPECT_EQ(hub.stats().published, 5u);
    EXPECT_EQ(hub.stats().skipped_rate, 0u);
}

// ---------------------------------------------------------------------------
// 6. 换格式 / 重建槽位
// ---------------------------------------------------------------------------

TEST(VideoFrameHub, SameGeometryDoesNotRebuildSlots) {
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));
    std::vector<uint8_t> src(kSize, 0x77);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));

    VideoFrameLease held;
    ASSERT_TRUE(hub.acquire(held));

    // 几何没变就复用槽位。若这里重建，held 这个在途租约会指向已释放内存。
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));
    const auto got = readLease(held);
    EXPECT_EQ(got.size(), kSize);
    EXPECT_EQ(got[0], 0x77);

    hub.release(held.slot);
    ::close(held.fd);
}

TEST(VideoFrameHub, GeometryChangeInvalidatesOldData) {
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));
    std::vector<uint8_t> src(kSize, 0x88);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));

    // 换分辨率后旧画面不能继续交付：宽高是新的，内存也是新的
    constexpr uint32_t w2 = 32, h2 = 16;
    constexpr size_t sz2 = static_cast<size_t>(w2) * h2;
    ASSERT_TRUE(hub.configure(w2, h2, w2, 0, sz2));

    VideoFrameLease lease;
    EXPECT_FALSE(hub.acquire(lease));

    std::vector<uint8_t> src2(sz2, 0x99);
    ASSERT_TRUE(hub.publish(src2.data(), src2.size(), 2, 2));
    ASSERT_TRUE(hub.acquire(lease));
    EXPECT_EQ(lease.width, w2);
    EXPECT_EQ(lease.height, h2);
    EXPECT_EQ(lease.size, sz2);
    hub.release(lease.slot);
    ::close(lease.fd);
}

namespace {

/// 序列号 → 64 位签章。用 splitmix64 的常量混合，任何相邻两帧都差得很远。
uint64_t signatureOf(uint64_t seq) {
    uint64_t z = seq + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// 位置 i 处、序列号 seq 的源帧应有的字节。
///
/// 用「签章的第 (i%8) 个字节」异或「位置乘子」构造：任意位置都能 O(1)
/// 直接算出来（验证时按 64 字节采样，不需要物化整帧），而每 8 字节一轮
/// 都不同，整片不会塌成同一个值。前一版对每个位置都算一次 splitmix64，
/// 2 MB × 300 轮把单测拖到 16 秒——那属于测试自身的开销。
uint8_t expectedByte(uint64_t seq, size_t i) {
    static constexpr uint8_t kMul[8] = {1, 3, 7, 13, 29, 61, 127, 251};
    const uint64_t sig = signatureOf(seq);
    const uint8_t base = static_cast<uint8_t>((sig >> ((i % 8) * 8)) & 0xFF);
    return static_cast<uint8_t>(base ^ static_cast<uint8_t>(i * kMul[i % 8]));
}

/// 帧指纹：**任意长度的前缀**都可独立校验。
///
/// 这一点是必需的，不是讲究。中枢在 `src_size > 槽位尺寸` 时只拷前缀
/// （`truncated` 记一条），所以「源尾部的标记字节」在槽位里根本不存在——
/// 上一版测试正是在这里误报，把一批合法截断判成了内容错乱。
///
/// 结构：
///   - 开头 8 字节放 64 位签章，序列号一变就完全不同（不依赖 mod 256 巧合）；
///   - 其余每个字节按「位置 + 序列号」共同决定，于是换一帧就整片不同。
void fillFingerprint(std::vector<uint8_t>& buf, uint64_t seq) {
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = expectedByte(seq, i);
    }
    if (buf.size() >= sizeof(uint64_t)) {
        const uint64_t sig = signatureOf(seq);
        std::memcpy(buf.data(), &sig, sizeof(sig));
    }
}

/// 交付出来的像素是否是「某个序列号源帧的前缀」
///
/// 采样步长 64：2 MB 逐字节比对会让单测跑到十几秒，而 64 字节一采样
/// 已经足以抓到整片错位（内容一旦来自别的帧，几乎每个采样点都不同）。
bool prefixBelongs(const std::vector<uint8_t>& got, uint64_t seq) {
    if (got.size() < sizeof(uint64_t)) return false;
    uint64_t have = 0;
    std::memcpy(&have, got.data(), sizeof(have));
    if (have != signatureOf(seq)) return false;

    for (size_t i = sizeof(uint64_t); i < got.size(); i += 64) {
        if (got[i] != expectedByte(seq, i)) return false;
    }
    // 末尾也查一个点：尾部错位不会在按步长采样里被漏掉
    if (got.size() > sizeof(uint64_t) &&
        got[got.size() - 1] != expectedByte(seq, got.size() - 1)) {
        return false;
    }
    return true;
}

}  // namespace

TEST(VideoFrameHub, DeliveredPixelsAlwaysBelongToTheirOwnSequence) {
    // 这条是「槽位代号守卫」的判决性测试，**判据不是「没崩」**。
    //
    // 不崩由 shared_ptr 保证（那是另一道防线：让 configure 在锁外 memcpy
    // 期间回收不掉内存）。代号守卫管的是另一件事：configure() 在 memcpy
    // 窗口内重建槽位后，本次发布必须作废。若不作废，中枢会把「标记为已
    // 发布」指向一块**从未被这次写入碰过**的新内存，于是消费者拿到一个
    // 前进的序号，配着空白或上一个尺度的残留画面。
    //
    // 这种错在面板上的表现是「画面偶尔卡一下」，没人会想到是中枢。
    // 所以用真实尺寸（2 MB）把 memcpy 窗口撑宽，让交叠真会发生，
    // 然后逐帧核对：拿到的内容必须属于它自己的 sequence。
    constexpr uint32_t bigW = 1640, bigH = 1232;
    const size_t bigSize = static_cast<size_t>(bigW) * bigH;
    constexpr uint32_t smallW = 8, smallH = 8;
    const size_t smallSize = static_cast<size_t>(smallW) * smallH;

    VideoFrameHubConfig cfg = fastConfig();
    cfg.slot_count = 3;
    cfg.use_dma_heap = false;   // 测试不依赖 /dev/dma_heap/system
    VideoFrameHub hub(cfg);
    ASSERT_TRUE(hub.configure(bigW, bigH, bigW, 0x3231564E, bigSize));

    std::vector<uint8_t> src(bigSize, 0);

    // ---- 阶段 A：无干扰，确认正常路径可用 ----
    //
    // 必须先有这一段。否则「并发下语义不变量从未被违反」可能只是
    // 「正常路径从未被执行」——那是最典型的假绿：测试通过，但它本来就
    // 检测不到任何东西。
    for (uint64_t seq = 1; seq <= 5; ++seq) {
        fillFingerprint(src, seq);
        ASSERT_TRUE(hub.publish(src.data(), src.size(), seq, seq));
        VideoFrameLease lease;
        ASSERT_TRUE(hub.acquire(lease));
        const auto got = readLease(lease);
        ASSERT_EQ(got.size(), bigSize);
        ASSERT_EQ(lease.sequence, seq);
        EXPECT_TRUE(prefixBelongs(got, seq)) << "阶段 A 正常路径内容错，seq=" << seq;
        hub.release(lease.slot);
        ::close(lease.fd);
    }

    // ---- 阶段 B：并发交叠 ----
    std::atomic<bool> stop{false};

    std::thread churn([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            // 反复在两种尺度之间重建：每次成功重建都会推进槽位代号，
            // 并把旧槽位从位图里摘掉。
            //
            // 这里刻意只改几何、不 sleep——重建本身（3 块 2 MB 的分配与
            // mmap）就足以把主循环的 memcpy 窗口覆盖住。真机上 configure
            // 只在换格式时调用，所以这种压力不是常态，而是为了把
            // 「锁外拷贝撞上重建」这个窗口放大到必然发生。
            hub.configure(smallW, smallH, smallW, 0, smallSize);
            hub.configure(bigW, bigH, bigW, 0x3231564E, bigSize);
        }
    });

    int checked = 0;
    for (uint64_t seq = 100; seq < 200; ++seq) {
        fillFingerprint(src, seq);
        const bool published = hub.publish(src.data(), src.size(), seq, seq);

        VideoFrameLease lease;
        if (!hub.acquire(lease)) continue;

        const auto got = readLease(lease);
        ASSERT_EQ(got.size(), lease.size);

        // 判据只有一条，且不依赖发布是否成功：**交付出来的像素必须是
        // 它所声称的那个 sequence 的前缀**。
        //
        // 这条覆盖两种失效：
        //   1. 发布成功但内容其实是更早/更晚那一帧（代号守卫缺失时，
        //      中枢会把「已发布」标在另一块内存上，序号往前走、像素没跟上）
        //   2. 内容来自别的尺度（configure 中途重建，几何与像素对不上）
        //
        // 注意它**不**要求 `published == true`：源比槽位大时只拷前缀是
        // 合法行为，所以用前缀校验而非全幅比对。
        EXPECT_TRUE(prefixBelongs(got, lease.sequence))
            << "交付内容的 sequence=" << lease.sequence << " 与实际像素不符"
            << " | published=" << published
            << " lease.size=" << lease.size
            << " bigSize=" << bigSize
            << " got[0]=" << int(got[0])
            << " want[0]=" << int(expectedByte(lease.sequence, 0));
        ++checked;

        hub.release(lease.slot);
        ::close(lease.fd);
    }

    stop.store(true);
    churn.join();

    // 阶段 B 必须真的取到过东西；否则并发那段什么也没验证。
    EXPECT_GT(checked, 0) << "阶段 B 一次都没能交付，并发交叠未被覆盖";
    // 交叠真的发生过：重建让至少部分发布被作废，或者槽位被占导致跳过。
    const auto st = hub.stats();
    EXPECT_GT(st.published, 0u) << "并发阶段一次都没发布成功，测试未覆盖目标";
}

TEST(VideoFrameHub, ConcurrentConfigureDuringPublishDoesNotCorruptMemory) {
    // 这条守的是 publish() 在**锁外** memcpy 的那个设计取舍：
    // 拷贝中途 configure() 会 munmap 掉 dst，提交时若不比对槽位代号，
    // 就会把 write 标记写进新位图里一块完全无关的缓冲。
    VideoFrameHubConfig cfg = fastConfig();
    cfg.slot_count = 3;
    VideoFrameHub hub(cfg);

    std::vector<uint8_t> a(kSize, 0xAA);
    std::vector<uint8_t> b(kSize, 0xBB);
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));

    std::atomic<bool> stop{false};
    std::thread churn([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            // 反复在大/小几何之间切换，制造 configure 与 publish 的交叠
            hub.configure(kW, kH, kStride, 0, kSize);
            hub.configure(8, 8, 8, 0, 64);
        }
    });

    for (int i = 0; i < 2000; ++i) {
        hub.configure(kW, kH, kStride, 0, kSize);
        hub.publish((i & 1) ? a.data() : b.data(), kSize,
                    static_cast<uint64_t>(i), static_cast<uint64_t>(i));
        VideoFrameLease l;
        if (hub.acquire(l)) {
            hub.release(l.slot);
            ::close(l.fd);
        }
    }
    stop.store(true);
    churn.join();

    // 没有崩、没有踩坏堆，就说明代号守卫生效。这里再收尾一次让它处于
    // 已知状态，后面的断言才有意义。
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));
    ASSERT_TRUE(hub.publish(a.data(), a.size(), 77, 77));
    VideoFrameLease l;
    ASSERT_TRUE(hub.acquire(l));
    const auto got = readLease(l);
    EXPECT_EQ(std::memcmp(got.data(), a.data(), kSize), 0);
    hub.release(l.slot);
    ::close(l.fd);
}

// ---------------------------------------------------------------------------
// 7. 观测
// ---------------------------------------------------------------------------

TEST(VideoFrameHub, DescribeReportsBackendGeometryAndCounters) {
    VideoFrameHub hub(fastConfig());
    ASSERT_TRUE(hub.configure(kW, kH, kStride, 0, kSize));
    std::vector<uint8_t> src(kSize, 0xCC);
    ASSERT_TRUE(hub.publish(src.data(), src.size(), 1, 1));
    VideoFrameLease l;
    ASSERT_TRUE(hub.acquire(l));
    hub.release(l.slot);
    ::close(l.fd);

    const std::string d = hub.describe();
    EXPECT_NE(d.find("hub:"), std::string::npos);
    EXPECT_NE(d.find("64x48"), std::string::npos);
    EXPECT_NE(d.find("stride=64"), std::string::npos);
    EXPECT_NE(d.find("pub=1"), std::string::npos);
    EXPECT_NE(d.find("acq=1"), std::string::npos);
    EXPECT_NE(d.find("rel=1"), std::string::npos);
    // truncated 只在非零时出现，避免常态噪声
    EXPECT_EQ(d.find("truncated="), std::string::npos);
}

TEST(VideoFrameHub, DescribeSaysUnconfiguredBeforeFirstConfigure) {
    VideoFrameHub hub(fastConfig());
    const std::string d = hub.describe();
    EXPECT_NE(d.find("unconfigured"), std::string::npos);
    EXPECT_NE(d.find("none"), std::string::npos);
}
