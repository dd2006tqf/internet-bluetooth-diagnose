/**
 * @file example_usage.cpp
 * @brief WeakNet 动态库使用示例程序 —— 演示完整集成流程
 *
 * 本程序演示如何在任意 C++ 应用中集成 WeakNet 客户端动态库
 * （libweaknet_client.so），展示以下典型操作：
 *
 *   1. weaknet_init()            初始化库并连接 D-Bus
 *   2. weaknet_is_connected()    检查连接状态
 *   3. weaknet_get_version()     获取库版本
 *   4. weaknet_get_interfaces()  获取当前网络接口（D-Bus Method: GetInterfaces）
 *   5. weaknet_health_check()    网络健康检查（D-Bus Method: HealthCheck）
 *   6. 按需的 get_* 查询       （这是当前唯一有效的数据路径）
 *   7. weaknet_cleanup()         释放资源
 *
 * ⚠️ 本示例**不再演示事件订阅**（2026-11-01）：
 *   服务端自 2026-10-19 起停发了全部 D-Bus 信号——停发时它们没有任何生产
 *   订阅者（决策与证据见 openspec/tasks/2026-10-19-stop-unsubscribed-dbus-signals.md）。
 *   于是 weaknet_subscribe_event() / weaknet_check_events() 这类 API 成了空转：
 *   订阅会成功（本地加了 match 规则），但**永远等不到事件**。
 *   示例程序若继续演示它，等于把"抄这个就能用"的错误信号传给集成方——
 *   而这正是当初那条"信号替代轮询"的消费路径从未建出来的原因之一。
 *   要拿当前状态请直接调用对应的 get_* / *_stats 查询方法。
 *
 * 依赖：
 *   - WeakNet 服务端必须已启动并在 D-Bus Session 总线上注册
 *   - weaknet_client.h + libweaknet_client.so
 *
 * 运行方式：
 *   LD_LIBRARY_PATH=. ./example_usage
 */

#include <iostream>
#include <thread>
#include <chrono>
#include "weaknet_client.h"

/**
 * @brief 事件回调函数 —— 打印收到的事件信息
 *
 * 用于 weaknet_subscribe_event() 的 C 回调签名。
 * 注意：当前版本中，订阅函数只添加 D-Bus match 规则，
 * 回调通过 weaknet_check_events() 轮询在外部触发。
 *
 * @param event_type 事件类型字符串（如 "InterfaceChanged"）
 * @param message    事件描述文本
 * @param counter    服务端单调递增的事件计数器
 * @param source     事件来源标识
 */
void onNetworkChange(const char* event_type, const char* message, int32_t counter, const char* source) {
    std::cout << "🔄 收到事件: type=" << event_type 
              << " counter=" << counter 
              << " source=" << source 
              << " message=" << message << std::endl;
}

/**
 * @brief 主函数 —— 按步骤演示 WeakNet 动态库的完整使用流程
 *
 * 无命令行参数。运行后会持续监控 30 秒的事件，
 * 期间可以在另一个终端插拔网络设备来触发 D-Bus 信号。
 */
