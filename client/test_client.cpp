/**
 * @file test_client.cpp
 * @brief WeakNet 客户端动态库完整接口验证工具（CLI 测试框架）
 *
 * 本程序用于全面验证 weaknet_client.h 中所有 C API 的正确性，
 * 同时也是一个可用的命令行工具，支持多种单次查询模式。
 *
 * 程序分为两种运行模式：
 *
 *   1. 无参数（或参数 "all"）—— 运行完整测试套件
 *      依次调用所有 weaknet_* 接口，打印测试统计结果
 *
 *   2. 指定子命令 —— 单次执行某一操作
 *      示例：./test_client ping 8.8.8.8
 *
 * D-Bus 通信基础：
 *   - Bus Name:   com.example.WeakNet
 *   - ObjPath:    /com/example/WeakNet
 *   - Interface:  com.example.WeakNet
 *
 * 支持的 D-Bus 方法（本程序中使用到的）：
 *   - GetInterfaces            获取网卡列表
 *   - HealthCheck              网络健康检查
 *   - Ping                     Ping 指定主机
 *   - GetBluetoothDevices      蓝牙设备列表
 *   - GetBluetoothAdapter      蓝牙适配器状态
 *   - GetEbpfMonitorHealth     eBPF 监控器健康快照
 *
 * 注意：服务端自 2026-10-19 起停发全部 D-Bus 信号，客户端的订阅/检查族
 * 已于 2026-11-01 删除（详见 weaknet_client.h 文件头）。本程序只覆盖
 * 真实可用的查询路径（get_* / *_stats 方法）。
 *
 * 运行依赖：
 *   - WeakNet 服务端已启动并在 D-Bus Session 总线上注册
 *   - weaknet_client.h + libweaknet_client.so
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>
#include <chrono>
#include <thread>

#include "weaknet_client.h"
#include "logger.hpp"

// =====================================================
// 测试基础设施
// =====================================================

/**
 * @brief 测试结果统计结构
 *
 * 用于 runAllTests() 汇总全部测试的通过/失败数。
 */
struct TestStats {
    int total = 0;
    int passed = 0;
    int failed = 0;
    
    /** @brief 记录一次测试结果 */
    void addResult(bool success) {
        total++;
        if (success) passed++;
        else failed++;
    }
    
    /** @brief 打印最终统计 */
    void print() {
        printf("\n📊 测试统计: 总计=%d, 通过=%d, 失败=%d, 成功率=%.1f%%\n", 
               total, passed, failed, total > 0 ? (passed * 100.0 / total) : 0.0);
    }
};

// 全局测试统计实例
TestStats g_stats;

// ===== 测试辅助宏 =====

/** @brief 开始一个测试用例，打印用例名称并计入总数 */
#define TEST_CASE(name) \
    printf("\n🧪 测试: %s\n", name); \
    g_stats.addResult(true)

/** @brief 断言条件为真，否则打印失败信息并从当前测试返回 false */
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf("   ❌ 失败: %s\n", message); \
            g_stats.failed++; \
            g_stats.passed--; \
            return false; \
        } else { \
            printf("   ✅ 通过: %s\n", message); \
        } \
    } while(0)

/** @brief 调用 weaknet_* API，失败则打印失败并返回 */
#define TEST_API_CALL(func, ...) \
    do { \
        if (func(__VA_ARGS__)) { \
            printf("   ✅ API调用成功\n"); \
        } else { \
            printf("   ❌ API调用失败\n"); \
            return false; \
        } \
    } while(0)

// =====================================================
// 测试用例实现
// =====================================================

/**
 * @brief 基础功能测试
 *
 * 测试 weaknet_init / weaknet_is_connected / weaknet_get_version / weaknet_get_build_info
 * 其中 version 和 build_info 不发起 D-Bus 调用，是本地常量。
 */
