/**
 * @file test_bpf_map_sizing_gtest.cpp
 * @brief eBPF Map 容量运行时定标 —— 纯逻辑单元测试
 *
 * 覆盖 resolveMonitorMaps 的 auto/fixed 两种模式、边界钳位与非法输入降级，
 * 以及 getMapSizingSpecs 规格表自洽性（min <= default <= max）。
 *
 * 不依赖 libbpf：可在 x86 直接运行。
 */

#include <gtest/gtest.h>

#include "utils/bpf_map_sizing.hpp"

#include <algorithm>
#include <string>

using namespace weaknet;

namespace {

/// 一张用于测试的 map 元数据
const MapSizeSpec kSpec{"test_map", 16, 32, 1024, 8192, 65536};

std::vector<MapSizeSpec> oneSpec() { return {kSpec}; }

const ResolvedMapSize& only(const std::vector<ResolvedMapSize>& v) {
    EXPECT_EQ(v.size(), 1u);
    return v.front();
}

} // namespace

// ---------------------------------------------------------------------------
// fixed 模式
// ---------------------------------------------------------------------------

TEST(BpfMapSizingTest, FixedModeUsesConfiguredEntries) {
    MapSizingConfig cfg;
    cfg.mode = "fixed";
    cfg.entries = 4096;

    auto resolved = resolveMonitorMaps(oneSpec(), cfg, 8ull * 1024 * 1024 * 1024);
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_EQ(only(resolved).max_entries, 4096u);
    EXPECT_EQ(only(resolved).resolved_by, MapSizeSource::Fixed);
}

TEST(BpfMapSizingTest, FixedModeClampsToMax) {
    MapSizingConfig cfg;
    cfg.mode = "fixed";
    cfg.entries = 1u << 20;  // 远超 max_entries

    auto resolved = resolveMonitorMaps(oneSpec(), cfg, 8ull * 1024 * 1024 * 1024);
    EXPECT_EQ(only(resolved).max_entries, kSpec.max_entries);
}

TEST(BpfMapSizingTest, FixedModeClampsToMin) {
    MapSizingConfig cfg;
    cfg.mode = "fixed";
    cfg.entries = 1;  // 低于 min_entries

    auto resolved = resolveMonitorMaps(oneSpec(), cfg, 8ull * 1024 * 1024 * 1024);
    EXPECT_EQ(only(resolved).max_entries, kSpec.min_entries);
}

// ---------------------------------------------------------------------------
// auto 模式
// ---------------------------------------------------------------------------

TEST(BpfMapSizingTest, AutoModeScalesWithRam) {
    MapSizingConfig cfg;
    cfg.mode = "auto";
    cfg.ram_budget_bp = 50;  // 0.5%

    // 小内存机：预算不足以支撑 default_entries，应低于 default 但不低于 min
    auto small = resolveMonitorMaps(oneSpec(), cfg, 256ull * 1024 * 1024);
    ASSERT_EQ(small.size(), 1u);
    EXPECT_GE(only(small).max_entries, kSpec.min_entries);
    EXPECT_LT(only(small).max_entries, kSpec.default_entries);

    // 大内存机：预算充足，应高于小内存机的结果
    auto large = resolveMonitorMaps(oneSpec(), cfg, 32ull * 1024 * 1024 * 1024);
    EXPECT_GT(only(large).max_entries, only(small).max_entries);
}

TEST(BpfMapSizingTest, AutoModeRespectsMaxCap) {
    MapSizingConfig cfg;
    cfg.mode = "auto";
    cfg.ram_budget_bp = 10000;  // 100% 内存，必然是极端值

    auto resolved = resolveMonitorMaps(oneSpec(), cfg, 64ull * 1024 * 1024 * 1024);
    EXPECT_EQ(only(resolved).max_entries, kSpec.max_entries);
}

TEST(BpfMapSizingTest, AutoModeReportsSource) {
    MapSizingConfig cfg;
    cfg.mode = "auto";
    cfg.ram_budget_bp = 50;

    auto resolved = resolveMonitorMaps(oneSpec(), cfg, 8ull * 1024 * 1024 * 1024);
    EXPECT_EQ(only(resolved).resolved_by, MapSizeSource::Auto);
}

