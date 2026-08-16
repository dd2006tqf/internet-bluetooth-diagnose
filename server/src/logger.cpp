// logger.cpp
// WeakNet 统一日志系统实现

#include "logger.hpp"
#include <iostream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <csignal>

namespace weaknet_dbus {

// 静态成员初始化
bool Logger::initialized_ = false;
std::string Logger::current_log_dir_ = "";

// 文件日志相关静态成员
std::ofstream Logger::file_stream_;
std::string Logger::file_log_path_;
std::mutex Logger::file_mutex_;
bool Logger::file_log_active_ = false;

// ---- 自定义 glog Sink：将日志同时写入时间戳文件 ----
class FileLogSink : public google::LogSink {
public:
    void send(google::LogSeverity severity, const char* full_filename,
              const char* base_filename, int line,
              const struct tm* tm_time, const char* message,
              size_t message_len) override {
        if (!Logger::isFileLogActive()) {
            return;
        }

        // 格式化时间戳
        std::ostringstream time_ss;
        time_ss << std::put_time(tm_time, "%Y-%m-%d %H:%M:%S");

        // 获取微秒部分（从系统时钟）
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count() % 1000000;
        time_ss << "." << std::setfill('0') << std::setw(6) << microseconds;

        // 日志级别
        const char* level_str = "UNKNOWN";
        switch (severity) {
            case google::INFO:    level_str = "INFO"; break;
            case google::WARNING: level_str = "WARNING"; break;
            case google::ERROR:   level_str = "ERROR"; break;
            case google::FATAL:   level_str = "FATAL"; break;
            default: break;
        }

        // 提取模块名（从 message 中查找 [module] 格式）
        std::string module = "SYSTEM";
        std::string msg(message, message_len);
        if (msg.size() > 2 && msg[0] == '[') {
            auto pos = msg.find(']');
            if (pos != std::string::npos && pos > 1) {
                module = msg.substr(1, pos - 1);
                msg = msg.substr(pos + 1);
                // 去除开头的空格
                if (!msg.empty() && msg[0] == ' ') {
                    msg = msg.substr(1);
                }
            }
        }

        // 构造日志行
        std::ostringstream log_line;
        log_line << "[" << time_ss.str() << "] ["
                 << level_str << "] [" << module << "] " << msg;

        // 写入文件
        Logger::writeToFileLog(log_line.str());
    }
};

// 全局 sink 实例
static FileLogSink g_file_log_sink;

bool Logger::init(const std::string& program_name,
                 const std::string& log_dir,
                 LogLevel min_level,
                 bool log_to_stderr) {
    if (initialized_) {
        return true;
    }

    try {
        // 创建日志目录
        std::filesystem::create_directories(log_dir);

        // 设置日志目录
        FLAGS_log_dir = log_dir;
        FLAGS_max_log_size = 10; // 10MB
        FLAGS_minloglevel = static_cast<int>(min_level);
        FLAGS_logtostderr = log_to_stderr;
        FLAGS_alsologtostderr = true;
        FLAGS_colorlogtostderr = true;
        FLAGS_stop_logging_if_full_disk = true;  // 磁盘满时停止写日志，防止撑爆磁盘

        // 初始化glog
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

void Logger::shutdown() {
    // 先停止文件日志
    stopFileLog();

    if (initialized_) {
        LOG(INFO) << "[logger] Shutting down logger";
        google::ShutdownGoogleLogging();
        initialized_ = false;
    }
}

void Logger::setLogLevel(LogLevel level) {
    if (initialized_) {
        FLAGS_minloglevel = static_cast<int>(level);
        LOG(INFO) << "[logger] Log level changed to: " << static_cast<int>(level);
    }
}

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

void Logger::startFileLog(const std::string& logDir) {
    std::lock_guard<std::mutex> lock(file_mutex_);

    if (file_log_active_) {
        return;  // 已经启动
    }

    try {
        // 创建目录
        std::filesystem::create_directories(logDir);

        // 生成带时间戳的文件名
        std::string timestamp = getCurrentTimestamp();
        file_log_path_ = logDir + "/server_" + timestamp + ".log";

        // 打开文件
        file_stream_.open(file_log_path_, std::ios::out | std::ios::trunc);
        if (!file_stream_.is_open()) {
            std::cerr << "[logger] Failed to open file log: " << file_log_path_ << std::endl;
            return;
        }

        file_log_active_ = true;

        // 注册 glog sink（在文件日志激活后）
        google::AddLogSink(&g_file_log_sink);

        // 写入文件头
        file_stream_ << "[System] File log started: " << file_log_path_ << std::endl;
        file_stream_.flush();

        std::cout << "[logger] File log started: " << file_log_path_ << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[logger] Failed to start file log: " << e.what() << std::endl;
    }
}

void Logger::stopFileLog() {
    std::lock_guard<std::mutex> lock(file_mutex_);

    if (!file_log_active_) {
        return;  // 未启动或已停止
    }

    try {
        // 移除 glog sink（在文件日志关闭前）
        google::RemoveLogSink(&g_file_log_sink);

        // 写入文件尾
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

void Logger::signalHandler(int signum) {
    // 仅停止文件日志，不退出进程（由 main 的信号处理逻辑决定是否退出）
    stopFileLog();
}

void Logger::writeToFileLog(const std::string& line) {
    if (!file_log_active_) {
        return;
    }

    std::lock_guard<std::mutex> lock(file_mutex_);
    if (file_stream_.is_open()) {
        file_stream_ << line << std::endl;
        file_stream_.flush();
    }
}

} // namespace weaknet_dbus