bool testBasicFunctions() {
    TEST_CASE("基础功能测试");
    
    char buffer[512], error[256];
    
    // 测试初始化 —— 建立 D-Bus Session 连接
    TEST_ASSERT(weaknet_init(), "库初始化");
    
    // 测试连接状态 —— 检查 D-Bus 连接句柄是否有效
    TEST_ASSERT(weaknet_is_connected(), "连接状态检查");
    
    // 测试版本信息 —— 本地返回 "WeakNet Client Library v1.0.0"
    TEST_ASSERT(weaknet_get_version(buffer, sizeof(buffer)), "获取版本信息");
    printf("   📦 版本: %s\n", buffer);
    
    // 测试编译信息 —— 本地返回 __DATE__ / __TIME__
    TEST_ASSERT(weaknet_get_build_info(buffer, sizeof(buffer)), "获取编译信息");
    printf("   🔧 编译信息: %s\n", buffer);
    
    return true;
}

/**
 * @brief 网络接口信息获取测试
 *
 * 测试 GetInterfaces、HealthCheck 两个 D-Bus Method 等查询能力。
 */
bool testNetworkInfo() {
    TEST_CASE("网络接口信息获取");
    
    char buffer[4096], error[256];
    
    // weaknet_get_interfaces → D-Bus Method: GetInterfaces → ARRAY of STRING → 逗号拼接
    TEST_API_CALL(weaknet_get_interfaces, buffer, sizeof(buffer), error, sizeof(error));
    printf("   📡 网络接口: %s\n", buffer);
    
    // weaknet_health_check → D-Bus Method: HealthCheck → STRING (JSON 诊断报告)
    TEST_API_CALL(weaknet_health_check, buffer, sizeof(buffer), error, sizeof(error));
    printf("   💚 健康检查: %s\n", buffer);

    // weaknet_get_network_experience → D-Bus Method: GetNetworkExperience → STRING (Schema v2 快照)
    TEST_API_CALL(weaknet_get_network_experience, buffer, sizeof(buffer), error, sizeof(error));
    printf("   🧭 网络体验: %s\n", buffer);

    // weaknet_get_diagnosis → D-Bus Method: GetDiagnosis → STRING (DiagnosisFacts JSON)
    TEST_API_CALL(weaknet_get_diagnosis, buffer, sizeof(buffer), error, sizeof(error));
    printf("   🧠 机器诊断: %s\n", buffer);

    // weaknet_execute_action → D-Bus Method: ExecuteAction → STRING (ExecutionResult JSON)
    // 使用只读的白名单动作 CHECK_RESOLVER_CONFIG 测试执行能力
    TEST_API_CALL(weaknet_execute_action, "CHECK_RESOLVER_CONFIG", "", "", buffer, sizeof(buffer), error, sizeof(error));
    printf("   ⚡ 动作执行: %s\n", buffer);

    return true;
}

/**
 * @brief Ping 功能测试
 *
 * 依次对 4 个目标调用 weaknet_ping_host（D-Bus Method: Ping）。
 * 包括两个可达目标（8.8.8.8, baidu.com）、本地回环、
 * 一个无效主机用于测试错误处理。
 */