// ---------------------------------------------------------------------------
// 降级路径：必须严格回退到 v1 编译期常量
// ---------------------------------------------------------------------------

TEST(BpfMapSizingTest, ZeroRamFallsBackToDefault) {
    MapSizingConfig cfg;
    cfg.mode = "auto";
    cfg.ram_budget_bp = 50;

    auto resolved = resolveMonitorMaps(oneSpec(), cfg, 0);
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_EQ(only(resolved).max_entries, kSpec.default_entries);
    EXPECT_EQ(only(resolved).resolved_by, MapSizeSource::Default);
}

TEST(BpfMapSizingTest, ZeroBudgetFallsBackToDefault) {
    MapSizingConfig cfg;
    cfg.mode = "auto";
    cfg.ram_budget_bp = 0;

    auto resolved = resolveMonitorMaps(oneSpec(), cfg, 8ull * 1024 * 1024 * 1024);
    EXPECT_EQ(only(resolved).max_entries, kSpec.default_entries);
    EXPECT_EQ(only(resolved).resolved_by, MapSizeSource::Default);
}

TEST(BpfMapSizingTest, InvalidModeFallsBackToDefault) {
    MapSizingConfig cfg;
    cfg.mode = "bogus";
    cfg.entries = 4096;

    auto resolved = resolveMonitorMaps(oneSpec(), cfg, 8ull * 1024 * 1024 * 1024);
    EXPECT_EQ(only(resolved).max_entries, kSpec.default_entries);
    EXPECT_EQ(only(resolved).resolved_by, MapSizeSource::Default);
}

TEST(BpfMapSizingTest, EmptySpecsYieldsEmptyResult) {
    MapSizingConfig cfg;
    cfg.mode = "auto";
    auto resolved = resolveMonitorMaps({}, cfg, 8ull * 1024 * 1024 * 1024);
    EXPECT_TRUE(resolved.empty());
}

// ---------------------------------------------------------------------------
// 规格表自洽性
// ---------------------------------------------------------------------------

TEST(BpfMapSizingTest, AllSpecsAreSelfConsistent) {
    const MapSizingScope scopes[] = {
        MapSizingScope::TcpRetrans, MapSizingScope::ProcessProfiler,
        MapSizingScope::HttpLatency, MapSizingScope::Dns,
        MapSizingScope::TcpConn, MapSizingScope::SkbDrop,
        MapSizingScope::Bluetooth,
    };
    for (auto scope : scopes) {
        const auto& specs = getMapSizingSpecs(scope);
        EXPECT_FALSE(specs.empty()) << "scope has no specs";
        for (const auto& spec : specs) {
            EXPECT_NE(spec.map_name, nullptr);
            EXPECT_GT(std::string(spec.map_name).size(), 0u);
            EXPECT_LE(spec.min_entries, spec.default_entries);
            EXPECT_LE(spec.default_entries, spec.max_entries);
            EXPECT_GT(spec.min_entries, 0u);
        }
    }
}

TEST(BpfMapSizingTest, DefaultModeMatchesV1Constants) {
    // 缺省配置（mode=auto 但 RAM=0）必须逐项等于 v1 编译期常量
    MapSizingConfig cfg;  // 默认构造
    const MapSizingScope scopes[] = {
        MapSizingScope::TcpRetrans, MapSizingScope::ProcessProfiler,
        MapSizingScope::HttpLatency, MapSizingScope::Dns,
        MapSizingScope::TcpConn, MapSizingScope::SkbDrop,
        MapSizingScope::Bluetooth,
    };
    for (auto scope : scopes) {
        const auto& specs = getMapSizingSpecs(scope);
        auto resolved = resolveMonitorMaps(specs, cfg, 0);
        ASSERT_EQ(resolved.size(), specs.size());
        for (size_t i = 0; i < specs.size(); ++i) {
            EXPECT_EQ(resolved[i].max_entries, specs[i].default_entries)
                << "map " << specs[i].map_name;
        }
    }
}

TEST(BpfMapSizingTest, TotalPhysicalRamIsPlausible) {
    // 真实主机上应能取到非零内存；取不到时必须是 0（调用方据此降级）
    size_t ram = totalPhysicalRamBytes();
    EXPECT_TRUE(ram == 0 || ram > 64ull * 1024 * 1024)
        << "unexpected RAM value: " << ram;
}
