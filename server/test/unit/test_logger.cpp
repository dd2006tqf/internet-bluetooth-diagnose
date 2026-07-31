// test_logger.cpp
// 日志系统单元测试
// 被测模块: logger.hpp / logger.cpp（格式化宏、磁盘满保护、日志清理）
// 编译: 见 Makefile test_logger 规则

#include "test_common.hpp"
#include "logger.hpp"

#include <functional>
#include <string>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>

using namespace weaknet_dbus;

// 捕获 fn() 期间写入 stderr（glog 在 FLAGS_logtostderr=true 时输出到 fd 2）的全部内容。
// 用 dup2 将 fd 2 重定向到临时文件，调用结束后恢复，再读回捕获内容。
static std::string captureStderr(const std::function<void()>& fn) {
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    if (saved < 0) return std::string();
    FILE* tmp = std::tmpfile();
    if (!tmp) { ::close(saved); return std::string(); }
    int tmpfd = ::fileno(tmp);
    ::dup2(tmpfd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    std::string out;
    std::rewind(tmp);
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) out.append(buf, n);
    std::fclose(tmp);
    return out;
}

// Task 1: LOG_INFO_F 应正确格式化 printf 风格参数
static void testLogInfoFFormat() {
    TEST_CASE("LOG_INFO_F 应格式化 %d/%s 参数");
    FLAGS_minloglevel = 0;  // 允许 INFO 输出以便捕获
    std::string out = captureStderr([]() {
        LOG_INFO_F(LogModule::SERVER, "count=%d name=%s", 42, "test");
    });
    std::cout << "[capture] " << out;  // 诊断输出：RED 阶段可见 %d/%s 未被替换
    CHECK_CONTAINS(out, "[server]");
    CHECK_CONTAINS(out, "count=42 name=test");
}

// Task 1: LOG_ERROR_F 应正确格式化 printf 风格参数
static void testLogErrorFFormat() {
    TEST_CASE("LOG_ERROR_F 应格式化 errno/strerror 风格参数");
    FLAGS_minloglevel = 0;
    std::string out = captureStderr([]() {
        LOG_ERROR_F(LogModule::NETWORK, "errno=%d msg=%s", 2, "No such file or directory");
    });
    std::cout << "[capture] " << out;
    CHECK_CONTAINS(out, "[network]");
    CHECK_CONTAINS(out, "errno=2 msg=No such file or directory");
}

// Task 2: Logger::init 应启用磁盘满保护（FLAGS_stop_logging_if_full_disk = true）
static void testDiskFullProtection() {
    TEST_CASE("Logger::init 应设置 FLAGS_stop_logging_if_full_disk = true");
    // init 已在 main 中调用，此处直接验证标志位
    CHECK_EQ(FLAGS_stop_logging_if_full_disk, true);
}

// Task 2: cleanOldLogs 应清理超期文件、保留未超期文件
static void testCleanOldLogs() {
    TEST_CASE("cleanOldLogs 清理超期文件并保留未超期文件");
    namespace fs = std::filesystem;
    const std::string tmp_dir = "./test_cleanlogs_dir";

    // 清理可能残留的同名目录，确保测试起始状态干净
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir, ec);

    // 创建"旧"文件并将修改时间设为 10 天前
    std::string old_file = tmp_dir + "/old.log";
    {
        FILE* f = std::fopen(old_file.c_str(), "w");
        std::fputs("old content", f);
        std::fclose(f);
    }
    auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(10 * 24);
    fs::last_write_time(old_file, old_time, ec);

    // 创建"新"文件（修改时间为当前）
    std::string new_file = tmp_dir + "/new.log";
    {
        FILE* f = std::fopen(new_file.c_str(), "w");
        std::fputs("new content", f);
        std::fclose(f);
    }

    // 调用 cleanOldLogs，清理超过 7 天的文件
    int deleted = Logger::cleanOldLogs(tmp_dir, 7);
    CHECK_EQ(deleted, 1);  // 应删除 1 个文件

    // 旧文件应被删除
    CHECK(fs::exists(old_file) == false);
    // 新文件应保留
    CHECK(fs::exists(new_file) == true);

    // 清理临时目录
    fs::remove_all(tmp_dir, ec);
}

// Task 2: cleanOldLogs 在目录不存在时应安全返回 0
static void testCleanOldLogsNonexistentDir() {
    TEST_CASE("cleanOldLogs 目录不存在时安全返回 0");
    int deleted = Logger::cleanOldLogs("./nonexistent_cleanlogs_dir_xyz", 7);
    CHECK_EQ(deleted, 0);
}

int main(int /*argc*/, char* argv[]) {
    // 使用 Logger::init 初始化 glog（替代 initTestLogging，避免重复初始化冲突，
    // 同时使 FLAGS_stop_logging_if_full_disk 测试能验证真实 init 流程）
    if (!Logger::init(argv[0], "./test_logs")) {
        std::cerr << "Failed to initialize logger" << std::endl;
        return 1;
    }
    testLogInfoFFormat();
    testLogErrorFFormat();
    testDiskFullProtection();
    testCleanOldLogs();
    testCleanOldLogsNonexistentDir();
    Logger::shutdown();
    return printTestResult();
}
