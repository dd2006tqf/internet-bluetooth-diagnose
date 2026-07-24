// test_serializer.cpp
// 序列化工具单元测试
// 被测模块: serializer.cpp（字符串/int32/文件序列化）
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/unit -o test/bin/test_serializer
//        test/unit/test_serializer.cpp src/serializer.cpp src/logger.cpp -lglog

#include "test_common.hpp"
#include "serializer.hpp"
#include <climits>
#include <cstdio>

using namespace weaknet_dbus;

// 测试1: 字符串往返（含中文）
static void testStringRoundTrip() {
    TEST_CASE("字符串往返-含中文");
    std::vector<uint8_t> buf;
    serializeString("hello网络", buf);
    size_t offset = 0;
    std::string out;
    CHECK(deserializeString(buf, offset, out));
    CHECK_EQ(out, "hello网络");
}

// 测试2: 空字符串
static void testStringEmpty() {
    TEST_CASE("空字符串往返");
    std::vector<uint8_t> buf;
    serializeString("", buf);
    size_t offset = 0;
    std::string out;
    CHECK(deserializeString(buf, offset, out));
    CHECK(out.empty());
}

// 测试3: int32 往返（含边界值）
static void testInt32RoundTrip() {
    TEST_CASE("int32往返-含边界值");
    int32_t values[] = {0, -1, 1, INT32_MAX, INT32_MIN, 123456, -987654};
    for (int32_t v : values) {
        std::vector<uint8_t> buf;
        serializeInt32(v, buf);
        size_t offset = 0;
        int32_t out;
        CHECK(deserializeInt32(buf, offset, out));
        CHECK_EQ(out, v);
    }
}

// 测试4: 文件序列化往返（Get回复）
static void testGetReplyFileRoundTrip() {
    TEST_CASE("Get回复文件序列化往返");
    std::string tmpFile = "/tmp/weaknet_test_ser_getreply.bin";
    std::string err;
    CHECK(serializeGetReplyToFile("test reply 数据123", tmpFile, &err));
    std::string reply;
    CHECK(deserializeGetReplyFromFile(tmpFile, &reply, &err));
    CHECK_EQ(reply, "test reply 数据123");
    std::remove(tmpFile.c_str());
}

// 测试5: Changed 信号载荷文件序列化往返
static void testChangedPayloadFileRoundTrip() {
    TEST_CASE("Changed信号载荷文件序列化往返");
    std::string tmpFile = "/tmp/weaknet_test_ser_changed.bin";
    ChangedPayload payload{"变化消息", 42};
    std::string err;
    CHECK(serializeChangedPayloadToFile(payload, tmpFile, &err));
    ChangedPayload restored;
    CHECK(deserializeChangedPayloadFromFile(tmpFile, &restored, &err));
    CHECK_EQ(restored.message, "变化消息");
    CHECK_EQ(restored.counter, 42);
    std::remove(tmpFile.c_str());
}

// 测试6: 畸形缓冲区 - 空缓冲区反序列化应失败
static void testMalformedBuffer() {
    TEST_CASE("畸形缓冲区-空缓冲区应失败");
    std::vector<uint8_t> empty;
    size_t offset = 0;
    std::string out;
    CHECK(!deserializeString(empty, offset, out));
}

// 测试7: offset 越界应失败
static void testOffsetOverflow() {
    TEST_CASE("offset越界应失败");
    std::vector<uint8_t> buf = {0x01, 0x02, 0x03};
    size_t offset = 100;  // 超出缓冲区
    int32_t out;
    CHECK(!deserializeInt32(buf, offset, out));
}

// 测试8: 不存在的文件读取应失败
static void testNonexistentFile() {
    TEST_CASE("不存在的文件读取应失败");
    std::string err;
    std::string reply;
    CHECK(!deserializeGetReplyFromFile("/tmp/weaknet_nonexistent_file.bin", &reply, &err));
}

// 测试9: 连续序列化多个值
static void testSequentialSerialization() {
    TEST_CASE("连续序列化多个值");
    std::vector<uint8_t> buf;
    serializeString("first", buf);
    serializeInt32(100, buf);
    serializeString("second", buf);

    size_t offset = 0;
    std::string s1, s2;
    int32_t v;
    CHECK(deserializeString(buf, offset, s1));
    CHECK(deserializeInt32(buf, offset, v));
    CHECK(deserializeString(buf, offset, s2));
    CHECK_EQ(s1, "first");
    CHECK_EQ(v, 100);
    CHECK_EQ(s2, "second");
}

// 测试10: writeBufferToFile / readFileToBuffer
static void testBufferFileIO() {
    TEST_CASE("缓冲区文件读写");
    std::string tmpFile = "/tmp/weaknet_test_ser_buffer.bin";
    std::vector<uint8_t> original = {0x01, 0x02, 0x03, 0xFF, 0x00, 0x42};
    std::string err;
    CHECK(writeBufferToFile(original, tmpFile, &err));
    std::vector<uint8_t> restored;
    CHECK(readFileToBuffer(tmpFile, &restored, &err));
    CHECK_EQ(restored.size(), original.size());
    CHECK_EQ(restored[0], original[0]);
    CHECK_EQ(restored[3], original[3]);
    std::remove(tmpFile.c_str());
}

int main(int /*argc*/, char* argv[]) {
    initTestLogging(argv[0]);
    testStringRoundTrip();
    testStringEmpty();
    testInt32RoundTrip();
    testGetReplyFileRoundTrip();
    testChangedPayloadFileRoundTrip();
    testMalformedBuffer();
    testOffsetOverflow();
    testNonexistentFile();
    testSequentialSerialization();
    testBufferFileIO();
    return printTestResult();
}
