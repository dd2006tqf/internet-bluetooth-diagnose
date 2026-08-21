// logger.hpp
// WeakNet 统一日志系统 - 基于 Google glog

#pragma once

#include <atomic>
#include <glog/logging.h>
#include <cstdio>
#include <string>
#include <memory>
#include <fstream>
#include <mutex>
#include <ctime>
#include <csignal>

namespace weaknet_dbus {

// 日志级别枚举
enum class LogLevel {
    INFO = 0,
    WARNING = 1,
    ERROR = 2,
    FATAL = 3
};

// 日志模块标识
namespace LogModule {
    constexpr const char* SERVER = "server";
    constexpr const char* CLIENT = "client";
    constexpr const char* DBUS = "dbus";
    constexpr const char* WEAK_MGR = "weak_mgr";
    constexpr const char* TCP_LOSS = "tcp_loss";
    constexpr const char* RTT = "rtt";
    constexpr const char* RSSI = "rssi";
    constexpr const char* NETWORK = "network";
    constexpr const char* EVENT_MGR = "event_mgr";
    constexpr const char* PING = "ping";
    constexpr const char* INTERFACE = "interface";
    constexpr const char* BLUETOOTH = "bluetooth";
    constexpr const char* SYSTEM = "system";
}

// 日志初始化类
class Logger {
public:
    // 初始化日志系统
    static bool init(const std::string& program_name,
                    const std::string& log_dir = "./logs/server",
                    LogLevel min_level = LogLevel::INFO,
                    bool log_to_stderr = true);

    // 清理日志系统
    static void shutdown();

    // 设置日志级别
    static void setLogLevel(LogLevel level);

    // 设置日志目录
    static void setLogDir(const std::string& dir);

    // 检查是否已初始化
    static bool isInitialized() { return initialized_; }

    // 清理超过 max_age_days 天的日志文件；目录不存在时安全返回。返回删除文件数。
    static int cleanOldLogs(const std::string& log_dir, int max_age_days);

    // ---- 带时间戳的文件日志 ----

    // 启动文件日志：创建 logDir 目录，打开 server/log/server_YYYYMMDD_HHMMSS.log
    static void startFileLog(const std::string& logDir = "./server/log");

    // 停止文件日志：刷新缓冲区，关闭文件流
    static void stopFileLog();

    // 获取当前时间戳字符串（YYYYMMDD_HHMMSS）
    static std::string getCurrentTimestamp();

    // 信号处理函数（SIGINT/SIGTERM）
    static void signalHandler(int signum);

    // 写入一行到文件日志
    static void writeToFileLog(const std::string& line);

    // 检查文件日志是否激活
    static bool isFileLogActive() { return file_log_active_; }

    // async-signal-safe 的停止标志（signal handler 中唯一可安全设置的变量）
    // C++11+ std::atomic<bool> 对 signal handler 是 safe 的（lock-free 时）
    static bool stopRequested() { return stop_requested_.load(std::memory_order_relaxed); }
    static void requestStop() { stop_requested_.store(true, std::memory_order_relaxed); }

private:
    static bool initialized_;
    static std::string current_log_dir_;
    static std::atomic<bool> stop_requested_;

    // 文件日志相关
    static std::ofstream file_stream_;
    static std::string file_log_path_;
    static std::mutex file_mutex_;
    static bool file_log_active_;
};

// 便捷的日志宏定义
#define LOG_INFO(module, msg) LOG(INFO) << "[" << module << "] " << msg
#define LOG_WARNING(module, msg) LOG(WARNING) << "[" << module << "] " << msg
#define LOG_ERROR(module, msg) LOG(ERROR) << "[" << module << "] " << msg
#define LOG_FATAL(module, msg) LOG(FATAL) << "[" << module << "] " << msg

// 带格式的日志宏（printf 风格：通过 snprintf 格式化后送入 glog）
#define LOG_INFO_F(module, fmt, ...)                                             \
    do {                                                                         \
        char _wklog_buf[2048];                                                   \
        std::snprintf(_wklog_buf, sizeof(_wklog_buf), (fmt), ##__VA_ARGS__);     \
        LOG(INFO) << "[" << (module) << "] " << _wklog_buf;                      \
    } while (0)

#define LOG_ERROR_F(module, fmt, ...)                                            \
    do {                                                                         \
        char _wklog_buf[2048];                                                   \
        std::snprintf(_wklog_buf, sizeof(_wklog_buf), (fmt), ##__VA_ARGS__);     \
        LOG(ERROR) << "[" << (module) << "] " << _wklog_buf;                     \
    } while (0)

// 条件日志宏
#define LOG_IF_INFO(condition, module, msg) \
    LOG_IF(INFO, condition) << "[" << module << "] " << msg

#define LOG_IF_ERROR(condition, module, msg) \
    LOG_IF(ERROR, condition) << "[" << module << "] " << msg

// 调试日志宏 (仅在DEBUG模式下编译)
#ifdef DEBUG
#define LOG_DEBUG(module, msg) LOG(INFO) << "[DEBUG][" << module << "] " << msg
#else
#define LOG_DEBUG(module, msg) do {} while(0)
#endif

// 性能日志宏
#define LOG_PERF(module, operation, duration_ms) \
    LOG(INFO) << "[" << module << "] PERF: " << operation << " took " << duration_ms << "ms"

// 网络状态日志宏
#define LOG_NETWORK_STATE(module, interface, state) \
    LOG(INFO) << "[" << module << "] Network interface " << interface << " state: " << state

// 事件日志宏
#define LOG_EVENT(module, event_type, message) \
    LOG(INFO) << "[" << module << "] EVENT: " << event_type << " - " << message

// 错误处理日志宏
#define LOG_ERROR_WITH_CODE(module, operation, error_code) \
    LOG(ERROR) << "[" << module << "] " << operation << " failed with error code: " << error_code

} // namespace weaknet_dbus


