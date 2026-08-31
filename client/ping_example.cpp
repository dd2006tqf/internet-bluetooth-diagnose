/**
 * @file ping_example.cpp
 * @brief WeakNet Ping 功能使用示例程序
 *
 * 本程序演示如何通过 WeakNet 客户端库发起 Ping 请求。
 * 与直接调用系统 `ping` 命令不同，这里的 Ping 请求
 * 是通过 D-Bus 转发给 WeakNet 服务端执行的：
 *
 *   客户端 --D-Bus Method Call-->  服务端执行 ICMP Ping  --> 返回统计结果
 *
 *   - D-Bus 方法名:  Ping
 *   - 参数:          STRING hostname (目标主机名或 IP)
 *   - 返回值:        STRING (包含延迟、丢包率的统计文本)
 *   - 超时:          10000ms (10秒)
 *
 * 运行方式：
 *   LD_LIBRARY_PATH=. ./ping_example
 */

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include "weaknet_client.h"

/**
 * @brief 主函数 —— 依次对多个目标主机执行 Ping 测试
 *
 * 无命令行参数。测试目标列表硬编码在 main 函数中，
 * 包含可达主机（8.8.8.8、baidu.com）和一个不可达的无效主机，
 * 用于演示成功和失败两种场景。
 */
int main() {
    std::cout << "=== WeakNet Ping功能使用示例 ===" << std::endl;

    // ===== 初始化 WeakNet 客户端库 =====
    // 内部建立 D-Bus 系统总线连接（dbus_bus_get DBUS_BUS_SYSTEM）
    if (!weaknet_init()) {
        std::cerr << "❌ 初始化WeakNet客户端库失败!" << std::endl;
        return 1;
    }
    std::cout << "✅ WeakNet客户端库已初始化" << std::endl;

    // ===== 测试目标列表 =====
    // 演示对不同类型目标的 Ping：公有 DNS、国内网站、海外网站、无效主机
    std::vector<std::string> targets = {
        "8.8.8.8",           // Google DNS —— 可靠可达
        "baidu.com",         // 百度 —— 国内常用站点
        "github.com",        // GitHub —— 海外站点
        "invalidhost12345.com" // 无效主机名 —— 用于测试错误处理路径
    };

    std::cout << "\n🔍 开始Ping测试..." << std::endl;
    
    for (const auto& target : targets) {
        std::cout << "\n📡 Ping目标: " << target << std::endl;
        
        char result[512];   // Ping 结果缓冲区
        char error[256];     // 错误信息缓冲区
        
        // ===== 发起 Ping 调用 =====
        // D-Bus: com.example.WeakNet /com/example/WeakNet Ping(STRING hostname)
        // 服务端执行 ICMP Ping，返回包含 avg/max 延迟和丢包率的文本
        if (weaknet_ping_host(target.c_str(), result, sizeof(result), error, sizeof(error))) {
            std::cout << "   ✅ 结果: " << result << std::endl;
        } else {
            std::cout << "   ❌ 失败: " << error << std::endl;
        }
        
        // 添加小延迟避免过于频繁的 D-Bus 调用
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\n📊 Ping测试完成!" << std::endl;
    
    // ===== 清理资源 =====
    // 断开 D-Bus 连接并释放单例客户端实例
    weaknet_cleanup();
    std::cout << "🧹 WeakNet客户端库已清理" << std::endl;

    return 0;
}
