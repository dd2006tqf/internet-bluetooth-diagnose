/**
 * @file weaknet_client.h
 * @brief WeakNet 客户端动态库 C API 接口头文件
 *
 * 本文件定义了 WeakNet 客户端动态库对外暴露的 C 接口，
 * 用于让其他 C/C++ 程序方便地集成 WeakNet 网络监控功能。
 *
 * 客户端通过 D-Bus 系统总线（System Bus）连接到 WeakNet 服务端：
 *   - 服务名 (Bus Name):   com.example.WeakNet
 *   - 对象路径 (ObjPath):  /com/example/WeakNet
 *   - 接口名 (Interface):  com.example.WeakNet
 *
 * 支持的 D-Bus 方法 (Method):
 *   - GetInterfaces             获取当前网络接口列表
 *   - HealthCheck               网络健康检查
 *   - Ping                      Ping 指定主机
 *   - GetBluetoothDevices       蓝牙设备列表
 *   - GetBluetoothAdapter       蓝牙适配器状态
 *   - GetDnsStats               DNS eBPF 监控统计
 *   - GetWifiLossStats          Wi-Fi 丢包统计
 *   - GetHttpLatencyStats       HTTP 请求延迟统计
 *   - GetProcessProfiling       进程网络画像
 *   - GetEbpfMonitorHealth      eBPF 监控器健康快照
 *   - GetHistory                查询历史监控数据
 *
 * ⚠️ 事件订阅族已于 2026-11-01 **删除**（服务端 2026-10-19 起不再对外发送 D-Bus 信号）。
 *
 *   停发的信号：Changed / InterfaceChanged / ConnectionModeChanged /
 *   NetworkQualityChanged / BluetoothDeviceChanged / MonitorStateChanged /
 *   EbpfMonitorStats。
 *
 *   **为什么服务端不对外发信号（这不是"因为没人订阅"，因果顺序反过来）**：
 *   服务端产出的对外数据是经过迟滞平滑的结论型快照，正确消费方式是每次读全量，
 *   不是维护本地增量副本。而 D-Bus 信号是单向广播，没有投递确认、没有背压、
 *   没有重放，也无法表达"不知道"——消费者漏一条就会永久停留在过期的旧值上，
 *   且"状态没变"与"消息没收到"在信号语义里无法区分。评估模型必须能明确回答
 *   UNKNOWN 及其原因（no_assessment_yet / stale_assessment），只有方法调用能做到。
 *   因此对外这条链路从一开始就设计成拉取式；核对时发现订阅者为零，是这个设计
 *   的结果，而不是停止发送的理由。
 *
 *   注意：D-Bus 信号并未废弃，服务端仍在**接收**方向使用它（订阅 org.bluez 的
 *   PropertiesChanged / InterfacesAdded / InterfacesRemoved）。信号适合
 *   "别人变了、不问就不会知道"的场景，不适合"我自己算出来的东西、你随时能来问"。
 *
 *   已删除的 API（曾长期"空转"：订得上但永远等不到事件）：
 *     weaknet_check_changes()              永远返回 false
 *     weaknet_subscribe_event()            只加 D-Bus match，服务端不会发
 *     weaknet_unsubscribe_event()          只做本地状态，固定返回 true
 *     weaknet_get_event_types()            本地拼接常量，与生产端无关
 *     weaknet_check_events()               永远返回 false
 *     weaknet_subscribe_network_quality()  阻塞监听循环，收不到信号
 *     weaknet_check_network_quality()      永远返回 false
 *     weaknet_subscribe_bluetooth_events() 只加 match，收不到信号
 *
 *   删除而非保留的理由：它们返回"成功"却不产生任何事件，对集成方是
 *   **静默的谎**——比编译报错危险得多。保留只会让人以为事件系统可用。
 *
 *   **迁移指引**：要拿当前状态请直接调用对应的 get_* / *_stats 查询方法。
 *   这是**架构选择，不是待补的缺口**：拉取才能保证消费者拿到的一定是当前结论，
 *   或者明确告知"不知道"。若确有事件驱动的真实需求，须先有真实订阅者、
 *   且先解决"如何表达不知道"，
 *   再走 OpenSpec change 重建发射端，不得凭空恢复发射。
 *   架构层面的取舍见 docs/架构设计-模块与代码结构.md 的 DbusService 段与
 *   server/src/dbus_service.cpp 文件头的 O3-B 说明。
 *
 * 使用流程：
 *   1. weaknet_init()              -- 初始化库并建立 D-Bus 连接
 *   2. weaknet_is_connected()      -- 检查连接是否就绪
 *   3. 调用各种 get_* / *_stats   -- 获取数据（这是当前唯一有效的数据路径）
 *   4. weaknet_cleanup()           -- 释放资源
 */

