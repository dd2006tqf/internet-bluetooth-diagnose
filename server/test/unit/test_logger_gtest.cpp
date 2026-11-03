// test_logger_gtest.cpp
// Logger system unit tests (Google Test version)
// Module under test: logger.hpp / logger.cpp
//   - printf 风格宏的格式化
//   - 磁盘写满保护
//   - 过期日志清理
//   - 日志级别解析/应用（运行期切换）
//   - DEBUG 宏的运行期开关
//   - 文件日志：命名、格式化、按大小滚动、数量上限、不覆盖旧会话

#include <gtest/gtest.h>
#include "logger.hpp"
#include "ebpf_log_bridge.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture
// ============================================================================
class LoggerTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Initialize logger once for all tests
        if (!Logger::init("test_logger", "./test_logs")) {
            std::cerr << "Failed to initialize logger" << std::endl;
        }
    }

    static void TearDownTestSuite() {
        Logger::shutdown();
    }

    void SetUp() override {
        // 每个用例都从 INFO 起步，避免上一个用例把级别留在 debug
        Logger::setLogLevel(LogLevel::INFO);
    }

    void TearDown() override {
        Logger::setLogLevel(LogLevel::INFO);
    }

    // Capture all content written to stderr during fn() execution
    // Uses dup2 to redirect fd 2 to a temporary file, restores after
    std::string captureStderr(const std::function<void()>& fn) {
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

    // 读入整个文件内容
    static std::string slurp(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // 列出目录内所有 .log 文件（按名字排序，便于断言）
    static std::vector<std::string> listLogs(const std::string& dir) {
        namespace fs = std::filesystem;
        std::vector<std::string> out;
        std::error_code ec;
        if (!fs::exists(dir, ec)) return out;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_regular_file() &&
                e.path().extension() == ".log") {
                out.push_back(e.path().string());
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }
};

// ============================================================================
// 1. 格式化宏
// ============================================================================

// Test 1: LOG_INFO_F should format printf-style parameters correctly
TEST_F(LoggerTest, LogInfoFFormat) {
    FLAGS_minloglevel = 0;  // Allow INFO output for capture
    std::string out = captureStderr([]() {
        LOG_INFO_F(LogModule::SERVER, "count=%d name=%s", 42, "test");
    });

    EXPECT_NE(out.find("[server]"), std::string::npos);
    EXPECT_NE(out.find("count=42 name=test"), std::string::npos);
}

// Test 2: LOG_ERROR_F should format errno/strerror style parameters correctly
TEST_F(LoggerTest, LogErrorFFormat) {
    FLAGS_minloglevel = 0;
    std::string out = captureStderr([]() {
        LOG_ERROR_F(LogModule::NETWORK, "errno=%d msg=%s", 2, "No such file or directory");
    });

    EXPECT_NE(out.find("[network]"), std::string::npos);
    EXPECT_NE(out.find("errno=2 msg=No such file or directory"), std::string::npos);
}

// ============================================================================
// 2. 磁盘与清理
// ============================================================================

// Test 3: Logger::init should enable disk-full protection
TEST_F(LoggerTest, DiskFullProtection) {
    EXPECT_TRUE(FLAGS_stop_logging_if_full_disk);
}

// Test 4: cleanOldLogs should clean expired files and keep non-expired files
TEST_F(LoggerTest, CleanOldLogs) {
    namespace fs = std::filesystem;
    const std::string tmp_dir = "./test_cleanlogs_dir";

    // Clean up any residual directory
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir, ec);

    // Create "old" file and set modification time to 10 days ago
    std::string old_file = tmp_dir + "/old.log";
    {
        FILE* f = std::fopen(old_file.c_str(), "w");
        std::fputs("old content", f);
        std::fclose(f);
    }
    auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(10 * 24);
    fs::last_write_time(old_file, old_time, ec);

    // Create "new" file (current modification time)
    std::string new_file = tmp_dir + "/new.log";
    {
        FILE* f = std::fopen(new_file.c_str(), "w");
        std::fputs("new content", f);
        std::fclose(f);
    }

    // Call cleanOldLogs, clean files older than 7 days
    int deleted = Logger::cleanOldLogs(tmp_dir, 7);
    EXPECT_EQ(deleted, 1);  // Should delete 1 file

    // Old file should be deleted
    EXPECT_FALSE(fs::exists(old_file));
    // New file should be kept
    EXPECT_TRUE(fs::exists(new_file));

    // Clean up temporary directory
    fs::remove_all(tmp_dir, ec);
}

