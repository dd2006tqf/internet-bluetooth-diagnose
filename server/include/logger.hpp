/**
 * @file logger.hpp
 * @brief WeakNet 统一日志系统：基于 Google glog + 文件日志（自动滚动）+ 便捷宏
 *
 * 封装 glog 的初始化/清理，额外提供：
 *   - 按模块标记的日志宏（LOG_INFO/LOG_ERROR 等带 [module] 前缀）
 *   - printf 风格的格式化日志宏（LOG_INFO_F/LOG_ERROR_F）
 *   - 运行期可开关的调试日志宏（LOG_DEBUG，日志级别为 debug 时输出）
 *   - 信号处理函数（SIGINT/SIGTERM 时仅置原子停止标志）
 *   - 时间戳命名的文件日志（server_YYYYMMDD_HHMMSS.log，按大小滚动 + 数量上限）
 *   - 日志文件自动清理（cleanOldLogs）
 *
 * 两级输出通道，分工明确：
 *   1. glog 自身的输出 → stderr（systemd 下即 journald），便于 journalctl 追踪；
 *   2. FileLogSink → 持久化目录下的 .log 文件，便于离线取证/回传。
 *   注意：glog 在 FLAGS_logtostderr=true 时**不会**写 FLAGS_log_dir，
 *   所以落盘完全由本项目的 sink 负责，滚动与保留策略也在 logger.cpp 内实现。
 *
 * 安全设计：
 *   stop_requested_ 是 std::atomic<bool>，在 signal handler 中是 safe 的
 *   （lock-free 原子变量在 C++11 后可在 signal handler 中使用）。
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <glog/logging.h>
#include <cstdio>
#include <string>
#include <fstream>
#include <mutex>
#include <ctime>
#include <csignal>

namespace weaknet_dbus {

/// 日志级别：映射到 glog 的 INFO/WARNING/ERROR/FATAL
///
/// DEBUG 的数值取 -1（排在 INFO 之前），只表达"开启调试输出"这一运行期语义：
/// 传给 glog 时会被夹到 INFO（避免动 glog 自身的 verbosity 机制），
/// 调试行是否输出由 Logger::debugEnabled() 单独决定。
enum class LogLevel {
    DEBUG = -1,
    INFO = 0,
    WARNING = 1,
    ERROR = 2,
    FATAL = 3
};

/// 将字符串日志级别（"debug"/"info"/"warning"/"warn"/"error"/"fatal"，大小写
/// 不敏感）解析为 LogLevel；非法值或空串返回 false
bool parseLogLevel(const std::string& str, LogLevel* out);

/// 校验并应用运行期日志级别（供 weaknet-cli set server.log_level 与云端下发共用）。
/// 成功返回 true；非法值返回 false 并填 error —— 调用方应据此回绝这次写入，
/// 否则会出现"配置写了、日志没变"的假成功。
bool applyLogLevel(const std::string& level, std::string* error);

/// 日志模块标识常量：统一模块名，便于在 grep 时过滤
///
/// 约定：eBPF 监控器的标签与 weaknet_config 中的监控器名保持一致
/// （dns / http_latency / tcp_rtt / ...），这样 `weaknet-cli get <name>` 与
/// `grep '\[<name>\]'` 用的是同一个词。
namespace LogModule {
    constexpr const char* SERVER       = "server";
    constexpr const char* CLIENT       = "client";
    constexpr const char* DBUS         = "dbus";
    constexpr const char* WEAK_MGR     = "weak_mgr";
    constexpr const char* TCP_LOSS     = "tcp_loss";
    constexpr const char* TCP_RETRANS  = "tcp_retrans";
    constexpr const char* TCP_CONN     = "tcp_conn";
    constexpr const char* TCP_CONNECT  = "tcp_connect";
    constexpr const char* TCP_RTT      = "tcp_rtt";
    constexpr const char* DNS          = "dns";
    constexpr const char* HTTP         = "http_latency";
    constexpr const char* SKB_DROP     = "skb_drop";
    constexpr const char* WIFI_LOSS    = "wifi_loss";
    constexpr const char* PROCESS_PROF = "process_profiler";
    constexpr const char* TRAFFIC      = "traffic";
    constexpr const char* EBPF         = "ebpf";
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
    /// 单个文件日志的滚动阈值（字节）。达到后关闭当前文件、开启新文件。
    /// 取值权衡：板端空间有限，频繁一点滚动换来有界占用而不是无限增长。
    static constexpr std::size_t kFileLogMaxBytes = 1024u * 1024u;

    /// 文件日志目录内保留的 .log 文件数量上限（滚动时删除最旧的）
    static constexpr int kFileLogMaxFiles = 5;

    /**
     * @brief 初始化日志系统
     *
     * 1. glog::InitGoogleLogging(program_name)
     * 2. 设置 glog 输出目录为 log_dir
     * 3. 设置 glog 最小日志级别
     * 4. 输出一行启动元信息（版本/构建类型/pid）
     *
     * @param program_name 程序名（用于 glog 日志文件名前缀）
     * @param log_dir      日志目录。既作为 glog 的 FLAGS_log_dir，也作为
     *                     startFileLog() 的默认目录。传空串表示不指定。
     * @param min_level    最小输出级别（默认 INFO；DEBUG 表示打开调试行）
     * @param log_to_stderr 是否输出到 stderr（默认 true；false 时 glog 才自己落盘）
     * @return true 初始化成功
     */
    static bool init(const std::string& program_name,
                    const std::string& log_dir = "./logs/server",
                    LogLevel min_level = LogLevel::INFO,
                    bool log_to_stderr = true);

    /// 关闭日志系统：写收尾日志 → 摘除文件 sink → ShutdownGoogleLogging
    static void shutdown();

    /// 当前生效的日志目录（供定时清理复用，避免各处写死相对路径）
    static const std::string& currentLogDir() { return current_log_dir_; }

    /// 动态调整日志级别（LogLevel::DEBUG 打开调试行）
    static void setLogLevel(LogLevel level);

    /// 当前是否开启调试输出（由 setLogLevel(LogLevel::DEBUG) 打开）
    static bool debugEnabled() { return debug_enabled_.load(std::memory_order_relaxed); }

    /**
     * @brief 安装崩溃信号处理器（glog 的 InstallFailureSignalHandler）
     *
     * 覆盖 SIGSEGV/SIGILL/SIGFPE/SIGABRT/SIGBUS 等致命信号，崩溃时把调用栈
     * 打到 stderr（systemd 下即 journald）。必须在其它线程创建之前调用。
     *
     * 只在服务端入口调用：客户端是动态库，给宿主进程装全局崩溃处理器不合适。
     * 注意：SDK 随后会为 SIGTERM/SIGINT 安装优雅退出的 handler，这会覆盖 glog
     * 对这两个信号的处理；崩溃类信号不受影响。
     */
    static void installCrashHandler();

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
     * 文件名格式：server_YYYYMMDD_HHMMSS.log。同秒重名时追加 _2/_3 后缀，
     * 不覆盖上一次会话的文件。目录不存在时自动创建。
     * 打开后每一条 LOG_INFO 等宏的输出会自动写入该文件（通过拦截 glog sink），
     * 单文件超过 kFileLogMaxBytes 自动滚动，目录内最多保留 kFileLogMaxFiles 个。
     *
     * @param logDir 日志文件目录；传空串则复用 init() 传入的目录
     */
    static void startFileLog(const std::string& logDir = "");
    static void stopFileLog();

    /// SIGINT/SIGTERM 处理：仅设 stop_requested_ 标志，由主线程轮询检测
    /// （async-signal-safe：只做一次原子 store）
    static void signalHandler(int signum);


    // ---------- 优雅停止 ----------

    /**
     * @brief 异步信号安全的停止标志
     *
     * stop_requested_ 是 std::atomic<bool>（lock-free），C++11 后 signal handler 可安全设置。
     * Looper 的信号处理器与 Logger::signalHandler 都会置位它；业务线程在循环中通过
     * stopRequested() 检测并优雅退出。
     */
    static bool stopRequested() { return stop_requested_.load(std::memory_order_relaxed); }

