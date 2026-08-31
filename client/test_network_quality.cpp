/**
 * @file test_network_quality.cpp
 * @brief WeakNet 网络质量事件监听测试程序
 *
 * 本程序持续监听 WeakNet 服务端发出的 NetworkQualityChanged 信号。
 * 信号包含三个参数：质量等级（Quality）、详细指标（Details, JSON）、计数器。
 *
 * 两种监听方式都做了演示：
 *   1. 阻塞回调模式 —— weaknet_subscribe_network_quality(callback)
 *      内部进入 while(true) 消息循环，每次收到信号调用回调
 *      （当前注释掉，因为回调会阻塞主线程）
 *   2. 非阻塞轮询模式 —— weaknet_check_network_quality(...)
 *      主循环每 100ms 检查一次 D-Bus 消息队列
 *
 * D-Bus 信号详情：
 *   - Signal:  NetworkQualityChanged
 *   - Payload: STRING quality ("Poor"/"Fair"/"Good"/"Excellent")
 *              + STRING details (JSON 格式指标详情)
 *              + INT32 counter
 *   - 注册方式: dbus_bus_add_match("type='signal',interface='com.example.WeakNet',member='NetworkQualityChanged'")
 *
 * 运行方式：
 *   LD_LIBRARY_PATH=. ./test_network_quality
 *   按 Ctrl+C 退出
 */

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include "weaknet_client.h"

// ===== 全局变量：用于信号处理优雅退出 =====
static bool running = true;

/**
 * @brief 信号处理函数 —— 捕获 Ctrl+C 或 kill 信号，优雅退出
 *
 * 收到 SIGINT 或 SIGTERM 时将 running 置为 false，
 * 主循环在下一轮迭代后退出。
 *
 * @param sig 收到的信号编号
 */
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cout << "\n收到退出信号，正在停止..." << std::endl;
        running = false;
    }
}

/**
 * @brief 网络质量事件回调函数（阻塞模式使用）
 *
 * 用于 weaknet_subscribe_network_quality()。
 * 返回值决定是否继续监听：返回 true 继续、返回 false 立即退出监听循环。
 *
 * @param quality  质量等级（"Poor"/"Fair"/"Good"/"Excellent"）
 * @param details  详细质量指标（JSON 格式）
 * @param counter  服务端事件计数器（单调递增）
 * @return true=继续监听；返回 running 以便收到退出信号后停止
 */
bool network_quality_callback(const char* quality, const char* details, int32_t counter) {
    std::cout << "\n=== 网络质量事件 ===" << std::endl;
    std::cout << "质量等级: " << quality << std::endl;
    std::cout << "事件计数: " << counter << std::endl;
    std::cout << "详细信息: " << details << std::endl;
    std::cout << "===================" << std::endl;
    
    // 返回 true 继续监听，返回 false 停止监听
    return running;
}

/**
 * @brief 主函数
 *
 * 执行步骤：
 *   1. 安装 SIGINT/SIGTERM 信号处理器
 *   2. 初始化 WeakNet 客户端（建立 D-Bus Session 连接）
 *   3. 等待连接建立（最多重试 10 次，每次 1 秒）
 *   4. 注册 NetworkQualityChanged 信号的 D-Bus match 规则
 *   5. 主循环中以 100ms 间隔非阻塞检查 D-Bus 消息队列
 *   6. 收到退出信号后调用 weaknet_cleanup() 释放资源
 */
int main() {
    std::cout << "WeakNet 网络质量事件监听测试程序" << std::endl;
    std::cout << "按 Ctrl+C 退出程序" << std::endl;
    
    // ===== 步骤 1：安装信号处理器 =====
    // 让 Ctrl+C 能够优雅地跳出主循环
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // ===== 步骤 2：初始化 WeakNet 客户端 =====
    // 内部调用 dbus_bus_get(DBUS_BUS_SYSTEM, ...) 建立 D-Bus 连接
    if (!weaknet_init()) {
        std::cerr << "错误: 无法初始化WeakNet客户端" << std::endl;
        return 1;
    }
    
    std::cout << "客户端初始化成功" << std::endl;
    
    // ===== 步骤 3：等待 D-Bus 连接就绪 =====
    // D-Bus 总线启动可能有短暂延迟，这里最多等 10 秒
    int retry_count = 0;
    while (!weaknet_is_connected() && retry_count < 10) {
        std::cout << "等待连接到服务器..." << std::endl;
        sleep(1);
        retry_count++;
    }
    
    if (!weaknet_is_connected()) {
        std::cerr << "错误: 无法连接到WeakNet服务器" << std::endl;
        weaknet_cleanup();
        return 1;
    }
    
    std::cout << "已连接到服务器，开始监听网络质量事件..." << std::endl;
    
    // ===== 步骤 4：订阅 NetworkQualityChanged 信号 =====
    // 内部通过 dbus_bus_add_match 注册匹配规则：
    //   type='signal', interface='com.example.WeakNet', member='NetworkQualityChanged'
    // 注意：当前只添加 D-Bus 订阅，不进入阻塞监听循环（避免阻塞主线程）
    if (!weaknet_subscribe_network_quality(network_quality_callback)) {
        std::cerr << "错误: 无法订阅网络质量事件" << std::endl;
        weaknet_cleanup();
        return 1;
    }
    
    std::cout << "网络质量事件订阅成功，开始监听..." << std::endl;
    
    // ===== 步骤 5：主循环 —— 非阻塞检查 D-Bus 消息队列 =====
    // weaknet_check_network_quality 内部：
    //   1. dbus_connection_read_write(conn, 0)   // 非阻塞读写，超时 0ms
    //   2. dbus_connection_pop_message(conn)    // 弹出一条待处理消息
    //   3. dbus_message_is_signal(msg, ..., "NetworkQualityChanged")  // 检查信号类型
    //   4. dbus_message_get_args(msg, ..., DBUS_TYPE_STRING, &quality, DBUS_TYPE_STRING, &details, DBUS_TYPE_INT32, &counter, ...)  // 解析参数
    while (running) {
        char quality[256] = {0};   // 质量等级输出缓冲区
        char details[1024] = {0};  // 详细指标输出缓冲区（JSON）
        char error[256] = {0};     // 错误信息缓冲区
        int32_t counter = 0;       // 事件计数器
        
        if (weaknet_check_network_quality(quality, sizeof(quality), 
                                        details, sizeof(details), 
                                           &counter, error, sizeof(error))) {
            std::cout << "\n=== 检测到网络质量事件 ===" << std::endl;
            std::cout << "质量等级: " << quality << std::endl;
            std::cout << "事件计数: " << counter << std::endl;
            std::cout << "详细信息: " << details << std::endl;
            std::cout << "=========================" << std::endl;
        }
        
        // 短暂休眠避免 CPU 占用过高（100ms）
        usleep(100000);
    }
    
    // ===== 步骤 6：清理资源 =====
    // 断开 D-Bus 连接、释放单例客户端、关闭日志
    std::cout << "正在清理资源..." << std::endl;
    weaknet_cleanup();
    
    std::cout << "程序已退出" << std::endl;
    return 0;
}
