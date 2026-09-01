/**
 * @file logger.cpp
 * @brief WeakNet 统一日志系统实现
 *
 * 本文件封装 glog（Google Logging Library）并扩展了两层能力：
 *   1. 自定义 FileLogSink：把 glog 日志回调转发到带时间戳的本地文件
 *   2. 文件日志开关/滚动清理：提供 startFileLog()/stopFileLog() 和 cleanOldLogs()
 *
 * 设计思路：
 *   - Logger 类仅包含静态成员，全局单例，通过 Logger::init() 初始化一次
 *   - FileLogSink 作为 glog 的 LogSink 子类，send() 回调中格式化日志行并写入文件
 *   - 文件日志写入刻意不加锁：send() 已在 glog 内部锁保护下，若再加锁会与
 *     其他"先持本锁再调用 LOG()"的代码形成 ABBA 死锁（详见 writeToFileLog 注释）
 *
 * 线程安全：
 *   - startFileLog/stopFileLog 通过 file_mutex_ 串行化开关状态
 *   - writeToFileLog 不加锁，依赖 glog 内部 Send() 调用的串行保证
 *   - signalHandler 仅写 std::atomic<bool>，可在信号处理上下文中安全调用
 */

#include "logger.hpp"
#include <cctype>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <atomic>

namespace weaknet_dbus {

bool parseLogLevel(const std::string& str, LogLevel* out) {
    std::string lower = str;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "info") { *out = LogLevel::INFO; return true; }
    if (lower == "warning" || lower == "warn") { *out = LogLevel::WARNING; return true; }
    if (lower == "error") { *out = LogLevel::ERROR; return true; }
    if (lower == "fatal") { *out = LogLevel::FATAL; return true; }
    return false;
}

// ---- 静态成员定义 ----
// 初始化标志，防止重复 InitGoogleLogging（glog 对多次初始化行为未定义）
bool Logger::initialized_ = false;
std::string Logger::current_log_dir_ = "";
std::atomic<bool> Logger::stop_requested_{false};

// 文件日志相关静态成员
std::ofstream Logger::file_stream_;
std::string Logger::file_log_path_;
std::mutex Logger::file_mutex_;
bool Logger::file_log_active_ = false;

/**
 * @brief 自定义 glog Sink：将日志同时写入带微秒时间戳的本地文件
 *
 * glog 每条日志最终都会回调 send()。本实现将原始消息拆分为时间戳、
 * 级别、模块名和正文，拼接成统一格式后写入 FileLogSink 绑定的 ofstream。
 * 模块名从 "[MODULE] 正文" 格式中解析：Logger 约定 LOG_INFO(LogModule::XXX, "...")
 * 展开后首字符必是 '['，便于自动提取模块标签。
 */
class FileLogSink : public google::LogSink {
public:
    void send(google::LogSeverity severity, const char* full_filename,
              const char* base_filename, int line,
              const struct tm* tm_time, const char* message,
              size_t message_len) override {
        // 未启用文件日志时直接返回，避免无效格式化开销
        if (!Logger::isFileLogActive()) {
            return;
        }

        // 时间戳：tm_time 精确到秒，再补微秒
        std::ostringstream time_ss;
        time_ss << std::put_time(tm_time, "%Y-%m-%d %H:%M:%S");

        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count() % 1000000;
        time_ss << "." << std::setfill('0') << std::setw(6) << microseconds;

        // glog 严重级别 → 可读字符串
        const char* level_str = "UNKNOWN";
        switch (severity) {
            case google::INFO:    level_str = "INFO"; break;
            case google::WARNING: level_str = "WARNING"; break;
            case google::ERROR:   level_str = "ERROR"; break;
            case google::FATAL:   level_str = "FATAL"; break;
            default: break;
        }

        // 从 message 中提取 [module] 格式的模块标签
        // WeakNet 的 LOG_INFO(LogModule::XXX, "...") 宏最终展开为 "[XXX] ..."
        std::string module = "SYSTEM";
        std::string msg(message, message_len);
        if (msg.size() > 2 && msg[0] == '[') {
            auto pos = msg.find(']');
            if (pos != std::string::npos && pos > 1) {
                module = msg.substr(1, pos - 1);
                msg = msg.substr(pos + 1);
                // 去除开头的空格，让正文紧凑
                if (!msg.empty() && msg[0] == ' ') {
                    msg = msg.substr(1);
                }
            }
        }

        // 格式: [2024-01-01 12:00:00.123456] [INFO] [EVENT_MGR] 日志正文
        std::ostringstream log_line;
        log_line << "[" << time_ss.str() << "] ["
                 << level_str << "] [" << module << "] " << msg;

        Logger::writeToFileLog(log_line.str());
    }
};