private:
    /// FileLogSink 是这两个内部入口的唯一消费者（glog 回调 → 文件写入）。
    /// 它们**不**对外暴露：外部若要写文件日志，直接用 LOG_* 宏即可，
    /// 绕过格式/滚动逻辑手工拼行只会产生不一致的证据。
    friend class FileLogSink;

    /// 文件日志是否处于激活状态（sink 的快速返回路径，无锁读）
    static bool isFileLogActive() { return file_log_active_.load(std::memory_order_relaxed); }

    /// 写入一行已格式化的日志（含滚动判断）。调用方须保证格式符合文件日志约定。
    static void writeToFileLog(const std::string& line);

    /// 打开一个新的时间戳日志文件并写文件头。调用方必须已持有 file_mutex_。
    /// @return 打开成功与否
    static bool openFileLog(const std::string& log_dir);

    /// 删除目录内最旧的若干个 .log，使总数不超过 kFileLogMaxFiles。
    /// 调用方必须已持有 file_mutex_；当前打开的文件永不被删除。
    static void pruneFileLogs(const std::string& log_dir);

    /// sink 路径上的滚动：当前文件超过 kFileLogMaxBytes 就关闭并换新文件。
    /// 调用方必须已持有 file_mutex_。
    static void rotateFileLogFromSink();

    static bool initialized_;
    static std::string current_log_dir_;
    static std::atomic<bool> stop_requested_;
    static std::atomic<bool> debug_enabled_;

    // 文件日志相关
    static std::ofstream file_stream_;         ///< 当前打开的时间戳日志文件流
    static std::string file_log_path_;         ///< 文件日志完整路径
    static std::string file_log_dir_;          ///< 文件日志目录（滚动时复用）
    static std::size_t file_log_bytes_;        ///< 当前文件已写字节数
    static std::mutex file_mutex_;             ///< 保护 file_stream_ 的并发写入
    static std::atomic<bool> file_log_active_; ///< 文件日志是否激活（sink 无锁读取）
};

