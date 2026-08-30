/**
 * @file main.cpp
 * @brief weaknet-dbus 服务可执行程序入口
 *
 * 本文件是整个项目的唯一进程入口点。main() 只做一件事：
 * 调用 weaknet_dbus::start_server() 启动服务。
 *
 * start_server() 内部完成：D-Bus 连接初始化 → 各监控线程启动 →
 * Looper 事件循环 → 优雅退出处理。详见 server.cpp。
 *
 * @return 进程退出码；0 表示正常退出，非 0 表示异常（如 D-Bus 连接失败）
 */

#include "server.hpp"

int main() {
    return weaknet_dbus::start_server();
}
