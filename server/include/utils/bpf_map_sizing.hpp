/**
 * @file bpf_map_sizing.hpp
 * @brief eBPF Map 容量运行时定标
 *
 * 把此前硬编码在各 *.bpf.c 源码里的 LRU_HASH max_entries 常量改为
 * 在 bpf_object__open_file → bpf_object__load 间隙按物理内存与配置定标。
 *
 * 两种模式：
 *   - fixed：直接使用配置的 entries，按各 map 的 [min,max] 钳位
 *   - auto ：预算 = 物理内存 × ram_budget_bp/10000，按
 *            default_entries × (key+value+内核开销) 加权分摊到该监控器的全部可调 map，
 *            再按 [min,max] 钳位
 *
 * 本头文件不依赖 libbpf（applyMapSizingPlan 除外），纯逻辑部分可在 x86 直接单测。
 * 默认行为必须与 v1 的编译期常量完全一致：ram_bytes=0、预算为 0 或 mode 非法时
 * 一律回退到 default_entries。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/// libbpf 前向声明（避免头文件依赖 libbpf）
struct bpf_object;

namespace weaknet {

/**
 * @brief 单个监控器的 map 定标配置（来自 config.yaml / SetMonitorParam）
 */
struct MapSizingConfig {
    std::string mode{"auto"};      ///< "auto" | "fixed"
    uint32_t entries{0};           ///< fixed 模式生效；auto 模式下作为提示值不参与计算
    uint32_t ram_budget_bp{50};    ///< auto：物理内存的万分之几（50 = 0.5%）
};

/**
 * @brief 一张 map 的定标元数据
 *
 * key/value 尺寸用于 auto 模式的加权分摊；min/max 是该 map 的语义边界。
 * default_entries 必须等于 v1 源码里的编译期常量，保证缺省行为不变。
 */
struct MapSizeSpec {
    const char* map_name;
    uint32_t key_size_bytes;
    uint32_t value_size_bytes;
    uint32_t min_entries;
    uint32_t default_entries;
    uint32_t max_entries;
};

/// 定标来源（用于 GetEbpfMapStats 观测）
enum class MapSizeSource : uint32_t {
    Fixed = 0,    ///< 来自 fixed 模式的 entries
    Auto = 1,     ///< 来自 auto 模式的预算分摊
    Default = 2,  ///< 回退到 default_entries（与 v1 一致）
};

/// 单张 map 的定标结果
struct ResolvedMapSize {
    const char* map_name;
    uint32_t max_entries;
    MapSizeSource resolved_by;
};

/// 参与定标的监控器（按配置块划分）
enum class MapSizingScope {
    TcpRetrans,        ///< tcp_retrans 配置块
    ProcessProfiler,   ///< process_profiler 配置块（含 flow_rate 的 current_sec/process_stats）
    HttpLatency,       ///< http_latency 配置块
    Dns,               ///< dns 配置块
    TcpConn,           ///< tcp_conn 配置块
    SkbDrop,           ///< skb_drop 配置块
    Bluetooth,         ///< bluetooth 配置块（a2dp）
};

/// auto 模式下每 entry 的内核开销估算（map 元数据 + 桶开销）
constexpr uint32_t kMapEntryOverheadBytes = 200;

/**
 * @brief 读取物理内存总量
 * @return 字节数；sysinfo 失败时返回 0（调用方据此回退默认值）
 */
size_t totalPhysicalRamBytes();

/**
 * @brief 按配置与物理内存解算各 map 的 max_entries
 *
 * @param specs     该监控器的 map 元数据表
 * @param cfg       定标配置
 * @param ram_bytes 物理内存字节数（0 表示不可用 → 全部回退 default）
 * @return 与 specs 等长、同序的解算结果
 *
 * 不变式：
 *   - 返回值中每项均落在对应 spec 的 [min_entries, max_entries]
 *   - mode 非 "auto"/"fixed"、预算为 0、或 ram_bytes=0 → 全部 default_entries
 */
std::vector<ResolvedMapSize> resolveMonitorMaps(const std::vector<MapSizeSpec>& specs,
                                                const MapSizingConfig& cfg,
                                                size_t ram_bytes);

/// 取某监控器的 map 元数据表（default_entries 与 v1 编译期常量一致）
const std::vector<MapSizeSpec>& getMapSizingSpecs(MapSizingScope scope);

/// 一次定标解算的结果集合（传给 monitor init 的"计划"）
using MapSizingPlan = std::vector<ResolvedMapSize>;

/**
 * @brief 从解算结果中查某 map 的最终容量
 * @return 命中返回该 map 的 max_entries；未命中返回 0（表示容量未知）
 */
inline uint32_t findResolvedMax(const MapSizingPlan& plan, const char* map_name) {
    if (!map_name) return 0;
    for (const auto& item : plan) {
        if (item.map_name && std::strcmp(item.map_name, map_name) == 0) {
            return item.max_entries;
        }
    }
    return 0;
}

/**
 * @brief 便捷入口：按监控器 scope 取 specs 并解算
 *
 * 供各 plugin 的 start() 调用：从 ctx->cfg.<mon>.map_sizing 读出配置，
 * 与物理内存一起解算成 plan，再传给对应 monitor 的 init()。
 */
MapSizingPlan resolveScopePlan(MapSizingScope scope, const MapSizingConfig& cfg,
                               size_t ram_bytes);

/**
 * @brief 在 load 之前把解算结果写入 bpf_object
 *
 * 必须在 bpf_object__open_file 之后、bpf_object__load 之前调用。
 * 找不到同名 map 时跳过该条（不视为错误：不同内核/版本 map 集合可能不同）。
 *
 * @return true 全部命中或跳过；false 表示 libbpf 不可用
 */
bool applyMapSizingPlan(bpf_object* obj, const std::vector<ResolvedMapSize>& plan);

} // namespace weaknet