// Test 5: cleanOldLogs should safely return 0 when directory doesn't exist
TEST_F(LoggerTest, CleanOldLogsNonexistentDir) {
    int deleted = Logger::cleanOldLogs("./nonexistent_cleanlogs_dir_xyz", 7);
    EXPECT_EQ(deleted, 0);
}

// ============================================================================
// 3. 日志级别解析
//
// 这一组用例锁的是"运行期改级别"这件事的两个历史缺陷：
//   - 之前 server.log_level 写入后根本没应用（假成功）
//   - 非法值被静默存进配置
// ============================================================================

TEST_F(LoggerTest, ParseLogLevelAcceptsAllDocumentedValues) {
    struct Case { const char* text; LogLevel expect; };
    const Case cases[] = {
        {"debug",   LogLevel::DEBUG},
        {"DEBUG",   LogLevel::DEBUG},
        {"info",    LogLevel::INFO},
        {"Info",    LogLevel::INFO},
        {"warning", LogLevel::WARNING},
        {"warn",    LogLevel::WARNING},
        {"error",   LogLevel::ERROR},
        {"fatal",   LogLevel::FATAL},
    };

    for (const auto& c : cases) {
        LogLevel got = LogLevel::INFO;
        EXPECT_TRUE(parseLogLevel(c.text, &got)) << "input: " << c.text;
        EXPECT_EQ(got, c.expect) << "input: " << c.text;
    }
}

TEST_F(LoggerTest, ParseLogLevelRejectsGarbage) {
    const char* bad[] = {"", "verbose", "trace", "info ", "  ", "0", "critical"};
    for (const char* text : bad) {
        LogLevel got = LogLevel::INFO;
        EXPECT_FALSE(parseLogLevel(text, &got)) << "input: '" << text << "'";
        // 失败时不得改动输出参数
        EXPECT_EQ(got, LogLevel::INFO) << "input: '" << text << "'";
    }
}

TEST_F(LoggerTest, ParseLogLevelToleratesNullOut) {
    // 只做合法性检查时不该要求写出参数
    EXPECT_TRUE(parseLogLevel("warning", nullptr));
    EXPECT_FALSE(parseLogLevel("nope", nullptr));
}

TEST_F(LoggerTest, ApplyLogLevelReportsInvalidValue) {
    std::string err;
    EXPECT_FALSE(applyLogLevel("verbose", &err));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("verbose"), std::string::npos);

    // 失败不得改变当前级别
    EXPECT_FALSE(Logger::debugEnabled());

    // 成功路径
    err.clear();
    EXPECT_TRUE(applyLogLevel("debug", &err));
    EXPECT_TRUE(Logger::debugEnabled());

    EXPECT_TRUE(applyLogLevel("error", nullptr));
    EXPECT_FALSE(Logger::debugEnabled());
}

// ============================================================================
// 4. DEBUG 宏的运行期开关
//
// 历史上 LOG_DEBUG 由 #ifdef DEBUG 守卫，而本项目两种构建都不定义 DEBUG，
// 于是 16 处调试日志全部失效。现在改为运行期原子开关。
// ============================================================================

TEST_F(LoggerTest, DebugMacroSilentByDefault) {
    FLAGS_minloglevel = 0;
    ASSERT_FALSE(Logger::debugEnabled());

    std::string out = captureStderr([]() {
        LOG_DEBUG(LogModule::EBPF, "should-not-appear");
    });
    EXPECT_EQ(out.find("should-not-appear"), std::string::npos)
        << "debug 关闭时不应产生任何输出";
}