#ifndef WEAKNET_CLIENT_H
#define WEAKNET_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WeakNet 客户端动态库 API
 * 
 * 此库提供与WeakNet D-Bus服务端通信的C接口，包括：
 * - 网络接口信息获取
 * - 网络状态监控
 * - 事件监听和订阅
 * - TCP丢包率监控
 * - RTT延迟监控
 * 
 * 使用示例（只用真实有效的查询路径）：
 *   weaknet_init();                               // 初始化
 *   weaknet_get_interfaces(buf, sz, err, errsz);  // 获取网卡列表
 *   weaknet_health_check(buf, sz, err, errsz);    // 获取健康快照
 *   weaknet_cleanup();                            // 清理
 *
 * 注意：weaknet_subscribe_* / weaknet_check_* 事件订阅族已于 2026-11-01 删除，见文件头说明。
 */

// ============== 库初始化和清理 ==============

/**
 * @brief 初始化 WeakNet 客户端库
 *
 * 必须在调用其他任何 weaknet_* 函数之前调用此函数。
 * 该函数会：
 *   1. 初始化日志系统（输出到 ./logs/client 目录）
 *   2. 连接到 D-Bus System 总线
 *
 * @return true  - 初始化成功，已连接到 D-Bus 总线
 * @return false - 初始化失败（D-Bus 未运行或权限不足）
 */
bool weaknet_init();

/**
 * @brief 清理 WeakNet 客户端库资源
 *
 * 在应用程序退出前调用此函数。会断开 D-Bus 连接、
 * 释放单例客户端实例、关闭日志系统。
 */
void weaknet_cleanup();

/**
 * @brief 检查客户端是否已连接到 WeakNet 服务
 *
 * @return true  - 已连接到 D-Bus 总线
 * @return false - 未初始化或连接已断开
 */
bool weaknet_is_connected();

// ============== 网络接口信息获取 ==============

/**
 * @brief 获取当前网络接口信息
 *
 * 通过 D-Bus 调用 GetInterfaces 方法，服务端返回网卡名称数组，
 * 本函数用逗号连接成一个字符串写入 buffer。
 *
 * D-Bus 调用：
 *   - Method: GetInterfaces
 *   - Returns: ARRAY of STRING → 拼接为 "eth0,wlan0,lo"
 *
 * @param buffer      结果缓冲区，将存储逗号分隔的网卡名称列表
 * @param buffer_size 缓冲区大小（字节）
 * @param error_buffer 错误信息缓冲区（调用失败时写入）
 * @param error_size   错误缓冲区大小
 * @return true  - 调用成功
 * @return false - 调用失败（error_buffer 中有错误描述）
 */
