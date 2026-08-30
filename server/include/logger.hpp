/**
 * @file logger.hpp
 * @brief WeakNet 统一日志系统：基于 Google glog + 文件时间戳 + 便捷宏
 *
 * 封装 glog 的初始化/清理，额外提供：
 *   - 按模块标记的日志宏（LOG_INFO/LOG_ERROR 等带 [module] 前缀）
 *   - printf 风格的格式化日志宏（LOG_INFO_F/LOG_ERROR_F）
 *   - DEBUG 条件编译日志宏（仅 Debug 模式编译）
 *   - 信号处理函数（SIGINT/SIGTERM 时优雅停止并清理日志文件）
 *   - 时间戳命名的文件日志（server_YYYYMMDD_HHMMSS.log）
 *   - 日志文件自动清理（cleanOldLogs）
 *
 * 性能优化点：
 *   glog 异步写盘 + 条件宏（LOG_DEBUG 在 Release 下展开为空语句）
 *   但 FILE_LOG 额外使用 std::ofstream + std::mutex，可能成为热点
 *
 * 安全设计：
 *   stop_requested_ 是 std::atomic<bool>，在 signal handler 中是 safe 的
 *   （lock-free 原子变量在 C++11 后可在 signal handler 中使用）
 */

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

/// 日志级别：映射到 glog 的 INFO/WARNING/ERROR/FATAL
enum class LogLevel {
    INFO = 0,
    WARNING = 1,
    ERROR = 2,
    FATAL = 3
};

/// 日志模块标识常量：统一模块名，便于在 grep 时过滤
namespace LogModule {
    constexpr const char* SERVER       = "server";
    constexpr const char* CLIENT       = "client";
    constexpr const char* DBUS         = "dbus";
    constexpr const char* WEAK_MGR     = "weak_mgr";
    constexpr const char* TCP_LOSS     = "tcp_loss";
    constexpr const char* RTT          = "rtt";
    constexpr const char* RSSI         = "rssi";
    constexpr const char* NETWORK      = "network";
    constexpr const char* EVENT_MGR    = "event_mgr";
    constexpr const char* PING         = "ping";
    constexpr const char* INTERFACE    = "interface";
    constexpr const char* BLUETOOTH    = "bluetooth";
    constexpr const char* SYSTEM       = "system";
}

/**
 * @brief WeakNet 日志管理器（纯静态工具类）
 *
 * 所有方法为 static，无需实例化。
 * 内部维护 glog 状态 + 可选的时间戳文件日志流。
 */
class Logger {
public:
    /**
     * @brief 初始化日志系统
     *
     * 1. glog::InitGoogleLogging(program_name)
     * 2. 设置 glog 输出目录为 log_dir
     * 3. 设置 glog 最小日志级别
     *
     * @param program_name 程序名（用于 glog 日志文件名前缀）
     * @param log_dir      glog 日志目录（默认 ./logs/server）
     * @param min_level    最小输出级别（默认 INFO）
     * @param log_to_stderr 是否同时输出到 stderr（默认 true）
     * @return true 初始化成功
     */
    static bool init(const std::string& program_name,
                    const std::string& log_dir = "./logs/server",
                    LogLevel min_level = LogLevel::INFO,
                    bool log_to_stderr = true);

    /// 清理 glog + 关闭文件日志
    static void shutdown();

    static void setLogLevel(LogLevel level);
    static void setLogDir(const std::string& dir);
    static bool isInitialized() { return initialized_; }

    /**
     * @brief 清理过期日志文件
     * @param log_dir 日志目录
     * @param max_age_days 最大保留天数
     * @return 删除的文件数
     */
    static int cleanOldLogs(const std::string& log_dir, int max_age_days);

    // ---------- 时间戳文件日志 ----------

    /**
     * @brief 启动文件日志：创建时间戳命名的 .log 文件
     *
     * 文件名格式：server_YYYYMMDD_HHMMSS.log
     * 目录不存在时自动创建。
     * 打开后每一条 LOG_INFO 等宏的输出会自动写入该文件（通过拦截 glog sink）。
     */
    static void startFileLog(const std::string& logDir = "./server/log");
    static void stopFileLog();

    /// 获取当前时间戳字符串（YYYYMMDD_HHMMSS），用于日志文件命名
    static std::string getCurrentTimestamp();

    /// SIGINT/SIGTERM 处理：设 stop_requested_ 标志，由主线程轮询检测
    static void signalHandler(int signum);

    static void writeToFileLog(const std::string& line);
    static bool isFileLogActive() { return file_log_active_; }

    // ---------- 优雅停止 ----------

    /**
     * @brief 异步信号安全的停止标志
     *
     * stop_requested_ 是 std::atomic<bool>（lock-free），C++11 后 signal handler 可安全设置。
     * 业务线程在循环中通过 stopRequested() 检测并优雅退出。
     */
    static bool stopRequested() { return stop_requested_.load(std::memory_order_relaxed); }
    static void requestStop() { stop_requested_.store(true, std::memory_order_relaxed); }

private:
    static bool initialized_;
    static std::string current_log_dir_;
    static std::atomic<bool> stop_requested_;

    // 文件日志相关
    static std::ofstream file_stream_;        ///< 当前打开的时间戳日志文件流
    static std::string file_log_path_;         ///< 文件日志完整路径
    static std::mutex file_mutex_;             ///< 保护 file_stream_ 的并发写入
    static bool file_log_active_;              ///< 文件日志是否激活
};

// ==================== 便捷日志宏 ====================

/// 基础宏：自动在 glog 输出前注入 [module] 前缀
#define LOG_INFO(module, msg)    LOG(INFO)    << "[" << module << "] " << msg
#define LOG_WARNING(module, msg) LOG(WARNING) << "[" << module << "] " << msg
#define LOG_ERROR(module, msg)   LOG(ERROR)   << "[" << module << "] " << msg
#define LOG_FATAL(module, msg)   LOG(FATAL)   << "[" << module << "] " << msg

/// printf 风格格式化宏：先 snprintf 到缓冲区，再送入 glog
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

/// 条件日志宏：condition 为 true 时才输出（glog LOG_IF 的包装）
#define LOG_IF_INFO(condition, module, msg) \
    LOG_IF(INFO, condition) << "[" << module << "] " << msg
#define LOG_IF_ERROR(condition, module, msg) \
    LOG_IF(ERROR, condition) << "[" << module << "] " << msg

/// DEBUG 专属宏：Release 模式下展开为空语句，零开销
#ifdef DEBUG
#define LOG_DEBUG(module, msg) LOG(INFO) << "[DEBUG][" << module << "] " << msg
#else
#define LOG_DEBUG(module, msg) do {} while(0)
#endif

/// 性能日志宏：记录操作耗时（ms），便于定位性能瓶颈
#define LOG_PERF(module, operation, duration_ms) \
    LOG(INFO) << "[" << module << "] PERF: " << operation << " took " << duration_ms << "ms"

/// 网络状态日志宏：统一网络接口状态变化的日志格式
#define LOG_NETWORK_STATE(module, interface, state) \
    LOG(INFO) << "[" << module << "] Network interface " << interface << " state: " << state

/// 事件日志宏：事件触发时的标准日志格式
#define LOG_EVENT(module, event_type, message) \
    LOG(INFO) << "[" << module << "] EVENT: " << event_type << " - " << message

/// 错误码日志宏：配合 system call 失败时输出 errno / 返回码
#define LOG_ERROR_WITH_CODE(module, operation, error_code) \
    LOG(ERROR) << "[" << module << "] " << operation << " failed with error code: " << error_code

} // namespace weaknet_dbus
