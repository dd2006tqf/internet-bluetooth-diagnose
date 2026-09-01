/**
 * @file weaknet_config.hpp
 * @brief 线程安全的运行时配置结构
 *
 * 配置驱动的唯一真相源（single source of truth）。所有硬编码参数
 * （RTT 目标、采样周期、eBPF 对象路径等）收拢于此，默认值即改造前
 * server.cpp 中的硬编码，保证「不配任何文件时行为零变化」。
 *
 * 线程安全：
 *   - 整数类参数（interval/timeout/window）用 std::atomic，监控线程每次
 *     循环现读，无需加锁
 *   - 字符串类参数（target/bpf_obj/data_dir）受 std::mutex 保护，通过
 *     get/set 方法访问；监控线程每轮现读，避免启动时快照的过期问题
 *
 * 设计约束：
 *   - 不做热加载：配置只读一次，运行时覆盖仅走 D-Bus SetMonitorParam
 *   - eBPF 对象路径默认保持相对路径（"build/xxx.bpf.o"），与 systemd
 *     WorkingDirectory + dist-arm64/server/build 布局的三方契约一致
 */

#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace weaknet_dbus {

/// 线程安全的字符串配置项（std::mutex 保护）
class ConfigString {
public:
    explicit ConfigString(std::string value) : value_(std::move(value)) {}

    std::string get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

    void set(const std::string& v) {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = v;
    }

private:
    mutable std::mutex mutex_;
    std::string value_;
};

/// 运行时配置根结构。默认值 = 现有代码的硬编码（行为零变化基线）。
struct WeakNetConfig {
    // ---------- 服务端 ----------
    ConfigString dbus_name{"com.example.WeakNet"};
    ConfigString data_dir{""};        ///< 空 → 走 WEAKNET_DATA_DIR / 内置默认
    ConfigString log_level{"info"};

    // ---------- 传统监控线程 ----------
    struct {
        std::atomic<bool> enabled{true};
        ConfigString target{"223.5.5.5"};
        std::atomic<uint32_t> interval_ms{10000};
        std::atomic<uint32_t> timeout_ms{800};
    } rtt;

    struct {
        std::atomic<bool> enabled{true};
        ConfigString target{"223.5.5.5"};
        std::atomic<uint32_t> interval_ms{2000};
        std::atomic<uint32_t> timeout_ms{800};
        std::atomic<uint32_t> window_size{30};
    } jitter;

    struct {
        std::atomic<bool> enabled{true};
        std::atomic<uint32_t> interval_ms{10000};
    } rssi;

    struct {
        std::atomic<bool> enabled{true};
        std::atomic<uint32_t> interval_ms{10000};
    } tcp_loss;

    struct {
        std::atomic<bool> enabled{true};
        std::atomic<uint32_t> interval_ms{10000};
    } traffic;

    struct {
        std::atomic<bool> enabled{true};
        std::atomic<uint32_t> interval_ms{15000};
    } quality;

    struct {
        std::atomic<bool> enabled{true};
        std::atomic<uint32_t> interval_ms{3000};
        ConfigString bpf_obj{"build/a2dp_media.bpf.o"};   ///< Phase2 eBPF 融合层对象
    } bluetooth;

    // ---------- eBPF 监控线程 ----------
    struct {
        std::atomic<bool> enabled{true};
        ConfigString bpf_obj{"build/dns_monitor.bpf.o"};
        std::atomic<uint32_t> interval_ms{10000};
    } dns;

    struct {
        std::atomic<bool> enabled{true};
        ConfigString bpf_obj{"build/wifi_packet_loss.bpf.o"};
        std::atomic<uint32_t> interval_ms{10000};
    } wifi_loss;

    struct {
        std::atomic<bool> enabled{true};
        ConfigString bpf_obj{"build/http_latency.bpf.o"};
        std::atomic<uint32_t> interval_ms{10000};
    } http_latency;

    struct {
        std::atomic<bool> enabled{true};
        ConfigString bpf_obj{"build/flow_rate.bpf.o"};
        std::atomic<uint32_t> interval_ms{15000};
    } process_profiler;

    struct {
        std::atomic<bool> enabled{true};
        ConfigString bpf_obj{"build/tcp_retransmit.bpf.o"};
        std::atomic<uint32_t> interval_ms{15000};   ///< 原始循环 i<150 × 100ms = 15s
    } tcp_retrans;

    struct {
        std::atomic<bool> enabled{true};
        ConfigString bpf_obj{"build/tcp_conn_stats.bpf.o"};
        std::atomic<uint32_t> interval_ms{15000};   ///< 原始循环 i<150 × 100ms = 15s
    } tcp_conn;
};

/**
 * @brief 加载 YAML 子集配置文件并填充 WeakNetConfig
 *
 * 支持语法（两段缩进、注释、时长后缀），见实现注释。缺字段回落默认值，
 * 语法/未知键错误返回 false 并带行号。
 *
 * @param path   配置文件路径；不存在时返回 true 且 out 保持默认值
 * @param out    输出配置（不存在的键保留既有值）
 * @param error  失败时填充错误描述（含行号）
 * @return true 成功（或文件不存在）；false 语法错误
 */
bool loadWeakNetConfig(const std::string& path, WeakNetConfig* out, std::string* error);

/// 判定字段是否为监控器开关（以 .enabled 结尾）
bool isEnabledKey(const std::string& key);

/// 将 "monitor.param" 拆分；格式非法返回 false
bool splitMonitorKey(const std::string& dotted, std::string* monitor, std::string* field);

/**
 * @brief 设置监控器参数：白名单校验 + 类型校验 + 区间校验 + 原子提交
 *
 * 仅修改内存态（与 YAML 启动快照分离），不写回文件。
 * 失败时 config 保持旧值。
 *
 * @param cfg    目标配置（直接持有 ctx.cfg 引用）
 * @param key    形如 "rtt.interval_ms" / "dns.bpf_obj"
 * @param value  字符串值
 * @param error  失败时填入错误描述
 * @return true 成功；false 校验失败
 */
bool setMonitorParam(WeakNetConfig* cfg, const std::string& key,
                     const std::string& value, std::string* error);

/**
 * @brief 序列化单个监控器当前参数为 JSON
 *
 * 例如 "rtt" → {"enabled":true,"target":"223.5.5.5","interval_ms":10000,"timeout_ms":800}
 * "all" → {"server":{...},"rtt":{...},...}
 * 未知 monitor 返回空字符串 + error
 */
std::string serializeMonitorJson(const WeakNetConfig& cfg, const std::string& monitor,
                                 std::string* error);

}  // namespace weaknet_dbus