TEST_F(LoggerTest, DebugMacroEmitsWhenEnabled) {
    FLAGS_minloglevel = 0;
    Logger::setLogLevel(LogLevel::DEBUG);
    ASSERT_TRUE(Logger::debugEnabled());

    std::string out = captureStderr([]() {
        LOG_DEBUG(LogModule::EBPF, "trace-marker-42");
    });
    EXPECT_NE(out.find("[DEBUG][ebpf]"), std::string::npos);
    EXPECT_NE(out.find("trace-marker-42"), std::string::npos);
}

TEST_F(LoggerTest, DebugMacroDoesNotEvaluateMessageWhenDisabled) {
    // 关闭时只有一次原子读，消息表达式不应被求值
    int evaluated = 0;
    captureStderr([&evaluated]() {
        LOG_DEBUG(LogModule::EBPF, "x=" << ++evaluated);
    });
    EXPECT_EQ(evaluated, 0);
}

// ============================================================================
// 5. 文件日志
// ============================================================================

TEST_F(LoggerTest, FileLogWritesFormattedLines) {
    namespace fs = std::filesystem;
    const std::string dir = "./test_filelog_dir";
    std::error_code ec;
    fs::remove_all(dir, ec);

    Logger::startFileLog(dir);

    LOG_INFO(LogModule::DNS, "filelog-marker-77");
    Logger::stopFileLog();

    const auto files = listLogs(dir);
    ASSERT_EQ(files.size(), 1u) << "应恰好生成一个 .log";
    EXPECT_NE(files[0].find("server_"), std::string::npos);
    EXPECT_EQ(fs::path(files[0]).extension().string(), ".log");

    const std::string body = slurp(files[0]);
    EXPECT_NE(body.find("filelog-marker-77"), std::string::npos);
    // 行首时间戳 + 级别 + 模块，三者都落到文件里才能离线定位
    EXPECT_NE(body.find("[INFO]"), std::string::npos);
    EXPECT_NE(body.find("[dns]"), std::string::npos);
    EXPECT_NE(body.find("File log started"), std::string::npos);

    fs::remove_all(dir, ec);
}

TEST_F(LoggerTest, FileLogDoesNotClobberPreviousSession) {
    namespace fs = std::filesystem;
    const std::string dir = "./test_filelog_nowrap_dir";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // 预置一个与首文件名同名的旧文件，模拟同秒重启
    Logger::startFileLog(dir);
    std::string first;
    {
        const auto files = listLogs(dir);
        ASSERT_EQ(files.size(), 1u);
        first = files[0];
    }
    LOG_INFO(LogModule::SERVER, "first-session-content");
    Logger::stopFileLog();

    Logger::startFileLog(dir);
    LOG_INFO(LogModule::SERVER, "second-session-content");
    Logger::stopFileLog();

    const auto files = listLogs(dir);
    EXPECT_GE(files.size(), 2u) << "第二次启动必须换新文件，不能截断旧文件";
    EXPECT_NE(slurp(first).find("first-session-content"), std::string::npos)
        << "旧会话内容必须保留";

    fs::remove_all(dir, ec);
}

TEST_F(LoggerTest, FileLogRotatesWhenExceedingMaxBytes) {
    namespace fs = std::filesystem;
    const std::string dir = "./test_filelog_rotate_dir";
    std::error_code ec;
    fs::remove_all(dir, ec);

    Logger::startFileLog(dir);

    // 写到远超阈值；每行约 100 字节
    const std::string payload(80, 'x');
    const int rounds = static_cast<int>(Logger::kFileLogMaxBytes / 80) + 50;
    for (int i = 0; i < rounds; ++i) {
        LOG_INFO(LogModule::SERVER, "rotate-" << i << "-" << payload);
    }
    Logger::stopFileLog();

    const auto files = listLogs(dir);
    EXPECT_GE(files.size(), 2u) << "超过 kFileLogMaxBytes 必须滚动出新文件";

    // 无单文件超过阈值太多（允许一行溢出，因为写入前才判断）
    for (const auto& f : files) {
        const auto size = fs::file_size(f, ec);
        EXPECT_LE(size, Logger::kFileLogMaxBytes + 4096)
            << "文件 " << f << " 超过滚动阈值";
    }

    fs::remove_all(dir, ec);
}

