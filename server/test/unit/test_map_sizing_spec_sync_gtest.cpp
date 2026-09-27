/**
 * @file test_map_sizing_spec_sync_gtest.cpp
 * @brief 规格表 default_entries 与 .bpf.c 编译期常量的交叉校验测试
 *
 * 背景：`bpf_map_sizing.hpp` 规定 `default_entries` 必须等于该 map 在对应
 * `.bpf.c` 源码中的编译期 `max_entries`，否则「未配置定标参数时行为与 v1
 * 零变化」这一不变式不成立。该约定原先只写在注释里，无人校验——真机核验
 * 发现 `process_stats` 的规格表默认值 8192 与源码声明 65536 相差 8 倍，
 * 且被既有单元测试（使用自造 spec，不读真实规格表）完全掩盖。
 *
 * 本测试遍历全部 MapSizingScope 的真实规格表，逐个到对应 .bpf.c 中解析该
 * map 声明的 max_entries（支持字面量与 `#define` 常量两种写法），逐项比对。
 * 不依赖 libbpf，可在 x86 直接运行。
 */

#include <gtest/gtest.h>

#include "utils/bpf_map_sizing.hpp"

#include <cstdint>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace weaknet;

#ifndef WEAKNET_SERVER_SOURCE_DIR
#  error "WEAKNET_SERVER_SOURCE_DIR must be defined by CMake"
#endif

