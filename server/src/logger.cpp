/**
 * @file logger.cpp
 * @brief WeakNet 统一日志系统实现
 *
 * 本文件封装 glog（Google Logging Library）并扩展了两层能力：
 *   1. 自定义 FileLogSink：把 glog 日志回调转发到带时间戳的本地文件
 *   2. 文件日志滚动与清理：按大小滚动、按数量裁剪、按年龄清理
 *
 * 设计思路：
 *   - Logger 类仅包含静态成员，全局单例，通过 Logger::init() 初始化一次
 *   - FileLogSink 作为 glog 的 LogSink 子类，send() 回调中格式化日志行并写入文件
 *   - glog 在 logtostderr=true 时不写 FLAGS_log_dir，因此落盘完全由本文件的
 *     sink 负责；FLAGS_max_log_size 对本项目无效，滚动阈值由 kFileLogMaxBytes 决定
 *
 * 线程安全：
 *   - startFileLog/stopFileLog/writeToFileLog 通过 file_mutex_ 串行化
 *   - 全项目没有任何代码在持有 file_mutex_ 时调用 LOG()，因此 sink 路径取
 *     file_mutex_ 不会与 glog 内部锁形成 ABBA（该约束由本文件保证，
 *     startFileLog/stopFileLog 内部只用 std::cout / 直接写流，不写日志）
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
#include <cstring>
#include <unistd.h>
#include <vector>
#include <algorithm>

#ifndef WEAKNET_VERSION
#define WEAKNET_VERSION "0.0.0"
#endif

#ifndef WEAKNET_BUILD_TYPE
#define WEAKNET_BUILD_TYPE "unknown"
#endif

namespace weaknet_dbus {

bool parseLogLevel(const std::string& str, LogLevel* out) {
    // out 可以为空：调用方只想知道这个值合不合法（校验路径），
    // 不希望被迫声明一个自己用不到的变量。
    LogLevel parsed = LogLevel::INFO;
    std::string lower = str;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    bool ok = true;
    if (lower == "debug")   parsed = LogLevel::DEBUG;
    else if (lower == "info")  parsed = LogLevel::INFO;
    else if (lower == "warning" || lower == "warn") parsed = LogLevel::WARNING;
    else if (lower == "error") parsed = LogLevel::ERROR;
    else if (lower == "fatal") parsed = LogLevel::FATAL;
    else ok = false;

    // 失败时不写出参：避免调用方拿到一个"看起来解析成功"的值
    if (ok && out) *out = parsed;
    return ok;
}

bool applyLogLevel(const std::string& level, std::string* error) {
    LogLevel parsed;
    if (!parseLogLevel(level, &parsed)) {
        if (error) {
            *error = "invalid log_level '" + level +
                     "' (expected one of: debug/info/warning/error/fatal)";
        }
        return false;
    }
    Logger::setLogLevel(parsed);
    return true;
}

// ---- 静态成员定义 ----
// 初始化标志，防止重复 InitGoogleLogging（glog 对多次初始化行为未定义）
bool Logger::initialized_ = false;
std::string Logger::current_log_dir_ = "";
// 运行期停止标志：由 Looper 的信号处理器置位，业务线程轮询
std::atomic<bool> Logger::stop_requested_{false};
// 调试输出开关：日志级别为 debug 时打开（LOG_DEBUG 只读这个原子量）
std::atomic<bool> Logger::debug_enabled_{false};

// 文件日志相关静态成员
std::ofstream Logger::file_stream_;
std::string Logger::file_log_path_;
std::string Logger::file_log_dir_;
std::size_t Logger::file_log_bytes_ = 0;
std::mutex Logger::file_mutex_;
std::atomic<bool> Logger::file_log_active_{false};

/**
 * @brief 生成文件名用的时间戳（格式 YYYYMMDD_HHMMSS）
 */
