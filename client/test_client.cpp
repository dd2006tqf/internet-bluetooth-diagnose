/**
 * @file test_client.cpp
 * @brief WeakNet 客户端动态库完整接口验证工具（CLI 测试框架）
 *
 * 本程序用于全面验证 weaknet_client.h 中所有 C API 的正确性，
 * 同时也是一个可用的命令行工具，支持多种单次查询和持续监听模式。
 *
 * 程序分为两种运行模式：
 *
 *   1. 无参数（或参数 "all"）—— 运行完整测试套件
 *      依次调用所有 weaknet_* 接口，打印测试统计结果
 *
 *   2. 指定子命令 —— 单次执行某一操作
 *      示例：./test_client ping 8.8.8.8
 *           ./test_client events
 *           ./test_client subscribe
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
 * 支持的 D-Bus 信号（本程序中订阅/检查的）：
 *   - Changed                  通用变化信号
 *   - InterfaceChanged         网卡变化
 *   - ConnectionModeChanged    上网网卡切换
 *   - NetworkQualityChanged    网络质量变化
 *   - BluetoothDeviceChanged   蓝牙设备变化
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
 * 测试 GetInterfaces、HealthCheck 两个 D-Bus Method，
 * 以及从序列化文件读取的离线路径。
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
    
    // weaknet_get_from_file —— 离线模式，不发起 D-Bus 调用，直接读取服务端序列化文件
    if (weaknet_get_from_file(buffer, sizeof(buffer), error, sizeof(error))) {
        printf("   📄 文件内容: %s\n", buffer);
    } else {
        printf("   ℹ️  文件读取: %s\n", error);
    }
    
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
 * @brief 事件系统测试
 *
 * 测试流程：
 *   1. weaknet_get_event_types —— 本地拼接 4 种事件类型字符串
 *   2. weaknet_subscribe_event —— 通过 dbus_bus_add_match 注册 D-Bus 信号匹配规则
 *   3. weaknet_check_events —— 每秒钟非阻塞检查一次 D-Bus 信号队列（持续 5 秒）
 *   4. weaknet_unsubscribe_event —— 取消订阅（简化版本，固定返回 true）
 *
 * D-Bus 匹配规则示例：type='signal',interface='com.example.WeakNet',member='InterfaceChanged'
 */
bool testEventSystem() {
    TEST_CASE("事件系统测试");
    
    char buffer[512], error[256];
    
    // 获取事件类型列表 —— 本地拼接字符串
    TEST_API_CALL(weaknet_get_event_types, buffer, sizeof(buffer), error, sizeof(error));
    printf("   📋 支持的事件类型: %s\n", buffer);
    
    // 订阅三类事件 —— 添加 D-Bus match 规则
    std::vector<std::string> eventTypes = {
        "InterfaceChanged",           // 网卡添加/删除
        "ConnectionModeChanged",      // 当前上网网卡切换
        "NetworkQualityChanged"      // 综合网络质量变化
    };
    
    for (const auto& eventType : eventTypes) {
        printf("   🔔 订阅事件: %s\n", eventType.c_str());
        TEST_API_CALL(weaknet_subscribe_event, eventType.c_str(), nullptr);
    }
    
    // 持续 5 秒每秒检查一次事件队列
    // weaknet_check_events 内部执行 dbus_connection_read_write(conn, 0) + pop_message
    // 检查四类信号：InterfaceChanged / ConnectionModeChanged / NetworkQualityChanged / BluetoothDeviceChanged
    printf("   🔍 检查事件 (5秒)...\n");
    char eventType[64], message[512], source[64];
    int32_t counter;
    
    for (int i = 0; i < 5; i++) {
        if (weaknet_check_events(eventType, sizeof(eventType), message, sizeof(message),
                                 &counter, source, sizeof(source), error, sizeof(error))) {
            printf("     🎯 检测到事件: type=%s counter=%d source=%s message=%s\n", 
                   eventType, counter, source, message);
        } else {
            printf("     ⏳ 第%d秒: 无事件\n", i+1);
        }
        sleep(1);
    }
    
    // 取消订阅 —— 当前实现简化，固定返回 true
    for (const auto& eventType : eventTypes) {
        printf("   🔕 取消订阅: %s\n", eventType.c_str());
        TEST_API_CALL(weaknet_unsubscribe_event, eventType.c_str());
    }
    
    return true;
}

