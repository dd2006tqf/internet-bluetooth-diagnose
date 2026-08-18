// test_logger_gtest.cpp
// Logger system unit tests (Google Test version)
// Module under test: logger.hpp / logger.cpp (format macros, disk-full protection, log cleanup)

#include <gtest/gtest.h>
#include "logger.hpp"

#include <functional>
#include <string>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>

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
};

// ============================================================================
// Test Cases
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
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