static std::string makeTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::tm tm_now;
    localtime_r(&time_t_now, &tm_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y%m%d_%H%M%S");
    return oss.str();
}

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
        (void)full_filename;
        (void)base_filename;
        (void)line;

        // 未启用文件日志时直接返回，避免无效格式化开销
        if (!Logger::isFileLogActive()) {
            return;
        }

        // 时间戳：tm_time 精确到秒，再补微秒
        std::ostringstream time_ss;
        if (tm_time) {
            time_ss << std::put_time(tm_time, "%Y-%m-%d %H:%M:%S");
        } else {
            time_ss << "0000-00-00 00:00:00";
        }

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
 * @param log_dir      日志目录（将自动创建），同时作为 startFileLog() 的默认目录
 * @param min_level    最低日志级别，低于此级别的消息会被过滤
 * @param log_to_stderr 是否输出到 stderr
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
        if (!log_dir.empty()) {
            std::filesystem::create_directories(log_dir);
        }

        // DEBUG 不是 glog 的严重级别：夹到 INFO，调试行由 debug_enabled_ 控制
        const bool debug = (min_level == LogLevel::DEBUG);
        const int glog_level = debug ? static_cast<int>(LogLevel::INFO)
                                     : static_cast<int>(min_level);

        // glog 全局配置项
        if (!log_dir.empty()) {
            FLAGS_log_dir = log_dir;
        }
        FLAGS_max_log_size = 10; // 仅 logtostderr=false 时生效；本项目的滚动见 kFileLogMaxBytes
        FLAGS_minloglevel = glog_level;
        FLAGS_logtostderr = log_to_stderr;
        FLAGS_alsologtostderr = true;       // 同时写文件和 stderr
        FLAGS_colorlogtostderr = true;
        FLAGS_stop_logging_if_full_disk = true;  // 磁盘满时停止写日志，防止撑爆磁盘
        FLAGS_logbufsecs = 0;               // 立即 flush：崩溃前的最后几行必须留在盘上

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
        debug_enabled_.store(debug, std::memory_order_relaxed);

        LOG(INFO) << "[logger] Logger initialized successfully";
        // 启动元信息：版本/构建类型/pid 是排障时最先要确认的三件事，
        // 之前完全没有记录，只能靠 OTA 目录反推当前跑的到底是哪个版本。
        LOG(INFO) << "[system] weaknet version=" << WEAKNET_VERSION
                  << " build=" << WEAKNET_BUILD_TYPE
                  << " pid=" << static_cast<long>(::getpid())
                  << " program=" << program_name;
        LOG(INFO) << "[logger] Log directory: " << (log_dir.empty() ? "(none)" : log_dir);
        LOG(INFO) << "[logger] Min log level: " << glog_level
                  << (debug ? " (debug output enabled)" : "");
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
    if (!initialized_) {
        stopFileLog();
        return;
    }

    // 顺序：先写收尾日志（此时 sink 还在，文件里能留下这一行），
    // 再摘 sink 关文件，最后关 glog。
    LOG(INFO) << "[logger] Shutting down logger";
    stopFileLog();
    google::ShutdownGoogleLogging();
    initialized_ = false;
}

/**
 * @brief 动态调整最低日志级别
 * @param level 新的最低级别，低于此级别的消息将被丢弃
 */
void Logger::setLogLevel(LogLevel level) {
    const bool debug = (level == LogLevel::DEBUG);
    debug_enabled_.store(debug, std::memory_order_relaxed);
    if (!initialized_) {
        return;
    }
    FLAGS_minloglevel = debug ? static_cast<int>(LogLevel::INFO)
                              : static_cast<int>(level);
    LOG(INFO) << "[logger] Log level changed to: "
              << (debug ? "debug" : std::to_string(static_cast<int>(level)));
}

/**
 * @brief 安装崩溃信号处理器：崩溃时把调用栈打到 stderr（journald 可查）
 */
