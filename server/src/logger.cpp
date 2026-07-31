// logger.cpp
// WeakNet 统一日志系统实现

#include "logger.hpp"
#include <iostream>
#include <filesystem>
#include <chrono>

namespace weaknet_dbus {

// 静态成员初始化
bool Logger::initialized_ = false;
std::string Logger::current_log_dir_ = "";

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

} // namespace weaknet_dbus