int main() {
    std::cout << "=== WeakNet 动态库使用示例 ===" << std::endl;

    // ===== 步骤 1：初始化 WeakNet 客户端库 =====
    // 内部会调用 dbus_bus_get(DBUS_BUS_SYSTEM) 连接 D-Bus
    std::cout << "\n1. 初始化WeakNet客户端库..." << std::endl;
    if (!weaknet_init()) {
        std::cerr << "❌ 初始化失败!" << std::endl;
        return 1;
    }
    std::cout << "✅ 初始化成功!" << std::endl;

    // ===== 步骤 2：检查连接状态 =====
    // 判断 D-Bus Session 连接是否就绪
    std::cout << "\n2. 检查连接状态..." << std::endl;
    if (weaknet_is_connected()) {
        std::cout << "✅ 已连接到WeakNet服务" << std::endl;
    } else {
        std::cout << "❌ 未连接到WeakNet服务" << std::endl;
    }

    // ===== 步骤 3：获取库版本和编译信息 =====
    // 这两个函数是本地常量/宏，不发起 D-Bus 调用
    char version_info[256];
    if (weaknet_get_version(version_info, sizeof(version_info))) {
        std::cout << "📦 库版本: " << version_info << std::endl;
    }

    char build_info[256];
    if (weaknet_get_build_info(build_info, sizeof(build_info))) {
        std::cout << "🔧 编译信息: " << build_info << std::endl;
    }

    // ===== 步骤 4：获取网络接口信息 =====
    // D-Bus 调用：com.example.WeakNet /com/example/WeakNet GetInterfaces
    // 返回值：ARRAY of STRING → 逗号拼接的字符串
    std::cout << "\n3. 获取网络接口信息..." << std::endl;
    char interface_info[4096];
    char error_buffer[256];
    
    if (weaknet_get_interfaces(interface_info, sizeof(interface_info), error_buffer, sizeof(error_buffer))) {
        std::cout << "✅ 网络接口信息: " << interface_info << std::endl;
    } else {
        std::cout << "❌ 获取失败: " << error_buffer << std::endl;
    }

    // ===== 步骤 5：网络健康检查 =====
    // D-Bus 调用：HealthCheck → 返回 JSON 格式诊断报告
    std::cout << "\n4. 执行网络健康检查..." << std::endl;
    char health_result[4096];
    
    if (weaknet_health_check(health_result, sizeof(health_result), error_buffer, sizeof(error_buffer))) {
        std::cout << "✅ 健康检查结果: " << health_result << std::endl;
    } else {
        std::cout << "❌ 健康检查失败: " << error_buffer << std::endl;
    }

    // ===== 步骤 6：查询当前状态（事件订阅的替代路径） =====
    //
    // 这里刻意演示**查询**而不是订阅：服务端已停发全部 D-Bus 信号
    // （2026-10-19），weaknet_subscribe_event() / weaknet_check_events()
    // 现在订得上但永远等不到事件。生产集成应当按需轮询 get_* 方法。
    std::cout << "\n6. 查询当前网络状态..." << std::endl;

    char query_buffer[2048];

    // 6.1 TCP 重传统计（真实数据源：eBPF tcp_retrans 探针）
    if (weaknet_get_tcp_retrans_stats(query_buffer, sizeof(query_buffer),
                                   error_buffer, sizeof(error_buffer))) {
        std::cout << "✅ TCP 统计: " << query_buffer << std::endl;
    } else {
        std::cout << "⚠️  TCP 统计暂不可用: " << error_buffer << std::endl;
    }

    // 6.2 TCP RTT 与抖动（真实数据源：tcp_connect / tcp_conn 探针）
    if (weaknet_get_tcp_rtt_stats(query_buffer, sizeof(query_buffer),
                              error_buffer, sizeof(error_buffer))) {
        std::cout << "✅ RTT 统计: " << query_buffer << std::endl;
    } else {
        std::cout << "⚠️  RTT 统计暂不可用: " << error_buffer << std::endl;
    }

    // 6.3 网络体验评分（Assurance v2 / SLE）
    if (weaknet_get_network_experience(query_buffer, sizeof(query_buffer),
                                    error_buffer, sizeof(error_buffer))) {
        std::cout << "✅ 网络体验: " << query_buffer << std::endl;
    } else {
        std::cout << "⚠️  网络体验暂不可用: " << error_buffer << std::endl;
    }

    // 若需要"持续监测"，正确做法是按固定周期重复上面的查询，
    // 而不是订阅一个永远不来的信号。
    std::cout << "\n   提示：持续监测请按周期轮询上述查询方法；"
                 "事件订阅族当前无效（见 weaknet_client.h 文件头）。" << std::endl;

    // ===== 步骤 7：清理资源 =====
    // 断开 D-Bus 连接、释放全局单例、关闭日志系统
    std::cout << "\n7. 清理资源..." << std::endl;
    weaknet_cleanup();
    std::cout << "✅ WeakNet客户端库已清理" << std::endl;

    // ===== 总结输出 =====
    std::cout << "\n=== 使用示例完成 ===" << std::endl;
    std::cout << "💡 集成要点：" << std::endl;
    std::cout << "    - 数据一律通过 get_* / *_stats 查询方法取得" << std::endl;
    std::cout << "    - 事件订阅族当前无效（服务端自 2026-10-19 停发全部信号）" << std::endl;
    std::cout << "    - 需要持续监测就按固定周期重复查询，不要依赖订阅" << std::endl;

    return 0;
}