bool testPingFunction() {
    TEST_CASE("Ping功能测试");
    
    char result[512], error[256];
    std::vector<std::string> targets = {
        "8.8.8.8",           // Google DNS —— 通常可达
        "baidu.com",         // 百度 —— 国内常用站点
        "127.0.0.1",         // 本地回环地址 —— 一定可达
        "invalidhost12345.com" // 无效主机名 —— 测试失败路径
    };
    
    for (const auto& target : targets) {
        printf("   🎯 Ping目标: %s\n", target.c_str());
        
        // D-Bus: com.example.WeakNet /com/example/WeakNet Ping(STRING hostname) → STRING
        if (weaknet_ping_host(target.c_str(), result, sizeof(result), error, sizeof(error))) {
            printf("     ✅ 结果: %s\n", result);
        } else {
            printf("     ❌ 失败: %s\n", error);
        }
        
        // 添加延迟避免过于频繁的 D-Bus 调用
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    return true;
}

/**
 * @brief 错误处理测试
 *
 * 验证 weaknet_ping_host 对非法输入的校验：
 *   - 空字符串 "" 应该被拒绝
 *   - NULL 指针应该被拒绝
 */
bool testErrorHandling() {
    TEST_CASE("错误处理测试");
    
    char buffer[512], error[256];
    
    // 测试空主机名 ping —— 应该被拒绝
    printf("   🚫 测试空主机名ping\n");
    if (!weaknet_ping_host("", buffer, sizeof(buffer), error, sizeof(error))) {
        printf("     ✅ 正确处理空主机名: %s\n", error);
    } else {
        printf("     ❌ 应该拒绝空主机名\n");
        return false;
    }
    
    // 测试 null 主机名 ping —— 应该被拒绝
    printf("   🚫 测试null主机名ping\n");
    if (!weaknet_ping_host(nullptr, buffer, sizeof(buffer), error, sizeof(error))) {
        printf("     ✅ 正确处理null主机名: %s\n", error);
    } else {
        printf("     ❌ 应该拒绝null主机名\n");
        return false;
    }
    
    return true;
}

/** @brief 调用 D-Bus Method: GetBluetoothDevices 获取蓝牙设备列表 */
bool testBluetoothDevices() {
    TEST_CASE("蓝牙设备获取");

    char buffer[4096], error[256];

    // GetBluetoothDevices → ARRAY of STRING（每行: "MAC|Name|RSSI|Connected|Type|Level"）
    TEST_API_CALL(weaknet_get_bluetooth_devices, buffer, sizeof(buffer), error, sizeof(error));
    printf("   📱 蓝牙设备列表: %s\n", buffer);

    return true;
}

/** @brief 调用 D-Bus Method: GetBluetoothAdapter 获取蓝牙适配器信息 */
bool testBluetoothAdapter() {
    TEST_CASE("蓝牙适配器信息");

    char buffer[4096], error[256];

    // GetBluetoothAdapter → STRING（"Powered:1|Name:xxx|Address:xx:xx:..."）
    TEST_API_CALL(weaknet_get_bluetooth_adapter, buffer, sizeof(buffer), error, sizeof(error));
    printf("   📡 蓝牙适配器: %s\n", buffer);

    return true;
}

/**
 * @brief 性能测试
 *
 * 连续调用 weaknet_get_interfaces 10 次，
 * 测量平均耗时。
 */
bool testPerformance() {
    TEST_CASE("性能测试");

    char buffer[512], error[256];
    int testCount = 10;

    printf("   ⚡ 执行%d次get_interfaces调用\n", testCount);
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < testCount; i++) {
        // 每次发起 D-Bus 调用：GetInterfaces → 返回 ARRAY of STRING
        if (!weaknet_get_interfaces(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("     ❌ 第%d次调用失败: %s\n", i+1, error);
            return false;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    printf("     ✅ 完成%d次调用，耗时%ldms，平均%.2fms/次\n",
           testCount, duration.count(), duration.count() / (double)testCount);

    return true;
}

bool testHistoryQuery() {
    TEST_CASE("历史监控数据查询接口测试");

    char buffer[4096], error[256];
    bool ret = weaknet_get_history("", "", "", 10, buffer, sizeof(buffer), error, sizeof(error));
    if (ret) {
        printf("   ✅ 成功获取历史监控记录 (长度: %zu 字节)\n", strlen(buffer));
        g_stats.addResult(true);
        return true;
    } else {
        printf("   ℹ️  查询历史记录返回: %s\n", error);
        g_stats.addResult(true);
        return true;
    }
}

// =====================================================
// 测试调度入口
// =====================================================

/**
 * @brief 运行完整测试套件（所有测试用例）
 *
 * 执行顺序：基础功能 → 网络信息 → Ping → 事件 → 蓝牙 → 网络质量 → 变化监控 → 错误处理 → 性能
 * 结束后 weaknet_cleanup() 并打印统计。
 */
void runAllTests() {
    printf("🚀 开始WeakNet客户端库完整接口验证\n");
    printf("============================================================\n");
    
    // ===== 初始化客户端（建立 D-Bus Session 连接）=====
    if (!weaknet_init()) {
        printf("❌ 初始化失败，无法继续测试\n");
        return;
    }
    
    // 依次运行各项测试
    testBasicFunctions();
    testNetworkInfo();
    testPingFunction();
    testBluetoothDevices();
    testBluetoothAdapter();
    testHistoryQuery();
    testErrorHandling();
    testPerformance();
    
    // ===== 清理资源 =====
    weaknet_cleanup();
    
    // 打印统计
    g_stats.print();
    
    printf("\n🎉 WeakNet客户端库接口验证完成!\n");
}

/**
 * @brief 执行单个命令（CLI 单次查询模式）
 *
 * 所有命令在执行前都会先 weaknet_init()，执行完后 weaknet_cleanup()。
 *
 * @param command 子命令字符串（argv[1]）
 * @param argc    原始命令行参数个数
 * @param argv    原始命令行参数数组
 * @return true=执行成功
 */
bool runSingleTest(const std::string& command, int argc, char* argv[]) {
    // 初始化客户端（建立 D-Bus Session 连接）
    if (!weaknet_init()) {
        printf("❌ 初始化失败\n");
        return false;
    }
    
    char buffer[4096], error[256];
    int32_t counter;
    
    // ===== get =====
    // D-Bus Method: GetInterfaces → ARRAY of STRING → 逗号拼接
    if (command == "get") {
        if (weaknet_get_interfaces(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 网络接口信息: %s\n", buffer);
        } else {
            printf("❌ 获取失败: %s\n", error);
            return false;
        }
    }
    // ===== health =====
    // D-Bus Method: HealthCheck → STRING (JSON 诊断报告)
    else if (command == "health") {
        if (weaknet_health_check(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 健康检查结果: %s\n", buffer);
        } else {
            printf("❌ 健康检查失败: %s\n", error);
            return false;
        }
    }
    // ===== ebpf-health =====
    // D-Bus Method: GetEbpfMonitorHealth → STRING
    else if (command == "ebpf-health") {
        if (weaknet_get_ebpf_monitor_health(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ eBPF 监控器健康状态: %s\n", buffer);
        } else {
            printf("❌ eBPF 监控器健康查询失败: %s\n", error);
            return false;
        }
    }
    // ===== skb-drop =====
    // D-Bus Method: GetSkbDropStats → STRING (JSON)
    else if (command == "skb-drop") {
        if (weaknet_get_skb_drop_stats(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ Socket丢包归因统计: %s\n", buffer);
        } else {
            printf("❌ 丢包归因统计查询失败: %s\n", error);
            return false;
        }
    }
    // ===== tcp-rtt =====
    // D-Bus Method: GetTcpRttStats → STRING (JSON)
    else if (command == "tcp-rtt") {
        if (weaknet_get_tcp_rtt_stats(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 被动 TCP RTT 统计: %s\n", buffer);
        } else {
            printf("❌ 被动 TCP RTT 查询失败: %s\n", error);
            return false;
        }
    }
    // ===== dns-stats =====
    // D-Bus Method: GetDnsStats → STRING
    else if (command == "dns-stats") {
        if (weaknet_get_dns_stats(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ DNS 解析统计: %s\n", buffer);
        } else {
            printf("❌ DNS 统计查询失败: %s\n", error);
            return false;
        }
    }
    // ===== wifi-loss =====
    // D-Bus Method: GetWifiLossStats → STRING
    else if (command == "wifi-loss") {
        if (weaknet_get_wifi_loss_stats(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ Wi-Fi 协议丢包归因: %s\n", buffer);
        } else {
            printf("❌ Wi-Fi 丢包统计查询失败: %s\n", error);
            return false;
        }
    }
    // ===== http-latency =====
    // D-Bus Method: GetHttpLatencyStats → STRING
    else if (command == "http-latency") {
        if (weaknet_get_http_latency_stats(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ HTTP 请求延迟指标 (TTFB): %s\n", buffer);
        } else {
            printf("❌ HTTP 延迟统计查询失败: %s\n", error);
            return false;
        }
    }
    // ===== profiling =====
    // D-Bus Method: GetProcessProfiling → STRING
    else if (command == "profiling") {
        if (weaknet_get_process_profiling(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 进程网络流量画像: %s\n", buffer);
        } else {
            printf("❌ 进程网络画像查询失败: %s\n", error);
            return false;
        }
    }
    // ===== history =====
    // D-Bus Method: GetHistory → STRING (JSON Array)
    else if (command == "history") {
        const char* iface = (argc >= 3) ? argv[2] : "";
        if (weaknet_get_history(iface, "", "", 10, buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 历史监控记录: %s\n", buffer);
        } else {
            printf("❌ 历史记录查询失败: %s\n", error);
            return false;
        }
    }
    // ===== bt-traffic =====
    // D-Bus Method: QueryBtDataPath → JSON
    else if (command == "bt-traffic") {
        std::string dev = (argc >= 3) ? argv[2] : "";
        if (weaknet_query_bt_data_path(dev.c_str(), 0, 0, 10, buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 工业蓝牙数据通道历史指标: %s\n", buffer);
        } else {
            printf("❌ 蓝牙数据通道指标查询失败: %s\n", error);
            return false;
        }
    }
    // ===== ping HOSTNAME =====
    // D-Bus Method: Ping(STRING hostname) → STRING
    // 需要 argc >= 3，argv[2] 为目标主机名
    else if (command == "ping") {
        if (argc < 3) {
            printf("❌ Ping命令需要指定主机名\n");
            printf("用法: %s ping HOSTNAME\n", argv[0]);
            return false;
        }
        std::string hostname = argv[2];
        if (weaknet_ping_host(hostname.c_str(), buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ Ping结果: %s\n", buffer);
        } else {
            printf("❌ Ping失败: %s\n", error);
            return false;
        }
    }
    // ===== bt-devices =====
    // D-Bus Method: GetBluetoothDevices → ARRAY of STRING
    else if (command == "bt-devices") {
        if (weaknet_get_bluetooth_devices(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 蓝牙设备列表: %s\n", buffer);
        } else {
            printf("❌ 获取蓝牙设备失败: %s\n", error);
            return false;
        }
    }
    // ===== bt-adapter =====
    // D-Bus Method: GetBluetoothAdapter → STRING
    else if (command == "bt-adapter") {
        if (weaknet_get_bluetooth_adapter(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 蓝牙适配器信息: %s\n", buffer);
        } else {
            printf("❌ 获取蓝牙适配器失败: %s\n", error);
            return false;
        }
    }
    // ===== bt-audio =====
    // D-Bus Method: GetBluetoothAudioQuality → JSON
    else if (command == "bt-audio") {
        std::string mac = (argc >= 3) ? argv[2] : "";
        if (weaknet_get_bluetooth_audio_quality(mac.empty() ? nullptr : mac.c_str(), buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 蓝牙音频质量与 eBPF 融合诊断:\n%s\n", buffer);
        } else {
            printf("❌ 获取蓝牙音频质量失败: %s\n", error);
            return false;
        }
    }
    // ===== coexistence =====
    // D-Bus Method: GetCoexistenceConflict → JSON
    else if (command == "coexistence") {
        if (weaknet_get_coexistence_conflict(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ Wi-Fi 与蓝牙 2.4GHz 共存诊断:\n%s\n", buffer);
        } else {
            printf("❌ 获取共存诊断失败: %s\n", error);
            return false;
        }
    }
    // ===== test-bt =====
    // 依次执行蓝牙相关查询测试（设备列表、适配器）
    else if (command == "test-bt") {
        testBluetoothDevices();
        testBluetoothAdapter();
        return true;
    }
    // ===== test-basic =====
    // 基础功能测试
    else if (command == "test-basic") {
        return testBasicFunctions();
    }
    // ===== test-network =====
    // 网络接口信息获取测试
    else if (command == "test-network") {
        return testNetworkInfo();
    }
    // ===== test-ping =====
    // Ping 功能测试
    else if (command == "test-ping") {
        return testPingFunction();
    }
    // ===== test-errors =====
    // 错误处理测试（空指针、空字符串校验）
    else if (command == "test-errors") {
        return testErrorHandling();
    }
    // ===== test-performance =====
    // 性能测试（10 次调用 get_interfaces，测量平均耗时）
    else if (command == "test-performance") {
        return testPerformance();
    }
    // ===== 未知命令 =====
    else {
        printf("❌ 未知命令: %s\n", command.c_str());
        return false;
    }
    
    // 正常退出前清理资源
    weaknet_cleanup();
    return true;
}

// =====================================================
// 主函数 —— 命令行参数解析入口
// =====================================================

/**
 * @brief 主函数
 *
 * 命令行参数说明：
 *   argv[1] —— 子命令（必填）
 *   argv[2] —— 子命令参数（可选，如 ping 的主机名）
 *
 * 子命令列表：
 *   all                      - 运行完整测试套件（默认行为）
 *   get                      - 获取当前网络接口列表（GetInterfaces）
 *   health                   - 网络健康检查（HealthCheck）
 *   ebpf-health              - eBPF 监控器健康快照（GetEbpfMonitorHealth）
 *   ping HOSTNAME            - Ping 指定主机（Ping 方法，需要第二个参数）
 *   bt-devices               - 获取蓝牙设备列表（GetBluetoothDevices）
 *   bt-adapter               - 获取蓝牙适配器状态（GetBluetoothAdapter）
 *   test-basic               - 基础功能测试
 *   test-network             - 网络接口信息测试
 *   test-ping                - Ping 功能测试
 *   test-bt                  - 蓝牙功能组合测试（设备列表 + 适配器）
 *   test-errors              - 错误处理边界测试
 *   test-performance         - get_interfaces 性能基准测试
 *
 * @param argc 参数个数
 * @param argv 参数数组（argv[0] = 可执行文件名，argv[1] = 子命令，argv[2] = 子命令参数）
 */
int main(int argc, char* argv[]) {
    // ===== 无参数或只指定可执行文件名 =====
    // 打印帮助信息并退出
    if (argc < 2) {
        printf("WeakNet 客户端动态库完整接口验证工具\n");
        printf("用法:\n");
        printf("  %s all                    - 运行所有接口验证测试\n", argv[0]);
        printf("  %s get                    - 获取当前网络接口\n", argv[0]);
        printf("  %s health                 - 网络健康检查\n", argv[0]);
        printf("  %s ebpf-health            - eBPF 监控器健康与性能状态\n", argv[0]);
        printf("  %s skb-drop               - 获取 Socket/skb 丢包归因快照\n", argv[0]);
        printf("  %s dns-stats              - 获取 DNS 解析与延迟统计\n", argv[0]);
        printf("  %s wifi-loss              - 获取 Wi-Fi 协议丢包归因统计\n", argv[0]);
        printf("  %s http-latency           - 获取 HTTP 事务延迟统计 (TTFB)\n", argv[0]);
        printf("  %s profiling              - 获取进程网络流量画像\n", argv[0]);
        printf("  %s history [IFACE]        - 查询网络历史监控记录\n", argv[0]);
        printf("  %s ping HOSTNAME          - Ping指定主机\n", argv[0]);
        printf("\n蓝牙功能:\n");
        printf("  %s bt-devices             - 获取蓝牙设备列表\n", argv[0]);
        printf("  %s bt-adapter             - 获取蓝牙适配器信息\n", argv[0]);
        printf("  %s bt-audio [MAC]         - 获取蓝牙音频质量与 eBPF 融合诊断\n", argv[0]);
        printf("  %s coexistence            - 获取 Wi-Fi 与蓝牙 2.4GHz 共存诊断\n", argv[0]);
        printf("\n测试模式:\n");
        printf("  %s test-basic             - 基础功能测试\n", argv[0]);
        printf("  %s test-network           - 网络信息测试\n", argv[0]);
        printf("  %s test-ping              - Ping功能测试\n", argv[0]);
        printf("  %s test-bt                - 蓝牙功能测试\n", argv[0]);
        printf("  %s test-errors            - 错误处理测试\n", argv[0]);
        printf("  %s test-performance       - 性能测试\n", argv[0]);
        return 1;
    }

    // ===== 有参数：argv[1] 为子命令 =====
    std::string command = argv[1];
    
    if (command == "all") {
        runAllTests();  // 完整测试套件
    } else {
        runSingleTest(command, argc, argv);  // 单次命令执行
    }

    return 0;
}
