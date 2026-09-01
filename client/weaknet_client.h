/**
 * @file weaknet_client.h
 * @brief WeakNet 客户端动态库 C API 接口头文件
 *
 * 本文件定义了 WeakNet 客户端动态库对外暴露的 C 接口，
 * 用于让其他 C/C++ 程序方便地集成 WeakNet 网络监控功能。
 *
 * 客户端通过 D-Bus（Session 总线）连接到 WeakNet 服务端：
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
 * 支持的 D-Bus 信号 (Signal):
 *   - Changed                   通用变化信号（旧版兼容）
 *   - InterfaceChanged           网卡添加/删除
 *   - ConnectionModeChanged     当前上网网卡切换
 *   - NetworkQualityChanged     综合网络质量变化
 *   - BluetoothDeviceChanged    蓝牙设备变化
 *
 * 使用流程：
 *   1. weaknet_init()              -- 初始化库并建立 D-Bus 连接
 *   2. weaknet_is_connected()      -- 检查连接是否就绪
 *   3. 调用各种 get_* / *_stats   -- 获取数据
 *   4. weaknet_subscribe_*         -- 订阅事件信号
 *   5. weaknet_check_*             -- 非阻塞轮询事件
 *   6. weaknet_cleanup()           -- 释放资源
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
 * 使用示例：
 *   weaknet_init();                          // 初始化
 *   weaknet_get_interfaces(buf, sz, err, errsz);  // 获取信息
 *   weaknet_subscribe_event("InterfaceChanged", callback);  // 订阅事件
 *   weaknet_check_events(...);                // 检查事件
 *   weaknet_cleanup();                       // 清理
 */

// ============== 库初始化和清理 ==============

/**
 * @brief 初始化 WeakNet 客户端库
 *
 * 必须在调用其他任何 weaknet_* 函数之前调用此函数。
 * 该函数会：
 *   1. 初始化日志系统（输出到 ./logs/client 目录）
 *   2. 连接到 D-Bus Session 总线
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
 * @brief 从序列化文件读取最新状态（离线模式）
 *
 * 不发起 D-Bus 调用，直接读取服务端写入的序列化文件。
 * 适用于服务端未运行但文件仍在的离线场景。
 *
 * @param buffer      结果缓冲区，将存储文件中的内容
 * @param buffer_size 缓冲区大小
 * @param error_buffer 错误信息缓冲区（调用失败时写入）
 * @param error_size   错误缓冲区大小
 * @return true  - 读取成功
 * @return false - 文件不存在或读取失败
 */
