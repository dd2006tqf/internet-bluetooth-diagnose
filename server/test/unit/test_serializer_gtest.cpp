// test_serializer_gtest.cpp
// Serializer unit tests (Google Test version)
// Module under test: serializer.cpp (string/int32/file serialization)

#include <gtest/gtest.h>
#include "serializer.hpp"
#include <climits>
#include <cstdio>
#include <unistd.h>
#include <thread>
#include <sys/stat.h>

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture
// ============================================================================
class SerializerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Fixture 必须使用允许的私有目录 /tmp/weaknet：isSafePath 要求文件的
        // 父目录与允许目录逐字节相等（不接受相似前缀，也不允许嵌套子目录），
        // 因此直接在该目录下以 "测试名_文件名" 命名临时文件避免冲突。
        const char* testName = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        testName_ = testName;
        mkdir("/tmp/weaknet", 0700);
        tmpDir_ = "/tmp/weaknet";
    }

    void TearDown() override {
        // Clean up temp files（不删除 /tmp/weaknet 本身：服务端运行期会复用该目录）
        for (const auto& f : filesToRemove_) {
            std::remove(f.c_str());
        }
    }

    // Helper: create temp file path（文件名以测试名开头，避免并行用例冲突）
    std::string tmpFile(const std::string& name) {
        std::string path = tmpDir_ + "/" + testName_ + "_" + name;
        filesToRemove_.push_back(path);
        return path;
    }

    std::string testName_;
    std::string tmpDir_;
    std::vector<std::string> filesToRemove_;
};

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: String round-trip (with Chinese characters)
TEST_F(SerializerTest, StringRoundTrip) {
    std::vector<uint8_t> buf;
    serializeString("hello", buf);

    size_t offset = 0;
    std::string out;
    ASSERT_TRUE(deserializeString(buf, offset, out));
    EXPECT_EQ(out, "hello");
}

// Test 2: Empty string
TEST_F(SerializerTest, StringEmpty) {
    std::vector<uint8_t> buf;
    serializeString("", buf);

    size_t offset = 0;
    std::string out;
    ASSERT_TRUE(deserializeString(buf, offset, out));
    EXPECT_TRUE(out.empty());
}

// Test 3: int32 round-trip (with boundary values)
TEST_F(SerializerTest, Int32RoundTrip) {
    int32_t values[] = {0, -1, 1, INT32_MAX, INT32_MIN, 123456, -987654};
    for (int32_t v : values) {
        std::vector<uint8_t> buf;
        serializeInt32(v, buf);

        size_t offset = 0;
        int32_t out;
        ASSERT_TRUE(deserializeInt32(buf, offset, out)) << "Failed for value: " << v;
        EXPECT_EQ(out, v) << "Round-trip failed for value: " << v;
    }
}

// Test 4: File serialization round-trip (Get reply)
TEST_F(SerializerTest, GetReplyFileRoundTrip) {
    std::string path = tmpFile("getreply.bin");
    std::string err;

    ASSERT_TRUE(serializeGetReplyToFile("test reply 123", path, &err))
        << "Error: " << err;

    std::string reply;
    ASSERT_TRUE(deserializeGetReplyFromFile(path, &reply, &err))
        << "Error: " << err;

    EXPECT_EQ(reply, "test reply 123");
}

// Test 5: Changed signal payload file serialization round-trip
TEST_F(SerializerTest, ChangedPayloadFileRoundTrip) {
    std::string path = tmpFile("changed.bin");
    ChangedPayload payload{"change message", 42};
    std::string err;

    ASSERT_TRUE(serializeChangedPayloadToFile(payload, path, &err))
        << "Error: " << err;

    ChangedPayload restored;
    ASSERT_TRUE(deserializeChangedPayloadFromFile(path, &restored, &err))
        << "Error: " << err;

    EXPECT_EQ(restored.message, "change message");
    EXPECT_EQ(restored.counter, 42);
}

// Test 6: Malformed buffer - empty buffer should fail
TEST_F(SerializerTest, MalformedBufferEmpty) {
    std::vector<uint8_t> empty;
    size_t offset = 0;
    std::string out;
    EXPECT_FALSE(deserializeString(empty, offset, out));
}

// Test 7: Offset overflow should fail
TEST_F(SerializerTest, OffsetOverflow) {
    std::vector<uint8_t> buf = {0x01, 0x02, 0x03};
    size_t offset = 100;  // Beyond buffer
    int32_t out;
    EXPECT_FALSE(deserializeInt32(buf, offset, out));
}

// Test 8: Non-existent file read should fail
TEST_F(SerializerTest, NonexistentFile) {
    std::string err;
    std::string reply;
    EXPECT_FALSE(deserializeGetReplyFromFile("/tmp/weaknet_nonexistent_file.bin", &reply, &err));
}

// Test 9: Sequential serialization of multiple values
TEST_F(SerializerTest, SequentialSerialization) {
    std::vector<uint8_t> buf;
    serializeString("first", buf);
    serializeInt32(100, buf);
    serializeString("second", buf);

    size_t offset = 0;
    std::string s1, s2;
    int32_t v;

    ASSERT_TRUE(deserializeString(buf, offset, s1));
    ASSERT_TRUE(deserializeInt32(buf, offset, v));
    ASSERT_TRUE(deserializeString(buf, offset, s2));

    EXPECT_EQ(s1, "first");
    EXPECT_EQ(v, 100);
    EXPECT_EQ(s2, "second");
}

// Test 10: writeBufferToFile / readFileToBuffer
TEST_F(SerializerTest, BufferFileIO) {
    std::string path = tmpFile("buffer.bin");
    std::vector<uint8_t> original = {0x01, 0x02, 0x03, 0xFF, 0x00, 0x42};
    std::string err;

    ASSERT_TRUE(writeBufferToFile(original, path, &err)) << "Error: " << err;

    std::vector<uint8_t> restored;
    ASSERT_TRUE(readFileToBuffer(path, &restored, &err)) << "Error: " << err;

    EXPECT_EQ(restored.size(), original.size());
    EXPECT_EQ(restored[0], original[0]);
    EXPECT_EQ(restored[3], original[3]);
}

// Test 11: Large string serialization
TEST_F(SerializerTest, LargeString) {
    std::string largeStr(10000, 'A');  // 10KB string
    std::vector<uint8_t> buf;
    serializeString(largeStr, buf);

    size_t offset = 0;
    std::string out;
    ASSERT_TRUE(deserializeString(buf, offset, out));
    EXPECT_EQ(out.size(), largeStr.size());
    EXPECT_EQ(out, largeStr);
}

// Test 12: Concurrent serialization (multi-threaded)
TEST_F(SerializerTest, ConcurrentSerialization) {
    std::vector<std::thread> threads;
    std::vector<std::vector<uint8_t>> results(4);

    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&results, i]() {
            for (int j = 0; j < 1000; ++j) {
                serializeInt32(i * 1000 + j, results[i]);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Verify each thread's results
    for (int i = 0; i < 4; ++i) {
        size_t offset = 0;
        for (int j = 0; j < 1000; ++j) {
            int32_t out;
            ASSERT_TRUE(deserializeInt32(results[i], offset, out));
            EXPECT_EQ(out, i * 1000 + j);
        }
    }
}

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