// ==================== 便捷日志宏 ====================

/// 基础宏：自动在 glog 输出前注入 [module] 前缀
#define LOG_INFO(module, msg)                                                    \
    do {                                                                         \
        LOG(INFO) << "[" << (module) << "] " << msg;                             \
    } while (0)
#define LOG_WARNING(module, msg)                                                 \
    do {                                                                         \
        LOG(WARNING) << "[" << (module) << "] " << msg;                          \
    } while (0)
#define LOG_ERROR(module, msg)                                                   \
    do {                                                                         \
        LOG(ERROR) << "[" << (module) << "] " << msg;                            \
    } while (0)
#define LOG_FATAL(module, msg)                                                   \
    do {                                                                         \
        LOG(FATAL) << "[" << (module) << "] " << msg;                            \
    } while (0)

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

/// DEBUG 日志宏：运行期由日志级别控制（`weaknet-cli set server.log_level debug`）。
/// 关闭时只有一次原子读，消息流不会被求值，不会构造流表达式。
/// 历史上这里用 `#ifdef DEBUG` 守卫，但本项目的构建从不定义 DEBUG，
/// 导致该宏恒为空语句、16 处调试日志全部失效。
#define LOG_DEBUG(module, msg)                                                   \
    do {                                                                         \
        if (::weaknet_dbus::Logger::debugEnabled()) {                            \
            LOG(INFO) << "[DEBUG][" << (module) << "] " << msg;                  \
        }                                                                        \
    } while (0)

} // namespace weaknet_dbus