void Logger::installCrashHandler() {
    google::InstallFailureSignalHandler();
    LOG(INFO) << "[logger] Crash signal handler installed";
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
 * @brief 打开一个新的时间戳日志文件（调用方必须已持有 file_mutex_）
 *
 * 文件名 server_YYYYMMDD_HHMMSS.log；同秒重名时追加 _2/_3… 后缀，
 * 不覆盖上一次会话的文件（旧实现用 trunc，同秒重启会抹掉前一进程的日志）。
 */
bool Logger::openFileLog(const std::string& log_dir) {
    file_log_dir_ = log_dir;
    const std::string base = log_dir + "/server_" + makeTimestamp();

    std::error_code ec;
    std::string candidate;
    for (int i = 1; i <= 99; ++i) {
        candidate = (i == 1) ? (base + ".log")
                             : (base + "_" + std::to_string(i) + ".log");
        if (!std::filesystem::exists(candidate, ec)) {
            break;
        }
        candidate.clear();
    }
    if (candidate.empty()) {
        candidate = base + "_overflow.log";
    }

    file_stream_.open(candidate, std::ios::out | std::ios::trunc);
    if (!file_stream_.is_open()) {
        std::cerr << "[logger] Failed to open file log: " << candidate << std::endl;
        file_log_path_.clear();
        file_log_bytes_ = 0;
        return false;
    }

    file_log_path_ = candidate;
    file_log_bytes_ = 0;
    return true;
}

/**
 * @brief 删除目录内最旧的 .log，使总数不超过 kFileLogMaxFiles
 * （调用方必须已持有 file_mutex_）
 *
 * 数量上限是滚动的兜底：即使进程长期不重启、日志持续增长，
 * 磁盘占用也不会超过 kFileLogMaxFiles * kFileLogMaxBytes。
 */
void Logger::pruneFileLogs(const std::string& log_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;

    struct Entry { fs::file_time_type mtime; fs::path path; };
    std::vector<Entry> entries;
    try {
        for (const auto& e : fs::directory_iterator(log_dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            if (e.path().extension() != ".log") continue;
            auto t = fs::last_write_time(e.path(), ec);
            if (ec) { ec.clear(); continue; }
            entries.push_back({t, e.path()});
        }
    } catch (const std::exception&) {
        return;
    }

    if (static_cast<int>(entries.size()) <= kFileLogMaxFiles) {
        return;
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });

    int to_delete = static_cast<int>(entries.size()) - kFileLogMaxFiles;
    for (const auto& ent : entries) {
        if (to_delete-- <= 0) break;
        if (ent.path == fs::path(file_log_path_)) continue;  // 绝不删当前文件
        std::error_code rmec;
        fs::remove(ent.path, rmec);
    }
}

/**
 * @brief sink 路径上的滚动：超过阈值就换一个新文件
 *
 * 只在 writeToFileLog() 内调用（已持有 file_mutex_）。滚动期间会短暂
 * 关闭并重开文件，sink 的其余写入被同一把锁挡住，不会串到旧文件。
 */
void Logger::rotateFileLogFromSink() {
    if (file_stream_.is_open()) {
        file_stream_ << "[system] File log rotated (limit reached), starting new file" << std::endl;
        file_stream_.flush();
        file_stream_.close();
    }
    if (!openFileLog(file_log_dir_)) {
        // 打不开新文件就让 sink 整体停摆，避免每条日志都重试一次 open
        file_log_active_.store(false, std::memory_order_relaxed);
        return;
    }
    pruneFileLogs(file_log_dir_);
}

/**
 * @brief 启动文件日志，打开带时间戳的日志文件并注册 glog sink
 * @param logDir 日志文件存放目录；空串表示复用 init() 传入的目录
 * @note  幂等操作；已启动时调用直接返回
 *
 * 锁序：AddLogSink 会在 **file_mutex_ 之外** 调用。glog 在回调 send() 期间
 * 持有自己的 sink 互斥量；若这里持着 file_mutex_ 去调 AddLogSink，就形成
 *   日志线程: glog sink 锁 → send() → writeToFileLog → 等 file_mutex_
 *   启动线程: file_mutex_ → AddLogSink → 等 glog sink 锁
 * 的 ABBA 死锁。因此文件准备与 sink 注册必须分成两步。
 */
void Logger::startFileLog(const std::string& logDir) {
    {
        std::lock_guard<std::mutex> lock(file_mutex_);

        if (file_log_active_.load(std::memory_order_relaxed)) {
            return;  // 已经启动
        }

        const std::string dir = logDir.empty() ? current_log_dir_ : logDir;
        if (dir.empty()) {
            std::cerr << "[logger] startFileLog: no log directory configured" << std::endl;
            return;
        }

        try {
            std::filesystem::create_directories(dir);
        } catch (const std::exception& e) {
            std::cerr << "[logger] Failed to create log directory " << dir << ": " << e.what() << std::endl;
            return;
        }

        if (!openFileLog(dir)) {
            return;
        }

        // 先把上一次会话遗留的超量文件收掉，再开始写
        pruneFileLogs(dir);

        // 写文件头，便于一眼看出日志会话的起始。
        //
        // 版本/构建/pid 必须在这里再写一遍：init() 输出的那行元信息发生在
        // startFileLog() 之前，sink 还没挂上，文件里根本收不到。而文件日志
        // 正是"离线取证"的那一条通道——恰恰最需要知道这份日志是谁写的。
        file_stream_ << "[system] File log started: " << file_log_path_ << std::endl;
        file_stream_ << "[system] weaknet version=" << WEAKNET_VERSION
                     << " build=" << WEAKNET_BUILD_TYPE
                     << " pid=" << static_cast<long>(::getpid()) << std::endl;
        file_stream_.flush();

        // 置位必须在注册 sink 之前：send() 依据它决定是否写入
        file_log_active_.store(true, std::memory_order_relaxed);
    }

    // 见上方锁序说明：AddLogSink 不能与 file_mutex_ 同时持有
    google::AddLogSink(&g_file_log_sink);

    std::cout << "[logger] File log started: " << file_log_path_ << std::endl;
}

/**
 * @brief 停止文件日志，先移除 glog sink 再关闭文件句柄
 * @note  幂等操作；未启动时调用直接返回
 *
 * 锁序同 startFileLog()：RemoveLogSink 必须在 **file_mutex_ 之外** 调用。
 * 先把 file_log_active_ 置 false 可以立刻挡住新的 sink 写入，随后
 * RemoveLogSink 返回时保证没有在途回调，最后再安全地关闭文件流。
 */
void Logger::stopFileLog() {
    if (!file_log_active_.exchange(false, std::memory_order_relaxed)) {
        return;  // 未启动或已停止
    }

    // 顺序重要：先 RemoveLogSink，防止关闭后仍有日志回调进来
    google::RemoveLogSink(&g_file_log_sink);

    std::lock_guard<std::mutex> lock(file_mutex_);

    // 写入文件尾标记，便于排查崩溃场景下日志截断位置
    if (file_stream_.is_open()) {
        file_stream_ << "[system] File log stopped" << std::endl;
        file_stream_.flush();
        file_stream_.close();
    }

    std::cout << "[logger] File log stopped: " << file_log_path_ << std::endl;
}

/**
 * @brief 信号处理函数（仅标记退出标志）
 * @param signum 接收到的信号编号
 * @note  严格 async-signal-safe：只对 std::atomic<bool> 做 store，
 *       不得调用任何非 async-signal-safe 的函数（mutex、I/O、glog 自身等）
 */
void Logger::signalHandler(int signum) {
    (void)signum;
    // async-signal-safe: 仅设置标志，不调用任何函数（mutex/I/O 均非 async-signal-safe）
    stop_requested_.store(true, std::memory_order_relaxed);
}

/**
 * @brief 将一行格式化好的日志写入文件
 * @param line 完整的日志行（已包含时间戳、级别、模块名）
 *
 * 加锁说明：file_mutex_ 只被本函数与 start/stopFileLog 使用，且
 * start/stopFileLog 内部不会调用 LOG()（它们只写 std::cout 与文件流），
 * 因此不存在"持 file_mutex_ 再等 glog 锁"的路径，不会与 glog 内部锁形成 ABBA。
 * 早期版本为避免该风险选择完全不加锁，代价是 stopFileLog 关闭流时
 * 可能与 sink 写入并发，属于数据竞争。
 */
void Logger::writeToFileLog(const std::string& line) {
    if (!file_log_active_.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(file_mutex_);
    // 取锁期间可能已被 stopFileLog 关停
    if (!file_log_active_.load(std::memory_order_relaxed) || !file_stream_.is_open()) {
        return;
    }

    if (file_log_bytes_ + line.size() + 1 > kFileLogMaxBytes) {
        rotateFileLogFromSink();
        if (!file_log_active_.load(std::memory_order_relaxed) || !file_stream_.is_open()) {
            return;
        }
    }

    file_stream_ << line << std::endl;
    file_stream_.flush();
    file_log_bytes_ += line.size() + 1;
}

} // namespace weaknet_dbus
