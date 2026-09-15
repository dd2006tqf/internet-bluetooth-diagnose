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
        std::atomic<uint32_t> capture_pages{64};             ///< perf ring buffer 页数（每 CPU）
        ConfigString assessment_profile{"INTERNET_ACCESS"};  ///< IR-3: NETWORK_ONLY | INTERNET_ACCESS
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

    struct {
        std::atomic<bool> enabled{true};
        ConfigString bpf_obj{"build/skb_drop.bpf.o"};
        std::atomic<uint32_t> interval_ms{10000};
    } skb_drop;

    struct {
        std::atomic<bool> enabled{true};
        ConfigString bpf_obj{"build/tcp_connect.bpf.o"};
        std::atomic<uint32_t> interval_ms{10000};
        std::atomic<uint32_t> capture_pages{32};
    } tcp_connect;

    // ---------- 受控主动连通性探测 ----------
    //
    // 默认 **关闭**，且**不内置任何第三方默认目标**。
    // 理由：probe 目标决定了"设备主动连接谁"，也决定了评价体系把谁当作
    // ground truth。硬编码第三方公共服务会产生新的错误 oracle
    // （对方限流/地区不可达/改响应都会被解释成 Internet 故障）。
    // 因此默认 targets 为空；部署方须显式提供 targets 才启用。
    //
    // targets 格式：逗号分隔的 "id|hostname|port"（端口可省略，默认 443）。
    // 本项目 YAML 解析器只支持 key: value 两级结构，不支持列表语法，
    // 故用逗号分隔字符串表达多目标；至少需要 2 个目标才有判定资格。
    struct {
        std::atomic<bool> enabled{false};
        // 与其它监控器一致，内部统一以**毫秒**存储（setDurationField 的语义）。
        // 此前字段名为 interval_sec 却存 ms，导致线程按"秒"计算 sleep 时长，
        // 探测实际约 2.8 小时才跑一轮 —— 结论长期停留在首轮快照。
        std::atomic<uint32_t> interval_ms{30000};
        std::atomic<uint32_t> timeout_ms{3000};
        ConfigString targets{""};        ///< "id|host|port,id|host|port"
        // HTTPS 探测开关。关闭时 evaluator 返回 NO_CAPABILITY
        // （reason=no_tls_probe_capability），绝不因 DNS/TCP 成功就宣称 HTTPS 可用。
        std::atomic<bool> https_enabled{true};
        // Portal oracle 开关。开启后额外做一次**明文 HTTP** connectivity-check
        // 请求，比对是否被重定向/替换内容。
        std::atomic<bool> portal_check_enabled{false};
        // Portal oracle 的受控端点。格式同 targets（"id|host|port"），
        // 但语义不同：这些端点必须是**响应已知**的 connectivity-check 服务。
        // 默认空 → 不做 oracle 探测 → Portal 返回 NO_CAPABILITY，
        // 绝不退回"观测到 302 就算门户"这种推测。
        ConfigString portal_targets{""};
        // 明文 HTTP 路径（Portal oracle 用），默认 "/"
        ConfigString portal_path{"/"};
        // oracle 响应正文必须包含的子串；空则不比对正文
        ConfigString portal_expect_body{""};
    } active_probe;

    // ---------- 边缘遥测上报（WeakNet → 中心平台）----------
    //
    // 默认 **关闭**，且**不内置任何默认服务端地址**。理由与 active_probe
    // 的 targets 相同：上报会把"这台设备主动把网络状态发给谁"变成系统事实，
    // 硬编码一个默认 endpoint 会让每个部署都默认向第三方外发数据。
    //
    // 启用时必须同时提供 url / device_id / token / private_key_path，
    // 缺一不可（否则 exporter 记 error 并保持关闭，不会半启用）。
    //
    // 安全语义：请求体是签名覆盖的原始字节，签名走 Ed25519（OpenSSL）。
    // 服务端用同一份字节验签，因此本端**不得**在签名后重新序列化。
    struct {
        std::atomic<bool> enabled{false};
        // 形如 "http://host:8000/api/v1/network/edge/telemetry"
        ConfigString url{""};
        // 租户标识，作为 X-Edge-Tenant 头；服务端据此归属数据
        ConfigString tenant{""};
        // 设备唯一标识（同时是服务端资产主键），字符集 [A-Za-z0-9._-]
        ConfigString device_id{""};
        // 设备预共享令牌，作为 X-Edge-Token 头（非机密中的机密，仍是凭据）
        ConfigString token{""};
        // Ed25519 私钥 PEM 路径；与 token 一起决定上报身份
        ConfigString private_key_path{""};
        // 密钥标识（服务端据 X-Edge-Key-Id 选择信任锚）
        ConfigString key_id{""};
        // 上报周期；与评估节奏（quality 线程）对齐，一个周期最多一条
        std::atomic<uint32_t> interval_ms{10000};
        // 单次 HTTP 请求超时；弱网下不宜过长，失败留待下一轮补发
        std::atomic<uint32_t> timeout_ms{5000};
    } edge;
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

/// 读取或更新指定监控器的 enabled 开关（供生命周期管理器同步运行意图）。
bool getMonitorEnabled(const WeakNetConfig& cfg, const std::string& monitor, bool* enabled);
bool setMonitorEnabled(WeakNetConfig* cfg, const std::string& monitor, bool enabled);

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