TEST_F(LoggerTest, FileLogPrunesToMaxFiles) {
    namespace fs = std::filesystem;
    const std::string dir = "./test_filelog_prune_dir";
    std::error_code ec;
    fs::remove_all(dir, ec);

    Logger::startFileLog(dir);

    // 写到足够多的轮次，产生远多于 kFileLogMaxFiles 的文件
    const std::string payload(200, 'y');
    const std::size_t per_file = Logger::kFileLogMaxBytes;
    const int rounds = static_cast<int>(per_file / payload.size()) * 20;

    for (int i = 0; i < rounds; ++i) {
        LOG_INFO(LogModule::SERVER, "prune-" << i << "-" << payload);
    }
    Logger::stopFileLog();

    const auto files = listLogs(dir);
    EXPECT_LE(files.size(), static_cast<std::size_t>(Logger::kFileLogMaxFiles))
        << "目录内 .log 数量必须有上限，否则板端空间会被吃光";

    fs::remove_all(dir, ec);
}

TEST_F(LoggerTest, FileLogStartIsIdempotentWhileActive) {
    namespace fs = std::filesystem;
    const std::string dir = "./test_filelog_idem_dir";
    std::error_code ec;
    fs::remove_all(dir, ec);

    Logger::startFileLog(dir);

    // 重复 start 不应产生第二个文件，也不应让流处于异常状态
    Logger::startFileLog(dir);

    LOG_INFO(LogModule::SERVER, "idem-marker");
    Logger::stopFileLog();

    const auto files = listLogs(dir);
    EXPECT_EQ(files.size(), 1u);
    EXPECT_NE(slurp(files[0]).find("idem-marker"), std::string::npos);

    fs::remove_all(dir, ec);
}

TEST_F(LoggerTest, FileLogUsesInitDirectoryWhenArgEmpty) {
    namespace fs = std::filesystem;
    const std::string dir = "./test_filelog_default_dir";

    // 换一个目录重新 init，再以空参数 start，验证目录复用
    Logger::shutdown();
    ASSERT_TRUE(Logger::init("test_logger", dir));

    Logger::startFileLog();
    LOG_INFO(LogModule::SERVER, "default-dir-marker");
    Logger::stopFileLog();
    Logger::shutdown();

    const auto files = listLogs(dir);
    ASSERT_FALSE(files.empty()) << "空参数必须回落到 init() 传入的目录";
    EXPECT_NE(slurp(files[0]).find("default-dir-marker"), std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);

    // 恢复套件级初始化，避免后续用例没有 logger
    ASSERT_TRUE(Logger::init("test_logger", "./test_logs"));
}

TEST_F(LoggerTest, LogMetadataLineCarriesVersionAndPid) {
    // 启动元信息是现场排障的第一手资料：版本对不上时，日志内容也不可信。
    // 这条断言确认 init() 真的把它写出来了，而不是只在头文件里承诺。
    FLAGS_minloglevel = 0;
    Logger::shutdown();
    std::string out = captureStderr([]() {
        Logger::init("test_logger", "./test_logs");
    });

    EXPECT_NE(out.find("version="), std::string::npos);
    EXPECT_NE(out.find("pid="), std::string::npos);
    EXPECT_NE(out.find("build="), std::string::npos);

    Logger::shutdown();
    ASSERT_TRUE(Logger::init("test_logger", "./test_logs"));
}

// ============================================================================
// 6. libbpf 桥接
//
// 不依赖内核查验器：这里只验证"没装桥/没输出"时的安全行为 ——
// 空缓存必须返回空串（调用方据此不追加任何东西），
// 而且安装动作必须可重入。
// ============================================================================

TEST_F(LoggerTest, LibbpfBridgeIsSafeWithoutLoad) {
    clearLibbpfError();
    EXPECT_TRUE(takeLibbpfError().empty());

    installLibbpfLogBridge();
    installLibbpfLogBridge();  // 幂等

    // 没有 BPF 加载时不应凭空产生错误文本
    EXPECT_TRUE(takeLibbpfError().empty());
}

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