/** 全局 sink 实例，通过 google::AddLogSink 注册后所有线程共享 */
static FileLogSink g_file_log_sink;

/**
 * @brief 初始化日志系统（全局仅调用一次）
 * @param program_name 程序名，传给 glog 作为日志文件名前缀
 * @param log_dir      日志输出目录（将自动创建）
 * @param min_level    最低日志级别，低于此级别的消息会被过滤
 * @param log_to_stderr 是否同时输出到 stderr
 * @return true 初始化成功；false 目录创建或 glog 初始化失败
 */
bool Logger::init(const std::string& program_name,
                 const std::string& log_dir,
                 LogLevel min_level,
                 bool log_to_stderr) {
    // 幂等：避免重复 InitGoogleLogging 崩溃
    if (initialized_) {
        return true;
    }

    try {
        std::filesystem::create_directories(log_dir);

        // glog 全局配置项
        FLAGS_log_dir = log_dir;
        FLAGS_max_log_size = 10; // 单文件 10MB 滚动
        FLAGS_minloglevel = static_cast<int>(min_level);
        FLAGS_logtostderr = log_to_stderr;
        FLAGS_alsologtostderr = true;       // 同时写文件和 stderr
        FLAGS_colorlogtostderr = true;
        FLAGS_stop_logging_if_full_disk = true;  // 磁盘满时停止写日志，防止撑爆磁盘

        try {
            google::InitGoogleLogging(program_name.c_str());
        } catch (const std::exception& e) {
            std::cerr << "[logger] Failed to initialize glog: " << e.what() << std::endl;
            return false;
        } catch (...) {
            std::cerr << "[logger] Failed to initialize glog: unknown error" << std::endl;
            return false;
        }

        current_log_dir_ = log_dir;
        initialized_ = true;

        LOG(INFO) << "[logger] Logger initialized successfully";
        LOG(INFO) << "[logger] Log directory: " << log_dir;
        LOG(INFO) << "[logger] Min log level: " << static_cast<int>(min_level);
        LOG(INFO) << "[logger] Log to stderr: " << (log_to_stderr ? "true" : "false");

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[logger] Failed to initialize logger: " << e.what() << std::endl;
        return false;
    }
}

/**
 * @brief 关闭日志系统，刷新缓冲区并停止 glog
 */
void Logger::shutdown() {
    // 先停止文件日志，再关闭 glog；顺序不能反，否则 shutdown 前的最后一条日志会丢失
    stopFileLog();

    if (initialized_) {
        LOG(INFO) << "[logger] Shutting down logger";
        google::ShutdownGoogleLogging();
        initialized_ = false;
    }
}

/**
 * @brief 动态调整最低日志级别
 * @param level 新的最低级别，低于此级别的消息将被丢弃
 */
void Logger::setLogLevel(LogLevel level) {
    if (initialized_) {
        FLAGS_minloglevel = static_cast<int>(level);
        LOG(INFO) << "[logger] Log level changed to: " << static_cast<int>(level);
    }
}

/**
 * @brief 动态切换日志目录
 * @param dir 新的目录路径，会自动创建
 */
void Logger::setLogDir(const std::string& dir) {
    if (initialized_) {
        try {
            std::filesystem::create_directories(dir);
            FLAGS_log_dir = dir;
            current_log_dir_ = dir;
            LOG(INFO) << "[logger] Log directory changed to: " << dir;
        } catch (const std::exception& e) {
            LOG(ERROR) << "[logger] Failed to change log directory: " << e.what();
        }
    }
}

/**
 * @brief 清理超过指定天数的旧日志文件
 * @param log_dir     日志目录
 * @param max_age_days 保留天数，超过此天数的文件将被删除
 * @return 被删除的文件数量；目录不存在时返回 0
 */
int Logger::cleanOldLogs(const std::string& log_dir, int max_age_days) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // 目录不存在时安全返回 0
    if (!fs::exists(log_dir, ec) || !fs::is_directory(log_dir, ec)) {
        return 0;
    }

    int deleted = 0;
    const auto now = fs::file_time_type::clock::now();
    const auto max_age = std::chrono::hours(24 * max_age_days);

    try {
        for (const auto& entry : fs::directory_iterator(log_dir)) {
            if (!entry.is_regular_file(ec)) continue;
            auto file_time = fs::last_write_time(entry.path(), ec);
            if (ec) continue;
            if (now - file_time > max_age) {
                if (fs::remove(entry.path(), ec)) {
                    ++deleted;
                }
            }
        }
    } catch (const std::exception& e) {
        LOG(ERROR) << "[logger] cleanOldLogs failed: " << e.what();
    }

    return deleted;
}