bool weaknet_get_interfaces(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 网络健康检查
 *
 * 通过 D-Bus 调用 HealthCheck 方法，服务端执行一系列
 * 网络诊断（DNS、路由、丢包等）并返回 JSON 格式结果。
 *
 * D-Bus 调用：
 *   - Method: HealthCheck
 *   - Returns: STRING (JSON 格式诊断报告)
 *
 * @param result_buffer 结果缓冲区，存储 JSON 诊断报告
 * @param result_size   结果缓冲区大小
 * @param error_buffer  错误信息缓冲区（调用失败时写入）
 * @param error_size    错误缓冲区大小
 * @return true  - 健康检查完成，结果写入 result_buffer
 * @return false - 调用失败
 */
bool weaknet_health_check(char* result_buffer, size_t result_size, char* error_buffer, size_t error_size);

/**
 * @brief Ping 指定主机（通过当前上网网卡）
 *
 * 通过 D-Bus 调用 Ping 方法，让服务端执行 ICMP Ping 并返回
 * 延迟、丢包率等统计信息。
 *
 * D-Bus 调用：
 *   - Method: Ping
 *   - Args: STRING hostname
 *   - Returns: STRING（包含平均延迟、丢包率的文本）
 *
 * @param hostname      目标主机名或 IP 地址（如 "8.8.8.8" 或 "baidu.com"）
 * @param result_buffer 结果缓冲区，存储 ping 统计文本
 * @param result_size   结果缓冲区大小
 * @param error_buffer  错误信息缓冲区（调用失败时写入）
 * @param error_size    错误缓冲区大小
 * @return true  - Ping 完成，结果写入 result_buffer
 * @return false - 调用失败（主机不可达或 D-Bus 错误）
 */
bool weaknet_ping_host(const char* hostname, char* result_buffer, size_t result_size, char* error_buffer, size_t error_size);

// ============== 版本和状态信息 ==============

/**
 * @brief 获取 WeakNet 客户端库版本信息
 *
 * 本函数不发起 D-Bus 调用，直接返回 "WeakNet Client Library v<X.Y.Z>"。
 * 版本号来自仓库根 VERSION（构建期以宏 WEAKNET_VERSION 注入），与发布包版本
 * 始终一致；此前是硬编码的 "v1.0.0"，与 VERSION 脱钩后无法用于版本判定。
 *
 * @param buffer      结果缓冲区
 * @param buffer_size 缓冲区大小
 * @return true - 固定返回成功
 */
bool weaknet_get_version(char* buffer, size_t buffer_size);

/**
 * @brief 获取库的编译时间和编译选项信息
 *
 * 本函数不发起 D-Bus 调用，使用编译器内置的
 * __DATE__ / __TIME__ 宏构建字符串。
 *
 * @param buffer      结果缓冲区
 * @param buffer_size 缓冲区大小
 * @return true - 固定返回成功
 */
bool weaknet_get_build_info(char* buffer, size_t buffer_size);

// ============== 蓝牙设备 API ==============

/**
 * @brief 获取蓝牙设备列表
 *
 * 通过 D-Bus 调用 GetBluetoothDevices 方法，服务端通过 BlueZ
 * 枚举已发现的蓝牙设备，返回字符串数组。
 *
 * D-Bus 调用：
 *   - Method: GetBluetoothDevices
 *   - Returns: ARRAY of STRING（每行格式: "MAC|Name|RSSI|Connected|Type|Level"）
 *
 * @param buffer       结果缓冲区，多行以 '\n' 分隔
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功（无蓝牙适配器时返回空列表）
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_bluetooth_devices(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 获取蓝牙适配器信息
 *
 * 通过 D-Bus 调用 GetBluetoothAdapter 方法，返回蓝牙适配器
 * 的供电状态、名称、MAC 地址等。
 *
 * D-Bus 调用：
 *   - Method: GetBluetoothAdapter
 *   - Returns: STRING（格式: "Powered:1|Name:xxx|Address:xx:xx:..."）
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_bluetooth_adapter(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 获取指定蓝牙设备的音频质量与 eBPF 融合诊断 JSON
 *
 * 通过 D-Bus 调用 GetBluetoothAudioQuality 方法，返回 A2DP 音频状态、
 * 有效传输速率、卡顿判定及综合质量评分。
 *
 * @param mac          设备 MAC 地址（传入空字符串或 NULL 则服务端自动选取首个设备）
 * @param buffer       结果缓冲区（JSON 格式）
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true 成功, false 失败
 */
bool weaknet_get_bluetooth_audio_quality(const char* mac, char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 获取 Wi-Fi 与蓝牙 2.4GHz 共存干扰根因诊断 JSON
 *
 * 通过 D-Bus 调用 GetCoexistenceConflict 方法，获取当前频段冲突状态与建议。
 *
 * @param buffer       结果缓冲区（JSON 格式）
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true 成功, false 失败
 */
bool weaknet_get_coexistence_conflict(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 获取网络体验权威评估（schema v2 JSON）
 *
 * 只读 AssessmentSnapshot（quality 线程 stabilizer 后的最终结论），
 * 含全部 Core/Service/Active SLE、source/scope/policy_role、
 * primary_issue 与 evidence。绝不重新 evaluate。
 */
bool weaknet_get_network_experience(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 查询端侧确定性机器诊断事实 JSON
 *
 * 通过 D-Bus 调用 GetDiagnosis 方法，返回当前快照经由 C++ DiagnosisEngine
 * 权威判定出的 DiagnosisFacts（包含故障域、首要问题、置信度、因果证据引用、
 * 排除原因及排查动作白名单）。
 *
 * D-Bus 调用：
 *   - Method: GetDiagnosis
 *   - Returns: STRING（DiagnosisFacts JSON）
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_diagnosis(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 执行白名单安全排查动作
 *
 * 通过 D-Bus 调用 ExecuteAction 方法。服务端以 root 权限运行，通过
 * ActionRegistry 严格校验动作白名单及强类型参数，以 posix_spawn 隔离安全执行。
 *
 * D-Bus 调用：
 *   - Method: ExecuteAction
 *   - Args: STRING action_id, STRING param_key, STRING param_val
 *   - Returns: STRING（ExecutionResult JSON）
 *
 * @param action_id    动作标识符（如 "CHECK_RESOLVER_CONFIG"、"PROBE_PUBLIC_RESOLVER"）
 * @param param_key    参数键（如 "resolver"，无参数传 NULL 或空字符串）
 * @param param_val    参数值（如 "223.5.5.5"，无参数传 NULL 或空字符串）
 * @param buffer       结果缓冲区（返回 JSON 执行结果）
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误缓冲区大小
 * @return true  - 成功接收执行结果
 * @return false - 权限不足、参数非法或 D-Bus 调用失败
 */
bool weaknet_execute_action(const char* action_id, const char* param_key, const char* param_val,
                            char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);


/* ============================== eBPF 监控数据 API ============================== */

/**
 * @brief 获取 DNS 监控统计
 *
 * 通过 D-Bus 调用 GetDnsStats 方法，服务端从 eBPF 环形缓冲区
 * 读取 DNS 查询统计。
 *
 * D-Bus 调用：
 *   - Method: GetDnsStats
 *   - Returns: STRING（格式: "totalQueries:N|avgLatencyMs:N|timeoutRate:N%"）
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_dns_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 获取 Wi-Fi 丢包统计
 *
 * 通过 D-Bus 调用 GetWifiLossStats 方法，服务端从 eBPF
 * 读取指定网卡的 TX 丢包率。
 *
 * D-Bus 调用：
 *   - Method: GetWifiLossStats
 *   - Returns: STRING（格式: "ifindex:N rxPkts:N txPkts:N txLossRate:N%"）
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_wifi_loss_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 获取 HTTP 请求延迟统计
 *
 * 通过 D-Bus 调用 GetHttpLatencyStats 方法，服务端从 eBPF
 * 读取 HTTP 请求的 p50/p95/p99 延迟和慢请求分析。
 *
 * D-Bus 调用：
 *   - Method: GetHttpLatencyStats
 *   - Returns: STRING（格式: "totalTxns:N|p50Ms:N|p95Ms:N|p99Ms:N|analysis:xxx"）
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_http_latency_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 获取进程网络画像
 *
 * 通过 D-Bus 调用 GetProcessProfiling 方法，服务端从 eBPF
 * 读取 Top N 带宽占用和重传严重的进程列表。
 *
 * D-Bus 调用：
 *   - Method: GetProcessProfiling
 *   - Returns: STRING（Top 带宽/重传进程列表）
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_process_profiling(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 获取六个 eBPF 监控器的健康与性能快照
 *
 * 通过 D-Bus 调用 GetEbpfMonitorHealth 方法，返回 DNS/Wi-Fi/HTTP/
 * 进程画像等 eBPF 监控器的加载状态、ring buffer 大小、丢包情况。
 *
 * D-Bus 调用：
 *   - Method: GetEbpfMonitorHealth
 *   - Returns: STRING（各监控器健康状态摘要）
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_ebpf_monitor_health(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 获取 Socket/skb 丢包原因精确归因统计（JSON）
 *
 * 通过 D-Bus 调用 GetSkbDropStats 方法，返回由 tracepoint/skb/kfree_skb
 * 采集的各类丢包原因（如 NETFILTER_DROP、NO_SOCKET、CSUM 错误等）聚合数据。
 *
 * D-Bus 调用：
 *   - Method: GetSkbDropStats
 *   - Returns: STRING (JSON 格式丢包归因快照)
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_skb_drop_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 查询被动 TCP RTT 与拥塞统计快照 (JSON)
 *
 * 通过 D-Bus 调用 GetTcpRttStats 方法，返回由内核 tracepoint/tcp/tcp_probe
 * 被动统计的真实连接平滑 RTT（P50/P90/P99、平均值）与连接快照。
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_tcp_rtt_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 查询 TCP 连接级重传统计（JSON）
 *
 * 通过 D-Bus 调用 GetTcpRetransStats 方法，返回全局重传率与
 * 重传最多的连接 Top-N（连接四元组、累计重传/发送段数、丢包率）。
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_tcp_retrans_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 查询 TCP 连接生命周期统计（JSON）
 *
 * 通过 D-Bus 调用 GetTcpConnStats 方法，返回入向连接的
 * accept/close 计数、活跃连接数、连接时长分布与每端口计数。
 *
 * @param buffer       结果缓冲区
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误信息缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_tcp_conn_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/* ============================== 运行时配置 API ============================== */

/**
 * @brief 运行时设置监控器参数
 *
 * 通过 D-Bus 调用 SetMonitorParam 方法，服务端校验白名单、类型、
 * 区间，通过后原子提交到线程安全配置（仅内存态，不写回文件）。
 * 配置键格式："monitor.field"，如 "rtt.interval"、"dns.bpf_obj"。
 *
 * D-Bus 调用：
 *   - Method: SetMonitorParam
 *   - Args: STRING key, STRING value
 *   - Returns: STRING "ok"
 *
 * @param key            配置键（如 "rtt.interval"）
 * @param value          字符串值（如 "5s"、"8.8.8.8"、"true"）
 * @param error_buffer   错误信息缓冲区
 * @param error_size     错误缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败或校验拒绝
 */
bool weaknet_set_monitor_param(const char* key, const char* value,
                               char* error_buffer, size_t error_size);

/**
 * @brief 查询监控器当前参数（返回 JSON）
 *
 * 通过 D-Bus 调用 GetMonitorParam 方法，返回指定监控器的
 * 完整参数 JSON。监控器名如 "rtt"、"dns"，或 "all" 返回全部。
 *
 * D-Bus 调用：
 *   - Method: GetMonitorParam
 *   - Args: STRING monitor
 *   - Returns: STRING（JSON 格式）
 *
 * @param monitor        监控器名（如 "rtt"、"all"）
 * @param buffer         结果缓冲区，存储 JSON
 * @param buffer_size    缓冲区大小
 * @param error_buffer   错误信息缓冲区
 * @param error_size     错误缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败或未知监控器
 */
bool weaknet_get_monitor_param(const char* monitor,
                               char* buffer, size_t buffer_size,
                               char* error_buffer, size_t error_size);

/* ============================== 监控器生命周期 API ============================== */
bool weaknet_list_monitors(char* buffer, size_t buffer_size,
                           char* error_buffer, size_t error_size);
bool weaknet_get_monitor_status(const char* monitor, char* buffer, size_t buffer_size,
                                char* error_buffer, size_t error_size);
bool weaknet_enable_monitor(const char* monitor, char* buffer, size_t buffer_size,
                            char* error_buffer, size_t error_size);
bool weaknet_disable_monitor(const char* monitor, char* buffer, size_t buffer_size,
                             char* error_buffer, size_t error_size);
bool weaknet_restart_monitor(const char* monitor, char* buffer, size_t buffer_size,
                             char* error_buffer, size_t error_size);
bool weaknet_save_monitor_overrides(char* buffer, size_t buffer_size,
                                    char* error_buffer, size_t error_size);

/* Lifecycle commands are available through weaknet-cli monitor ... */

/* ============================== 历史数据 API ============================== */

/**
 * @brief 查询规范化无线设备事件（Canonical Device Event，只读）
 *
 * 通过 D-Bus 调用 QueryDeviceEvents 方法，从事件存储查询断连等
 * 无线设备事件。事件字段含 event_id / site_id / gateway_id / 设备与
 * 地址类型 / event_type / reason 与 raw_reason / source 与证据 / rssi（可为 null）。
 *
 * D-Bus 调用：
 *   - Method: QueryDeviceEvents
 *   - Args: STRING device_address, STRING event_type,
 *           INT64 start_ms, INT64 end_ms, INT32 limit
 *   - Returns: STRING（JSON 数组）
 *
 * 全部过滤参数可传空/0 表示不限制；limit<=0 时服务端钳制为 1，
 * 上限 10000（与 GetHistory 同一防护策略）。
 *
 * @param device_address 设备地址过滤（"" 表示所有设备）
 * @param event_type     事件类型过滤（如 "LINK_DISCONNECTED"；"" 表示不限）
 * @param start_ms       起始时间 Unix 毫秒（0 表示不限）
 * @param end_ms         结束时间 Unix 毫秒（0 表示不限）
 * @param limit          最大返回条数
 * @param buffer         结果缓冲区（JSON 数组）
 * @param buffer_size    缓冲区大小
 * @param error_buffer   错误信息缓冲区
 * @param error_size     错误缓冲区大小
 * @return true 成功；false 调用失败（error_buffer 含原因）
 */
bool weaknet_query_device_events(const char* device_address, const char* event_type,
                                 int64_t start_ms, int64_t end_ms, int32_t limit,
                                 char* buffer, size_t buffer_size,
                                 char* error_buffer, size_t error_size);

/**
 * @brief 查询区域级异常事件 SiteIncident（只读）
 *
 * 与 weaknet_query_device_events 的区别是**抽象层次**：后者回答"哪台设备
 * 发生了什么"（设备层事实），本函数回答"这一片区域是否同时出了问题、
 * 影响了哪些设备"（区域层解释）。
 *
 * 当现场多台设备在关联窗口内同时出现非计划断连/链路劣化时，服务端会产出一条
 * SiteIncident，并保留构成它的每一条 device_event 的可追溯回链。
 *
 * 字段语义：
 *   - `state`             OPEN / ONGOING / RESOLVED（生命周期）
 *   - `started_at_ms`     覆盖的最早异常时刻（不是"发现时刻"）
 *   - `last_event_ms`     最近一次吸收异常的时刻；时间过滤以此为准
 *   - `resolved_at_ms`    结案时刻；仍在活跃时为 null
 *   - `affected_devices`  受影响设备数量（事实）
 *   - `affected_device_ids` 受影响设备地址清单（由回链表派生）
 *   - `suspected_cause`   推断原因，当前恒为 null——关联层只做时空聚合，
 *                         不做根因推断（诊断器是后续阶段的能力）
 *
 * D-Bus 调用：
 *   - Method: QuerySiteIncidents
 *   - Args: STRING state, INT64 start_ms, INT64 end_ms, INT32 limit
 *   - Returns: STRING（JSON 数组）
 *
 * @param state        状态过滤（如 "OPEN"；"" 表示不限）
 * @param start_ms     起始时间 Unix 毫秒（0 表示不限）
 * @param end_ms       结束时间 Unix 毫秒（0 表示不限）
 * @param limit        最大返回条数（服务端上限 10000）
 * @param buffer       结果缓冲区（JSON 数组）
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误缓冲区大小
 * @return true 成功；false 调用失败（error_buffer 含原因）
 */
bool weaknet_query_site_incidents(const char* state,
                                  int64_t start_ms, int64_t end_ms, int32_t limit,
                                  char* buffer, size_t buffer_size,
                                  char* error_buffer, size_t error_size);

/**
 * @brief 查询工业蓝牙 HCI ACL 数据通道历史指标（只读，Phase 1b）
 *
 * 通过 D-Bus 调用 QueryBtDataPath 方法，从 SQLite 获取工业蓝牙数传
 * 链路客观流量时间序列数据。
 *
 * D-Bus 调用：
 *   - Method: QueryBtDataPath
 *   - Args: STRING device_address, INT64 start_ms, INT64 end_ms, INT32 limit
 *   - Returns: STRING（JSON 数组）
 *
 * @param device_address 设备 MAC 地址过滤（空字符串表示不限）
 * @param start_ms       起始时间 Unix 毫秒（0 表示不限）
 * @param end_ms         结束时间 Unix 毫秒（0 表示不限）
 * @param limit          最大返回条数（上限 10000）
 * @param buffer         结果缓冲区
 * @param buffer_size    结果缓冲区大小
 * @param error_buffer   错误信息缓冲区
 * @param error_size     错误信息缓冲区大小
 * @return true 成功；false 失败
 */
bool weaknet_query_bt_data_path(const char* device_address,
                                int64_t start_ms, int64_t end_ms, int32_t limit,
                                char* buffer, size_t buffer_size,
                                char* error_buffer, size_t error_size);

/**
 * @brief 查询 OTA 本地升级状态（只读）
 *
 * 通过 D-Bus 调用 GetUpdateStatus，返回安装根目录的升级布局状态：
 * 已安装版本（current 软链接目标）、布局类型（releases/flat）、
 * 待应用的 staging 版本、当前版本是否已通过健康窗口转正（committed）。
 *
 * D-Bus 调用：
 *   - Method: GetUpdateStatus
 *   - Returns: STRING（JSON）
 *
 * 注意：apply/rollback 不经 D-Bus——升级会重启服务端自身，由
 * weaknet-local-update.sh 在服务外执行（weaknet-cli update 转发）。
 *
 * @param buffer       结果缓冲区（JSON）
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区
 * @param error_size   错误缓冲区大小
 * @return true  - 成功
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_update_status(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 查询历史监控数据
 *
 * 通过 D-Bus 调用 GetHistory 方法，服务端从 SQLite 数据库中
 * 查询网络质量历史快照。
 *
 * D-Bus 调用：
 *   - Method: GetHistory
 *   - Args: STRING interface, STRING start, STRING end, INT32 limit
 *   - Returns: STRING（JSON 数组格式的历史记录）
 *
 * 参数说明：
 *   - interface: 网卡名，空字符串 "" 表示所有网卡
 *   - start:     起始时间 (ISO 8601)，空字符串 "" 表示不限
 *   - end:       结束时间 (ISO 8601)，空字符串 "" 表示不限
 *   - limit:     最大返回行数（如 100）
 *
 * @param interface     网卡名过滤条件
 * @param start         起始时间
 * @param end           结束时间
 * @param limit         最大返回行数
 * @param buffer        结果缓冲区，存储 JSON 数组
 * @param buffer_size   缓冲区大小
 * @param error_buffer  错误信息缓冲区
 * @param error_size    错误信息缓冲区大小
 * @return true  - 成功，buffer 中为 JSON 数组
 * @return false - D-Bus 调用失败
 */
bool weaknet_get_history(const char* interface, const char* start, const char* end,
                         int32_t limit, char* buffer, size_t buffer_size,
                         char* error_buffer, size_t error_size);

// ============== 视频帧（零拷贝 fd 分发） ==============

/**
 * 一帧视频的元数据 + 句柄。
 *
 * ## 为什么不是 JSON 字符串
 *
 * 其余所有 API 都返回 JSON——一帧 1640×1232 的 NV12 是 3MB，序列化成
 * 字符串再解析回来会成为这条路径的瓶颈，而且 D-Bus 的最大消息长度
 * 会直接卡死它。所以帧数据走 `DBUS_TYPE_UNIX_FD`：内核只复制 fd，
 * 像素仍是同一块 dma-buf，接收方 mmap 即可读。
 *
 * ## 所有权（必须读）
 *
 * `weaknet_get_video_frame` 成功时 `fd` 是**调用方自己的**一份副本，
 * 必须由调用方 `close(fd)`。不关就是 fd 泄漏，中枢的槽位也永远不释放。
 *
 * 用完帧后还应调用 `weaknet_release_video_frame(slot)`：
 *   - 不调用也不会永久卡死——中枢按 `slot_count`（默认 3）轮转，
 *     且同一进程再次取帧会自动归还上一次；
 *   - 但会占着槽位不放，中枢少一个可写槽，极端情况下开始跳帧。
 *
 * ## `valid == false` 时
 *
 * 中枢还没出第一帧（摄像头刚启动、或摄像头不在位）是**正常形态**，
 * 不是错误。此时 `fd` 仍是一个有效句柄（空的 memfd），`size == 0`，
 * 调用方应直接 close 并稍后重试，不要 mmap 它。
 */
typedef struct weaknet_video_frame {
    int      fd;            ///< 帧数据句柄，调用方负责 close；valid=false 时也要关
    uint32_t width;         ///< 可见宽度（像素）
    uint32_t height;        ///< 可见高度（像素）
    uint32_t stride;        ///< **Y 平面一行的字节数**，可能不等于 width
    uint32_t uv_offset;     ///< **UV 平面相对帧起始的字节偏移**；0 = 等于 stride*height
    uint32_t fourcc;        ///< V4L2 四字符码，小端打包；NV12 = 0x3231564E
    uint32_t size;          ///< fd 对应的映射长度（Y 平面 + UV 平面），0 表示无帧
    uint64_t sequence;      ///< 采集序号，单调递增；用于判断是否拿到了新帧
    uint64_t timestamp_ns;  ///< 采集时间戳（CLOCK_MONOTONIC，纳秒）
    uint32_t slot;          ///< 中枢槽位号，归还时原样传回；无帧时为 UINT32_MAX
    bool     valid;         ///< true = 有真实帧；false = 无帧（fd 仍须 close）
} weaknet_video_frame;

/**
 * @brief 取一帧当前画面
 *
 * 成功返回 true 时，`out` 被完整填充。**注意**：`valid == false` 也是一种
 * 成功——它表示"服务端正常工作，只是暂时没有帧"，与返回 false（调用本身
 * 失败）是两件不同的事。调用方应先看返回值，再看 `out->valid`。
 *
 * @param out           [out] 帧描述，返回 true 时必须由调用方 close(out->fd)
 * @param error_buffer  [out] 错误信息缓冲区（失败时填写）
 * @param error_size    错误信息缓冲区大小
 * @return true  - 调用成功，检查 out->valid 判断有没有帧
 * @return false - 调用失败（未连接 / 服务端拒绝 / 无法分配句柄）
 */
bool weaknet_get_video_frame(weaknet_video_frame* out,
                             char* error_buffer, size_t error_size);

/**
 * @brief 归还一帧，让中枢可以覆写该槽位
 *
 * 必须传 `weaknet_get_video_frame` 给出的 `slot`。传错槽位会被服务端拒绝
 * （返回 false），不会误释放别人正在读的帧。
 *
 * @param slot          取帧时得到的槽位号
 * @param error_buffer  [out] 错误信息缓冲区
 * @param error_size    错误信息缓冲区大小
 * @return true  - 归还成功
 * @return false - 归还失败（槽位不属于本进程 / 已被自动归还 / 未连接）
 */
bool weaknet_release_video_frame(uint32_t slot,
                                 char* error_buffer, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif // WEAKNET_CLIENT_H