/**
 * @brief 网络质量事件监听测试（非阻塞轮询）
 *
 * 通过 weaknet_check_network_quality() 每秒钟检查一次
 * NetworkQualityChanged 信号队列（持续 10 秒）。
 *
 * D-Bus 信号 Payload:
 *   STRING quality + STRING details + INT32 counter
 */
bool testNetworkQualityEvents() {
    TEST_CASE("网络质量事件监听测试");
    
    char quality[256], details[1024], error[256];
    int32_t counter;
    
    // 先通过 weaknet_subscribe_event 添加 NetworkQualityChanged 的 D-Bus 订阅
    printf("   🔔 订阅网络质量事件...\n");
    if (weaknet_subscribe_event("NetworkQualityChanged", nullptr)) {
        printf("     ✅ 网络质量事件订阅成功\n");
    } else {
        printf("     ❌ 网络质量事件订阅失败\n");
        return false;
    }
    
    // 非阻塞检查网络质量事件（持续 10 秒）
    printf("   🔍 检查网络质量事件 (10秒)...\n");
    for (int i = 0; i < 10; i++) {
        // weaknet_check_network_quality 内部：
        //   dbus_connection_read_write(conn, 0) + pop_message + 解析 NetworkQualityChanged 信号的三个参数
        if (weaknet_check_network_quality(quality, sizeof(quality), details, sizeof(details),
                                             &counter, error, sizeof(error))) {
            printf("     🎯 检测到网络质量事件:\n");
            printf("       质量等级: %s\n", quality);
            printf("       详细信息: %s\n", details);
            printf("       事件计数: %d\n", counter);
        } else {
            printf("     ⏳ 第%d秒: 无网络质量事件 (%s)\n", i+1, error);
        }
        sleep(1);
    }
    
    return true;
}

/**
 * @brief 网络质量事件回调测试（阻塞模式）
 *
 * 通过 weaknet_subscribe_network_quality(callback) 进入阻塞监听循环。
 * 回调函数使用静态变量计数，收到 3 个事件后返回 false 停止监听。
 *
 * 注意：subscribe_to_network_quality 内部会进入 while(true) 循环，
 * 只有当 callback 返回 false 时才会退出。
 */
bool testNetworkQualityCallback() {
    TEST_CASE("网络质量事件回调测试");
    
    // 定义回调函数（使用 lambda 转换为 weaknet_network_quality_callback_t）
    static int callback_count = 0;
    auto quality_callback = [](const char* quality, const char* details, int32_t counter) -> bool {
        callback_count++;
        printf("     🎯 回调收到网络质量事件 #%d:\n", callback_count);
        printf("       质量等级: %s\n", quality);
        printf("       详细信息: %s\n", details);
        printf("       事件计数: %d\n", counter);
        
        // 收到 3 个事件后停止监听（返回 false）
        return callback_count < 3;
    };
    
    printf("   🔔 订阅网络质量事件回调 (最多3个事件)...\n");
    // weaknet_subscribe_network_quality 内部会：
    //   1. dbus_bus_add_match("...NetworkQualityChanged")
    //   2. 进入 while(true) { read_write(100ms); pop_message; 解析三个参数; 调用 callback; }
    if (weaknet_subscribe_network_quality(quality_callback)) {
        printf("     ✅ 网络质量事件订阅成功\n");
    } else {
        printf("     ❌ 网络质量事件订阅失败\n");
        return false;
    }
    
    return true;
}

/**
 * @brief 状态变化监控测试（Changed 信号）
 *
 * 每秒钟调用 weaknet_check_changes() 非阻塞检查 Changed 信号（持续 3 秒）。
 * Changed 是旧版通用变化信号，Payload 为 STRING text + INT32 counter。
 */