bool weaknet_get_from_file(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

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

// ============== 网络状态变化监控 ==============

/**
 * @brief 检查网络状态变化（非阻塞）
 *
 * 从 D-Bus 消息队列中尝试弹出 Changed 信号。
 * 如果没有待处理信号，立即返回 false。
 *
 * D-Bus 信号：
 *   - Signal: Changed (旧版通用信号)
 *   - Payload: STRING message + INT32 counter
 *
 * @param message_buffer 消息缓冲区，存储变化描述文本（输出）
 * @param message_size   消息缓冲区大小
 * @param counter        事件计数器（输出，单调递增）
 * @param error_buffer   错误信息缓冲区（无新变化时写入）
 * @param error_size     错误缓冲区大小
 * @return true  - 检测到新的变化信号
 * @return false - 无新变化或调用出错
 */
bool weaknet_check_changes(char* message_buffer, size_t message_size, int32_t* counter, char* error_buffer, size_t error_size);

// ============== 事件监听和订阅系统 ==============

/**
 * @brief 事件回调函数类型（旧版风格）
 *
 * 用于 weaknet_subscribe_event() 的用户回调。
 *
 * @param event_type 事件类型（如 "InterfaceChanged", "Changed"）
 * @param message    事件消息内容
 * @param counter    事件计数器（服务端单调递增）
 * @param source     事件来源（如 "event_manager"）
 */
typedef void weaknet_event_callback_t(const char* event_type, const char* message, int32_t counter, const char* source);

/**
 * @brief 网络质量事件回调函数类型
 *
 * 用于 weaknet_subscribe_network_quality() 的用户回调。
 * 返回值决定是否继续监听：返回 true 继续，返回 false 停止。
 *
 * @param quality  网络质量等级（如 "poor", "fair", "good", "excellent"）
 * @param details  详细质量信息（JSON 格式的指标数据）
 * @param counter  事件计数器（服务端单调递增）
 * @return true  - 继续监听后续事件
 * @return false - 停止监听
 */
typedef bool (*weaknet_network_quality_callback_t)(const char* quality, const char* details, int32_t counter);

/**
 * @brief 订阅特定 D-Bus 事件（Signal）
 *
 * 通过 dbus_bus_add_match 在 D-Bus 总线上注册匹配规则，
 * 让指定事件类型的信号能被本客户端接收。
 *
 * 支持的事件类型字符串（对应 D-Bus 信号名）：
 *   - "Changed"                通用变化信号
 *   - "InterfaceChanged"       网卡添加/删除
 *   - "ConnectionModeChanged" 上网网卡切换
 *   - "NetworkQualityChanged" 网络质量变化
 *   - "BluetoothDeviceChanged" 蓝牙设备变化
 *
 * @param event_type 要订阅的事件类型字符串
 * @param callback   事件回调函数（可为 NULL，仅添加 D-Bus 订阅不触发用户回调）
 * @return true  - 订阅成功
 * @return false - D-Bus 连接未就绪或添加 match 失败
 */
bool weaknet_subscribe_event(const char* event_type, weaknet_event_callback_t callback);

/**
 * @brief 取消订阅事件
 *
 * 注意：当前实现为简化版本，直接返回 true，
 * 实际 D-Bus match 规则的移除需要更复杂的管理。
 *
 * @param event_type 要取消订阅的事件类型字符串
 * @return true - 固定返回成功
 */
bool weaknet_unsubscribe_event(const char* event_type);

/**
 * @brief 获取支持的事件类型列表
 *
 * 本函数不发起 D-Bus 调用，直接在本地拼接
 * "InterfaceChanged,ConnectionModeChanged,NetworkQualityChanged,BluetoothDeviceChanged"
 * 写入结果缓冲区。
 *
 * @param buffer       结果缓冲区，存储逗号分隔的事件类型列表
 * @param buffer_size  缓冲区大小
 * @param error_buffer 错误信息缓冲区（当前实现不会失败）
 * @param error_size   错误缓冲区大小
 * @return true - 固定返回成功
 */
bool weaknet_get_event_types(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);

/**
 * @brief 非阻塞检查事件（多信号通用）
 *
 * 从 D-Bus 消息队列中尝试弹出以下任一信号：
 *   - InterfaceChanged / ConnectionModeChanged
 *   - NetworkQualityChanged / BluetoothDeviceChanged
 *
 * D-Bus 信号 Payload（统一格式）：
 *   STRING text + INT32 counter
 *
 * @param event_type_buffer 事件类型缓冲区（输出，如 "InterfaceChanged"）
 * @param event_type_size   事件类型缓冲区大小
 * @param message_buffer    消息缓冲区（输出，变化描述文本）
 * @param message_size      消息缓冲区大小
 * @param counter           事件计数器（输出）
 * @param source_buffer     事件来源缓冲区（输出，固定为 "event_manager"）
 * @param source_size       来源缓冲区大小
 * @param error_buffer      错误信息缓冲区（无事件时写入）
 * @param error_size        错误缓冲区大小
 * @return true  - 检测到一个事件
 * @return false - 队列为空或发生错误
 */
bool weaknet_check_events(char* event_type_buffer, size_t event_type_size,
                          char* message_buffer, size_t message_size,
                          int32_t* counter, char* source_buffer, size_t source_size,
                          char* error_buffer, size_t error_size);

// ============== 网络质量监控 ==============

/**
 * @brief 订阅网络质量事件（阻塞监听模式）
 *
 * 内部会阻塞在 D-Bus 消息循环中，持续监听 NetworkQualityChanged 信号。
 * 每次收到信号后调用用户提供的 callback。
 * 当 callback 返回 false 时退出监听循环。
 *
 * D-Bus 信号：
 *   - Signal: NetworkQualityChanged
 *   - Payload: STRING quality + STRING details + INT32 counter
 *
 * @param callback 网络质量事件回调函数，返回值控制是否继续监听
 * @return true  - 监听循环正常退出
 * @return false - 订阅失败（客户端未连接）
 */
bool weaknet_subscribe_network_quality(weaknet_network_quality_callback_t callback);

/**
 * @brief 非阻塞检查网络质量事件
 *
 * 从 D-Bus 消息队列中尝试弹出 NetworkQualityChanged 信号。
 *
 * D-Bus 信号 Payload：
 *   STRING quality ("Poor"/"Fair"/"Good"/"Excellent")
 *   STRING details (JSON 格式指标详情)
 *   INT32 counter
 *
 * @param quality_buffer  网络质量等级缓冲区（输出）
 * @param quality_size    质量等级缓冲区大小
 * @param details_buffer   详细质量信息缓冲区（输出，JSON）
 * @param details_size     详细信息缓冲区大小
 * @param counter          事件计数器（输出）
 * @param error_buffer     错误信息缓冲区（无事件时写入）
 * @param error_size       错误缓冲区大小
 * @return true  - 检测到网络质量事件
 * @return false - 队列为空或发生错误
 */
bool weaknet_check_network_quality(char* quality_buffer, size_t quality_size,
                                   char* details_buffer, size_t details_size, 
                                   int32_t* counter, char* error_buffer, size_t error_size);

// ============== 版本和状态信息 ==============

/**
 * @brief 获取 WeakNet 客户端库版本信息
 *
 * 本函数不发起 D-Bus 调用，直接返回硬编码版本字符串
 * "WeakNet Client Library v1.0.0"。
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
 * @brief 订阅蓝牙设备变化事件
 *
 * 通过 D-Bus add_match 订阅 BluetoothDeviceChanged 信号。
 * 当前实现只添加 D-Bus 匹配规则，不进入阻塞监听循环。
 * 实际事件通过 weaknet_check_events() 轮询获取。
 *
 * D-Bus 信号：
 *   - Signal: BluetoothDeviceChanged
 *   - Payload: STRING message + INT32 counter
 *
 * @param callback 事件回调函数（可为 NULL，当前版本回调暂未生效）
 * @return true  - 订阅成功
 * @return false - 客户端未连接
 */
bool weaknet_subscribe_bluetooth_events(weaknet_event_callback_t callback);

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

/* ============================== 历史数据 API ============================== */

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

#ifdef __cplusplus
}
#endif

#endif // WEAKNET_CLIENT_H