namespace {

/// scope → 该 scope 的 map 定义所在 .bpf.c 源文件（相对 server/ 目录）
const std::map<MapSizingScope, const char*> kScopeSourceFiles = {
    {MapSizingScope::TcpRetrans,      "src/tcp_retransmit.bpf.c"},
    {MapSizingScope::ProcessProfiler, "src/flow_rate.bpf.c"},
    {MapSizingScope::HttpLatency,     "src/http_latency.bpf.c"},
    {MapSizingScope::Dns,             "src/dns_monitor.bpf.c"},
    {MapSizingScope::TcpConn,         "src/tcp_conn_stats.bpf.c"},
    {MapSizingScope::SkbDrop,         "src/skb_drop.bpf.c"},
    {MapSizingScope::Bluetooth,       "src/bpf/a2dp_media.bpf.c"},
};

std::string readFileOrEmpty(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

/// 在源码中解析 `#define NAME VALUE` 形式的整数常量；未命中返回 false
bool lookupDefine(const std::string& text, const std::string& name, uint64_t* out) {
    const std::regex re("#define\\s+" + name + "\\s+([0-9]+)\\b");
    std::smatch m;
    if (!std::regex_search(text, m, re)) return false;
    *out = std::stoull(m[1].str());
    return true;
}

/**
 * 解析某个 map 在 .bpf.c 中声明的编译期 max_entries。
 *
 * 写法为 `struct { ... __uint(max_entries, X); ... } <name> SEC(".maps");`，
 * 其中 X 可能是整数字面量，也可能是 `#define` 的标识符。
 *
 * 关键：只扫描**紧邻该声明之前**的那一个 map 定义块（上一个 `.maps` 声明之后
 * 到本声明之间），不能扫整个前缀——否则会误取其它 map 的 max_entries，
 * 并且一遇到本文件里无法解析的标识符（例如 enum 常量 DNS_STAT_MAX）就整体失败。
 */
bool parseDeclaredMaxEntries(const std::string& text, const std::string& mapName,
                             uint64_t* out) {
    // 定位 `} <name> SEC(".maps");`
    const std::regex decl("\\}\\s*" + mapName + "\\s+SEC\\(\"\\.maps\"\\)\\s*;");
    std::smatch m;
    if (!std::regex_search(text, m, decl)) return false;
    const size_t decl_pos = static_cast<size_t>(m.position());

    // 本 map 定义块的起点 = 上一个 `.maps` 声明结束处
    const std::string mapsMarker = "SEC(\".maps\");";
    const size_t prev = text.rfind(mapsMarker, decl_pos);
    const size_t block_start =
        (prev == std::string::npos) ? 0 : prev + mapsMarker.size();
    const std::string block = text.substr(block_start, decl_pos - block_start);

    const std::regex entry("__uint\\(max_entries,\\s*([A-Za-z0-9_]+)\\s*\\)");
    uint64_t found = 0;
    bool any = false;
    for (auto it = std::sregex_iterator(block.begin(), block.end(), entry);
         it != std::sregex_iterator(); ++it) {
        const std::string value = (*it)[1].str();
        uint64_t parsed = 0;
        if (value[0] >= '0' && value[0] <= '9') {
            parsed = std::stoull(value);
        } else if (!lookupDefine(text, value, &parsed)) {
            return false;  // 无法解析的标识符：视为未知，交由断言报错
        }
        found = parsed;
        any = true;
    }
    if (!any) return false;
    *out = found;
    return true;
}

std::string scopeLabel(MapSizingScope scope) {
    switch (scope) {
        case MapSizingScope::TcpRetrans:      return "TcpRetrans";
        case MapSizingScope::ProcessProfiler: return "ProcessProfiler";
        case MapSizingScope::HttpLatency:     return "HttpLatency";
        case MapSizingScope::Dns:             return "Dns";
        case MapSizingScope::TcpConn:         return "TcpConn";
        case MapSizingScope::SkbDrop:         return "SkbDrop";
        case MapSizingScope::Bluetooth:       return "Bluetooth";
    }
    return "?";
}

}  // namespace

/**
 * 核心不变式：规格表 default_entries 必须等于 .bpf.c 中该 map 的编译期
 * max_entries。任何一处漂移都会让「不配任何定标参数时行为与 v1 零变化」失效。
 */
TEST(MapSizingSpecSyncTest, DefaultEntriesMatchesBpfSourceConstant) {
    size_t checked = 0;
    for (const auto& [scope, relative] : kScopeSourceFiles) {
        const std::string path =
            std::string(WEAKNET_SERVER_SOURCE_DIR) + "/" + relative;
        const std::string text = readFileOrEmpty(path);
        ASSERT_FALSE(text.empty()) << "无法读取 BPF 源码: " << path;

        const auto& specs = getMapSizingSpecs(scope);
        ASSERT_FALSE(specs.empty()) << "规格表为空: " << scopeLabel(scope);

        for (const auto& spec : specs) {
            const std::string mapName = spec.map_name;
            uint64_t declared = 0;
            ASSERT_TRUE(parseDeclaredMaxEntries(text, mapName, &declared))
                << "在 " << relative << " 中找不到 map '" << mapName
                << "' 的 max_entries 声明（scope=" << scopeLabel(scope) << "）";

            EXPECT_EQ(static_cast<uint64_t>(spec.default_entries), declared)
                << "规格表默认值与 BPF 源码声明不一致: map=" << mapName
                << " (scope=" << scopeLabel(scope) << ")  规格表 default_entries="
                << spec.default_entries << "  源码 max_entries=" << declared
                << "  —— 缺省行为将偏离 v1，请同步 " << relative;
            ++checked;
        }
    }
    // 守住覆盖面：当前共 14 张受管 map，防止解析逻辑静默只覆盖一部分
    EXPECT_GE(checked, 14u) << "交叉校验覆盖的 map 数量异常偏少";
}

/// 规格表自身的单调性（既有测试已覆盖，这里再守一次以免新增条目破坏）
TEST(MapSizingSpecSyncTest, SpecBoundsAreMonotonic) {
    for (const auto& [scope, relative] : kScopeSourceFiles) {
        (void)relative;
        for (const auto& spec : getMapSizingSpecs(scope)) {
            EXPECT_LE(spec.min_entries, spec.default_entries)
                << spec.map_name << " (scope=" << scopeLabel(scope) << ")";
            EXPECT_LE(spec.default_entries, spec.max_entries)
                << spec.map_name << " (scope=" << scopeLabel(scope) << ")";
        }
    }
}