bool testChangeMonitoring() {
    TEST_CASE("状态变化监控");
    
    char message[512], error[256];
    int32_t counter;
    
    printf("   🔍 检查状态变化 (3秒)...\n");
    for (int i = 0; i < 3; i++) {
        // weaknet_check_changes 内部：dbus_connection_read_write(conn, 0) + pop_message + 检查 Changed 信号
        if (weaknet_check_changes(message, sizeof(message), &counter, error, sizeof(error))) {
            printf("     🎯 检测到变化: %s (counter=%d)\n", message, counter);
        } else {
            printf("     ⏳ 第%d秒: 无变化\n", i+1);
        }
        sleep(1);
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
 * @brief 蓝牙事件监听测试
 *
 * 订阅 BluetoothDeviceChanged 信号后，
 * 每秒钟调用 weaknet_check_events() 非阻塞检查（持续 15 秒）。
 *
 * D-Bus 信号 Payload: STRING text + INT32 counter
 */
bool testBluetoothEvents() {
    TEST_CASE("蓝牙事件监听测试");

    char buffer[512], error[256];

    // 订阅 BluetoothDeviceChanged —— dbus_bus_add_match 注册 D-Bus 匹配规则
    printf("   🔔 订阅蓝牙设备变化事件...\n");
    if (weaknet_subscribe_bluetooth_events(nullptr)) {
        printf("     ✅ 蓝牙事件订阅成功\n");
    } else {
        printf("     ❌ 蓝牙事件订阅失败\n");
        return false;
    }

    // 监听蓝牙事件（持续 15 秒）
    printf("   🔍 监听蓝牙事件 (15秒)...\n");
    char eventType[64], message[512], source[64];
    int32_t counter;

    for (int i = 0; i < 15; i++) {
        // 检查四类信号队列（包括 BluetoothDeviceChanged）
        if (weaknet_check_events(eventType, sizeof(eventType), message, sizeof(message),
                                 &counter, source, sizeof(source), error, sizeof(error))) {
            printf("     🎯 检测到事件: type=%s counter=%d source=%s message=%s\n",
                   eventType, counter, source, message);
        } else {
            printf("     ⏳ 第%d秒: 无蓝牙事件\n", i+1);
        }
        sleep(1);
    }

    return true;
}

/**
 * @brief 蓝牙事件回调测试（阻塞模式，持续 30 秒）
 *
 * 注意：subscribe_bluetooth_events 当前只添加 D-Bus match，
 * 不进入阻塞监听循环。本测试实际上是一个 30 秒的被动等待。
 */
bool testBluetoothCallback() {
    TEST_CASE("蓝牙事件回调测试");

    static int bt_callback_count = 0;
    auto bt_callback = [](const char* event_type, const char* message, int32_t counter, const char* source) {
        bt_callback_count++;
        printf("     🎯 回调收到蓝牙事件 #%d:\n", bt_callback_count);
        printf("       事件类型: %s\n", event_type);
        printf("       消息内容: %s\n", message);
        printf("       事件计数: %d\n", counter);
        printf("       事件来源: %s\n", source);
    };

    printf("   🔔 订阅蓝牙事件回调（按Ctrl+C停止）...\n");
    if (weaknet_subscribe_bluetooth_events(bt_callback)) {
        printf("     ✅ 蓝牙事件回调订阅成功\n");
    } else {
        printf("     ❌ 蓝牙事件回调订阅失败\n");
        return false;
    }

    // 保持运行 30 秒（被动等待）
    printf("   🔍 等待蓝牙事件 (30秒)...\n");
    for (int i = 0; i < 30; i++) {
        sleep(1);
    }

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
    testEventSystem();
    testBluetoothDevices();
    testBluetoothAdapter();
    testBluetoothEvents();
    testBluetoothCallback();
    testNetworkQualityEvents();
    testNetworkQualityCallback();
    testChangeMonitoring();
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
 * 所有命令在执行前都会先 weaknet_init()，执行完后 weaknet_cleanup()（subscribe/quality-sub/bt-events 等持续监听命令除外）。
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
    // ===== file =====
    // 离线模式，不发起 D-Bus 调用，直接读取服务端序列化文件
    else if (command == "file") {
        if (weaknet_get_from_file(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 文件中的状态: %s\n", buffer);
        } else {
            printf("❌ 读取文件失败: %s\n", error);
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
    // ===== check =====
    // 单次非阻塞检查 Changed 信号（旧版通用变化信号）
    else if (command == "check") {
        if (weaknet_check_changes(buffer, sizeof(buffer), &counter, error, sizeof(error))) {
            printf("✅ 检测到变化: %s (counter=%d)\n", buffer, counter);
        } else {
            printf("ℹ️  没有检测到新变化: %s\n", error);
        }
    }
    // ===== events =====
    // 单次非阻塞检查多类事件信号（InterfaceChanged / ConnectionModeChanged / NetworkQualityChanged / BluetoothDeviceChanged）
    else if (command == "events") {
        char eventType[64], message[512], source[64];
        if (weaknet_check_events(eventType, sizeof(eventType), message, sizeof(message),
                                  &counter, source, sizeof(source), error, sizeof(error))) {
            printf("✅ 检测到事件: type=%s counter=%d source=%s message=%s\n", 
                        eventType, counter, source, message);
        } else {
            printf("ℹ️  没有检测到事件: %s\n", error);
        }
    }
    // ===== quality =====
    // 单次非阻塞检查 NetworkQualityChanged 信号
    // Payload: STRING quality + STRING details + INT32 counter
    else if (command == "quality") {
        char quality[256], details[1024];
        if (weaknet_check_network_quality(quality, sizeof(quality), details, sizeof(details),
                                             &counter, error, sizeof(error))) {
            printf("✅ 检测到网络质量事件:\n");
            printf("   质量等级: %s\n", quality);
            printf("   详细信息: %s\n", details);
            printf("   事件计数: %d\n", counter);
        } else {
            printf("ℹ️  没有检测到网络质量事件: %s\n", error);
        }
    }
    // ===== quality-sub =====
    // 持续监听 NetworkQualityChanged 信号（最多 60 秒，每秒轮询一次）
    else if (command == "quality-sub") {
        printf("🔄 开始监听网络质量事件（按Ctrl+C停止）...\n");
        char quality[256], details[1024];
        int count = 0;
        while (count < 60) { // 最多运行 1 分钟
            if (weaknet_check_network_quality(quality, sizeof(quality), details, sizeof(details),
                                                     &counter, error, sizeof(error))) {
                printf("📊 网络质量事件: %s (分数: %s, 计数: %d)\n", quality, details, counter);
            }
            usleep(1000000); // 等待 1 秒
            count++;
        }
    }
    // ===== event-types =====
    // 本地拼接 "InterfaceChanged,ConnectionModeChanged,NetworkQualityChanged,BluetoothDeviceChanged"
    else if (command == "event-types") {
        if (weaknet_get_event_types(buffer, sizeof(buffer), error, sizeof(error))) {
            printf("✅ 支持的事件类型: %s\n", buffer);
        } else {
            printf("❌ 获取事件类型失败: %s\n", error);
            return false;
        }
    }
    // ===== event-sub EVENT_TYPE =====
    // 通过 dbus_bus_add_match 订阅指定事件类型的 D-Bus 信号
    // 需要 argc >= 3，argv[2] 为事件类型名称
    else if (command == "event-sub" && argc >= 3) {
        std::string eventType = argv[2];
        if (weaknet_subscribe_event(eventType.c_str(), nullptr)) {
            printf("✅ 成功订阅事件类型: %s\n", eventType.c_str());
        } else {
            printf("❌ 订阅事件失败: %s\n", eventType.c_str());
            return false;
        }
    }
    // ===== subscribe =====
    // 持续监听 Changed 信号（最多 5 分钟，每秒轮询一次）
    else if (command == "subscribe") {
        printf("🔄 开始持续监听网络变化（按Ctrl+C停止）...\n");
        int count = 0;
        while (count < 300) { // 最多运行 5 分钟
            if (weaknet_check_changes(buffer, sizeof(buffer), &counter, error, sizeof(error))) {
                printf("📢 收到网络变化: %s (counter=%d)\n", buffer, counter);
            }
            usleep(1000000); // 等待 1 秒
            count++;
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
    // ===== bt-events =====
    // 订阅 BluetoothDeviceChanged 信号后持续监听（最多 60 秒）
    else if (command == "bt-events") {
        printf("🔄 开始监听蓝牙设备变化（按Ctrl+C停止）...\n");
        // 订阅 BluetoothDeviceChanged —— 添加 D-Bus match 规则
        if (!weaknet_subscribe_bluetooth_events(nullptr)) {
            printf("❌ 订阅蓝牙事件失败\n");
            return false;
        }
        printf("✅ 蓝牙事件订阅成功\n");
        int count = 0;
        while (count < 60) {
            char eventType[64], message[512], source[64];
            // 每秒检查一次四类信号队列（包括 BluetoothDeviceChanged）
            if (weaknet_check_events(eventType, sizeof(eventType), message, sizeof(message),
                                     &counter, source, sizeof(source), error, sizeof(error))) {
                printf("📢 蓝牙设备变化: type=%s counter=%d source=%s message=%s\n",
                       eventType, counter, source, message);
            }
            usleep(1000000);
            count++;
        }
    }
    // ===== test-bt =====
    // 依次执行蓝牙相关测试（设备列表、适配器、事件监听）
    else if (command == "test-bt") {
        testBluetoothDevices();
        testBluetoothAdapter();
        testBluetoothEvents();
        return true;
    }
    // ===== test-bt-callback =====
    // 蓝牙事件回调测试（阻塞/被动等待 30 秒）
    else if (command == "test-bt-callback") {
        return testBluetoothCallback();
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
    // ===== test-events =====
    // 事件系统测试（subscribe → check → unsubscribe）
    else if (command == "test-events") {
        return testEventSystem();
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
    // ===== test-quality =====
    // 网络质量事件非阻塞监听测试（持续 10 秒）
    else if (command == "test-quality") {
        return testNetworkQualityEvents();
    }
    // ===== test-quality-callback =====
    // 网络质量事件阻塞回调测试（收到 3 个事件后退出）
    else if (command == "test-quality-callback") {
        return testNetworkQualityCallback();
    }
    // ===== 未知命令 =====
    else {
        printf("❌ 未知命令: %s\n", command.c_str());
        return false;
    }
    
    // 正常退出前清理资源（subscribe/quality-sub/bt-events 等持续监听命令会先返回）
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
 *   argv[2] —— 子命令参数（可选，如 ping 的主机名、event-sub 的事件类型）
 *
 * 子命令列表：
 *   all                      - 运行完整测试套件（默认行为）
 *   get                      - 获取当前网络接口列表（GetInterfaces）
 *   health                   - 网络健康检查（HealthCheck）
 *   ebpf-health              - eBPF 监控器健康快照（GetEbpfMonitorHealth）
 *   file                     - 从序列化文件读取最新状态（离线模式）
 *   ping HOSTNAME            - Ping 指定主机（Ping 方法，需要第二个参数）
 *   check                    - 单次非阻塞检查 Changed 信号
 *   subscribe                - 持续监听 Changed 信号（最多 5 分钟）
 *   events                   - 单次非阻塞检查多类事件信号
 *   quality                  - 单次非阻塞检查 NetworkQualityChanged 信号
 *   quality-sub              - 持续监听网络质量事件（最多 60 秒）
 *   event-types              - 获取支持的事件类型字符串列表
 *   event-sub EVENT_TYPE     - 订阅指定事件类型（只添加 D-Bus match，不阻塞）
 *   bt-devices               - 获取蓝牙设备列表（GetBluetoothDevices）
 *   bt-adapter               - 获取蓝牙适配器状态（GetBluetoothAdapter）
 *   bt-events                - 持续监听蓝牙设备变化（最多 60 秒）
 *   test-basic               - 基础功能测试
 *   test-network             - 网络接口信息测试
 *   test-ping                - Ping 功能测试
 *   test-events              - 事件系统测试
 *   test-quality             - 网络质量事件非阻塞监听测试
 *   test-quality-callback    - 网络质量事件阻塞回调测试
 *   test-bt                  - 蓝牙功能组合测试
 *   test-bt-callback         - 蓝牙事件回调测试
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
        printf("  %s file                   - 从文件读取最新状态\n", argv[0]);
        printf("  %s ping HOSTNAME          - Ping指定主机\n", argv[0]);
        printf("  %s check                  - 单次检查变化\n", argv[0]);
        printf("  %s subscribe              - 持续监听网络变化\n", argv[0]);
        printf("  %s events                 - 单次检查事件\n", argv[0]);
        printf("  %s quality                - 单次检查网络质量事件\n", argv[0]);
        printf("  %s quality-sub            - 持续监听网络质量事件\n", argv[0]);
        printf("  %s event-types            - 获取支持的事件类型\n", argv[0]);
        printf("  %s event-sub EVENT_TYPE   - 订阅特定事件类型\n", argv[0]);
        printf("\n蓝牙功能:\n");
        printf("  %s bt-devices             - 获取蓝牙设备列表\n", argv[0]);
        printf("  %s bt-adapter             - 获取蓝牙适配器信息\n", argv[0]);
        printf("  %s bt-events              - 持续监听蓝牙设备变化\n", argv[0]);
        printf("\n测试模式:\n");
        printf("  %s test-basic             - 基础功能测试\n", argv[0]);
        printf("  %s test-network           - 网络信息测试\n", argv[0]);
        printf("  %s test-ping              - Ping功能测试\n", argv[0]);
        printf("  %s test-events            - 事件系统测试\n", argv[0]);
        printf("  %s test-quality           - 网络质量事件测试\n", argv[0]);
        printf("  %s test-quality-callback  - 网络质量事件回调测试\n", argv[0]);
        printf("  %s test-bt                - 蓝牙功能测试\n", argv[0]);
        printf("  %s test-bt-callback       - 蓝牙事件回调测试\n", argv[0]);
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