// ---- 带时间戳的文件日志实现 ----

/**
 * @brief 生成文件名用的时间戳（格式 YYYYMMDD_HHMMSS）
 * @return 时间戳字符串
 */
std::string Logger::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_now;
    localtime_r(&time_t_now, &tm_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y%m%d_%H%M%S");
    return oss.str();
}

/**
 * @brief 启动文件日志，打开带时间戳的日志文件并注册 glog sink
 * @param logDir 日志文件存放目录
 * @note  幂等操作；已启动时调用直接返回
 */
void Logger::startFileLog(const std::string& logDir) {
    std::lock_guard<std::mutex> lock(file_mutex_);

    if (file_log_active_) {
        return;  // 已经启动
    }

    try {
        std::filesystem::create_directories(logDir);

        // 文件名: server_YYYYMMDD_HHMMSS.log
        std::string timestamp = getCurrentTimestamp();
        file_log_path_ = logDir + "/server_" + timestamp + ".log";

        // trunc 模式：每次启动重新生成，避免单文件无限膨胀
        file_stream_.open(file_log_path_, std::ios::out | std::ios::trunc);
        if (!file_stream_.is_open()) {
            std::cerr << "[logger] Failed to open file log: " << file_log_path_ << std::endl;
            return;
        }

        file_log_active_ = true;

        // 注册 glog sink —— 必须在 file_log_active_ 置 true 之后，
        // 否则 send() 会因 isFileLogActive() 返回 false 而跳过写入
        google::AddLogSink(&g_file_log_sink);

        // 写文件头，便于一眼看出日志会话的起始
        file_stream_ << "[System] File log started: " << file_log_path_ << std::endl;
        file_stream_.flush();

        std::cout << "[logger] File log started: " << file_log_path_ << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[logger] Failed to start file log: " << e.what() << std::endl;
    }
}

/**
 * @brief 停止文件日志，先移除 glog sink 再关闭文件句柄
 * @note  幂等操作；未启动时调用直接返回
 */
void Logger::stopFileLog() {
    std::lock_guard<std::mutex> lock(file_mutex_);

    if (!file_log_active_) {
        return;  // 未启动或已停止
    }

    try {
        // 顺序重要：先 RemoveLogSink，防止关闭后仍有日志回调进来
        google::RemoveLogSink(&g_file_log_sink);

        // 写入文件尾标记，便于排查崩溃场景下日志截断位置
        if (file_stream_.is_open()) {
            file_stream_ << "[System] File log stopped" << std::endl;
            file_stream_.flush();
            file_stream_.close();
        }

        file_log_active_ = false;

        std::cout << "[logger] File log stopped: " << file_log_path_ << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[logger] Failed to stop file log: " << e.what() << std::endl;
    }
}

/**
 * @brief 信号处理函数（仅标记退出标志）
 * @param signum 接收到的信号编号
 * @note  严格 async-signal-safe：只对 std::atomic<bool> 做 store，
 *       不得调用任何非 async-signal-safe 的函数（mutex、I/O、glog 自身等）
 */
void Logger::signalHandler(int signum) {
    // async-signal-safe: 仅设置标志，不调用任何函数（mutex/I/O 均非 async-signal-safe）
    stop_requested_.store(true, std::memory_order_relaxed);
}

/**
 * @brief 将一行格式化好的日志写入文件
 * @param line 完整的日志行（已包含时间戳、级别、模块名）
 *
 * 注意：此处刻意不加 file_mutex_。原因是 glog 的 Send() 在内部锁保护下调用本方法，
 * 若本方法再加锁，其他线程执行"先持 file_mutex_ 再调用 LOG()"时就会形成 ABBA 死锁：
 *   线程 A: glog_send → [glog_lock] → writeToFileLog → 等 file_mutex_
 *   线程 B: 业务代码 → [file_mutex_] → LOG() → 等 glog_lock
 * 由于 glog 保证 Send() 回调的串行执行，这里直接写入就是安全的。
 */
void Logger::writeToFileLog(const std::string& line) {
    if (!file_log_active_) {
        return;
    }

    // 注意：不再使用 file_mutex_ 加锁。
    // send() 在 glog 内部锁下调用，若 writeToFileLog 也持 file_mutex_，
    // 其他线程在 file_mutex_ 内调 LOG() → glog 锁 → 死锁。
    // glog 保证 Send() 回调的线程安全性，此处直接写入即可。
    if (file_stream_.is_open()) {
        file_stream_ << line << std::endl;
        file_stream_.flush();
    }
}

} // namespace weaknet_dbus
