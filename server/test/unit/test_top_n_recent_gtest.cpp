// test_top_n_recent_gtest.cpp
// 单元测试：TopNRecent —— 遍历中保留「最近 N 条」的定长选择器
//
// 为什么需要这些用例：这段逻辑原先埋在 TcpRttMonitor::getStats() 里，
// 而那个函数依赖真实 BPF map，无法单测。堆式选择恰恰是「写错了不报错」
// 的那类代码 —— 顺序搞错只会少一条明细，分位数和连接总数照样正确，
// 上真机也未必看得出。所以必须在这里把语义钉死。

#include <gtest/gtest.h>

#include <vector>

#include "utils/top_n_recent.hpp"

namespace {

struct Item {
    uint64_t lastTimestampNs = 0;
    int id = 0;
};

/// 参照实现：全量排序取前 N —— TopNRecent 必须与它逐项一致。
std::vector<Item> referenceTopN(std::vector<Item> all, size_t n) {
    std::sort(all.begin(), all.end(), [](const Item& a, const Item& b) {
        return a.lastTimestampNs > b.lastTimestampNs;
    });
    if (all.size() > n) all.resize(n);
    return all;
}

/// 时间戳降序（消费方唯一依赖的顺序性质）。
void expectDescending(const std::vector<Item>& v) {
    for (size_t i = 1; i < v.size(); ++i) {
        EXPECT_GE(v[i - 1].lastTimestampNs, v[i].lastTimestampNs)
            << "输出必须按 lastTimestampNs 降序，index " << i << " 处逆序";
    }
}

}  // namespace

// 保留下来的必须恰好是时间戳最大的 N 条，且按降序输出。
TEST(TopNRecentTest, KeepsNewestNAndReturnsDescending) {
    weaknet::TopNRecent<Item> top(5);
    std::vector<Item> all;
    // 故意乱序输入，覆盖堆的上升/下沉两条路径。
    const uint64_t ts[] = {10, 90, 30, 70, 50, 20, 80, 40, 60, 100};
    for (int i = 0; i < 10; ++i) {
        Item it;
        it.lastTimestampNs = ts[i];
        it.id = i;
        all.push_back(it);
        top.offer(it);
    }

    auto got = top.take();
    auto want = referenceTopN(all, 5);

    ASSERT_EQ(got.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(got[i].lastTimestampNs, want[i].lastTimestampNs)
            << "第 " << i << " 条必须与全量排序取前 N 的参照实现一致";
        EXPECT_EQ(got[i].id, want[i].id);
    }
    expectDescending(got);
}

// 淘汰路径：堆满之后的每一条新候选都会触发「弹出堆顶 + 重新入堆」。
// 这里逐步把最旧的挤出去，最终必须只留下最新的一批。
TEST(TopNRecentTest, EvictsOldestWhenFull) {
    weaknet::TopNRecent<Item> top(3);
    for (uint64_t t = 1; t <= 4; ++t) {
        Item it;
        it.lastTimestampNs = t;
        it.id = static_cast<int>(t);
        top.offer(it);
    }
    auto got = top.take();
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].lastTimestampNs, 4u);
    EXPECT_EQ(got[1].lastTimestampNs, 3u);
    EXPECT_EQ(got[2].lastTimestampNs, 2u);
    // 时间戳 1 必须已被淘汰。
    for (const auto& it : got) {
        EXPECT_NE(it.lastTimestampNs, 1u) << "最旧的一条必须被淘汰";
    }
    expectDescending(got);
}

// 边界：候选数少于 N 时不能补空、不能少给。
TEST(TopNRecentTest, FewerThanLimitKeepsAll) {
    weaknet::TopNRecent<Item> top(10);
    for (uint64_t t = 1; t <= 3; ++t) {
        Item it;
        it.lastTimestampNs = t;
        it.id = static_cast<int>(t);
        top.offer(it);
    }
    auto got = top.take();
    ASSERT_EQ(got.size(), 3u);
    expectDescending(got);
}

// 边界：N = 0 时不得崩溃、不得产出。
TEST(TopNRecentTest, ZeroLimitYieldsEmpty) {
    weaknet::TopNRecent<Item> top(0);
    Item it;
    it.lastTimestampNs = 42;
    top.offer(it);
    EXPECT_TRUE(top.take().empty());
}

// 边界：完全没有候选。
TEST(TopNRecentTest, EmptyYieldsEmpty) {
    weaknet::TopNRecent<Item> top(5);
    EXPECT_TRUE(top.take().empty());
}

// full()/oldest() 是 getStats() 快路径的判断依据：
// 没满之前不能跳过构造（否则前 N 条会缺），满了之后 oldest() 必须是堆中最旧的。
TEST(TopNRecentTest, FullAndOldestTrackEvictionFrontier) {
    weaknet::TopNRecent<Item> top(3);
    Item a; a.lastTimestampNs = 5;
    Item b; b.lastTimestampNs = 1;
    Item c; c.lastTimestampNs = 9;

    top.offer(a);
    EXPECT_FALSE(top.full()) << "只有 1 条时不该判定已满";
    top.offer(b);
    EXPECT_FALSE(top.full());
    top.offer(c);
    EXPECT_TRUE(top.full()) << "满 3 条后必须判定已满";
    EXPECT_EQ(top.oldest().lastTimestampNs, 1u)
        << "堆顶必须是最旧的一条，否则 getStats() 的跳过判据会误杀新条目";
}

// 大规模一致性：随机顺序下与参照实现逐项比对。
// 这条用例覆盖 getStats() 的真实规模（板端实测 2258 条常驻）。
TEST(TopNRecentTest, MatchesReferenceOnLargeRandomInput) {
    constexpr size_t kTotal = 2258;
    constexpr size_t kLimit = 32;

    std::vector<Item> all;
    all.reserve(kTotal);
    for (size_t i = 0; i < kTotal; ++i) {
        Item it;
        // 用非单调的伪随机序列，避免退化成「输入已有序」的特例。
        it.lastTimestampNs = (static_cast<uint64_t>(i) * 2654435761u) % 1000000u;
        it.id = static_cast<int>(i);
        all.push_back(it);
    }

    weaknet::TopNRecent<Item> top(kLimit);
    for (const auto& it : all) {
        if (top.full() && it.lastTimestampNs <= top.oldest().lastTimestampNs) {
            continue;  // 与 getStats() 的跳过判据完全一致
        }
        top.offer(it);
    }

    auto got = top.take();
    auto want = referenceTopN(all, kLimit);

    ASSERT_EQ(got.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(got[i].lastTimestampNs, want[i].lastTimestampNs)
            << "第 " << i << " 条与参照实现不一致";
    }
    expectDescending(got);
}

// take() 之后对象必须可复用（getStats() 每轮都用同一个成员）。
TEST(TopNRecentTest, TakeResetsForReuse) {
    weaknet::TopNRecent<Item> top(2);
    Item a; a.lastTimestampNs = 1;
    Item b; b.lastTimestampNs = 2;
    Item c; c.lastTimestampNs = 3;
    top.offer(a);
    top.offer(b);
    EXPECT_EQ(top.take().size(), 2u);
    EXPECT_FALSE(top.full()) << "take() 之后必须回到空状态";
    top.offer(c);
    auto got = top.take();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].lastTimestampNs, 3u);
}
