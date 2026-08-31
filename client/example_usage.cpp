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
 *   6. weaknet_subscribe_event() 订阅 InterfaceChanged / ConnectionModeChanged 信号
 *   7. weaknet_check_events()    非阻塞轮询 D-Bus 信号队列
 *   8. weaknet_unsubscribe_event() 取消订阅
 *   9. weaknet_cleanup()         释放资源
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

    // ===== 步骤 6：获取支持的事件类型 =====
    // 本地拼接字符串 "InterfaceChanged,ConnectionModeChanged,NetworkQualityChanged,BluetoothDeviceChanged"
    std::cout << "\n5. 获取支持的事件类型..." << std::endl;
    char event_types[256];
    
    if (weaknet_get_event_types(event_types, sizeof(event_types), error_buffer, sizeof(error_buffer))) {
        std::cout << "✅ 支持的事件类型: " << event_types << std::endl;
    } else {
        std::cout << "❌ 获取事件类型失败: " << error_buffer << std::endl;
    }

    // ===== 步骤 7：订阅网络变化事件 =====
    // 通过 dbus_bus_add_match 注册 D-Bus 信号匹配规则
    // 匹配规则：type='signal', interface='com.example.WeakNet', member=<事件名>
    std::cout << "\n6. 订阅网络变化事件..." << std::endl;
    
    if (weaknet_subscribe_event("InterfaceChanged", onNetworkChange)) {
        std::cout << "✅ 已订阅InterfaceChanged事件" << std::endl;
    } else {
        std::cout << "❌ 订阅InterfaceChanged事件失败" << std::endl;
    }

    if (weaknet_subscribe_event("ConnectionModeChanged", onNetworkChange)) {
        std::cout << "✅ 已订阅ConnectionModeChanged事件" << std::endl;
    } else {
        std::cout << "❌ 订阅ConnectionModeChanged事件失败" << std::endl;
    }

    // ===== 步骤 8：开始事件监控（持续 30 秒） =====
    // 每 500ms 调用一次 weaknet_check_events() 非阻塞轮询 D-Bus 队列
    // weaknet_check_events 检查四种信号：InterfaceChanged / ConnectionModeChanged / NetworkQualityChanged / BluetoothDeviceChanged
    std::cout << "\n7. 开始事件监控（30秒）..." << std::endl;
    std::cout << "   注意: 在新终端中运行服务器或改变网络状态来触发事件" << std::endl;
    
    char event_type_buffer[64];
    char message_buffer[512];
    char source_buffer[64];
    int32_t counter;
    
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::seconds(30);
    
    int event_count = 0;
    
    while (std::chrono::steady_clock::now() < end_time) {
        auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(end_time - now).count();
        
        // 非阻塞检查事件 —— 内部调用 dbus_connection_read_write(conn, 0) + pop_message
        if (weaknet_check_events(event_type_buffer, sizeof(event_type_buffer),
                                  message_buffer, sizeof(message_buffer),
                                  &counter, source_buffer, sizeof(source_buffer),
                                  error_buffer, sizeof(error_buffer))) {
            event_count++;
            std::cout << "\n🎯 EVENT #" << event_count << ":" << std::endl;
            std::cout << "   Type: " << event_type_buffer << std::endl;
            std::cout << "   Counter: " << counter << std::endl;
            std::cout << "   Source: " << source_buffer << std::endl;
            std::cout << "   Message: " << message_buffer << std::endl;
        }
        
        std::cout << "\r⏳ 剩余时间: " << remaining << "秒 (检测到 " << event_count << " 个事件)";
        std::cout.flush();
        
        // 短暂休眠避免 CPU 占用过高
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    std::cout << std::endl;
    
    // ===== 步骤 9：取消事件订阅 =====
    // 当前实现为简化版本，固定返回 true（不实际移除 D-Bus match 规则）
    std::cout << "\n8. 取消事件订阅..." << std::endl;
    
    if (weaknet_unsubscribe_event("InterfaceChanged")) {
        std::cout << "✅ 已取消InterfaceChanged事件订阅" << std::endl;
    }
    
    if (weaknet_unsubscribe_event("ConnectionModeChanged")) {
        std::cout << "✅ 已取消ConnectionModeChanged事件订阅" << std::endl;
    }

    // ===== 步骤 10：清理资源 =====
    // 断开 D-Bus 连接、释放全局单例、关闭日志系统
    std::cout << "\n9. 清理资源..." << std::endl;
    weaknet_cleanup();
    std::cout << "✅ WeakNet客户端库已清理" << std::endl;

    // ===== 总结输出 =====
    std::cout << "\n=== 使用示例完成 ===" << std::endl;
    std::cout << "📊 本次运行检测到 " << event_count << " 个事件" << std::endl;
    std::cout << "💡 提示: 要看到实际事件，请:" << std::endl;
    std::cout << "    - 确保WeakNet服务器正在运行" << std::endl;
    std::cout << "    - 插入/拔出网络设备" << std::endl;
    std::cout << "    - 切换网络连接" << std::endl;
    std::cout << "    - 改变网络配置" << std::endl;

    return 0;
}
