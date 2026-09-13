/**
 * @file test_tls_probe_client_gtest.cpp
 * @brief TlsProbeClient 的 HTTP 响应解析与失败路径
 *
 * ## 为什么用 socketpair 而不是连真实服务器
 *
 * 这些用例要验证的是**解析与超时逻辑**，不是真实 TLS 握手。
 * socketpair 提供一个确定性的对端，可以精确构造畸形响应、分片写入、
 * 慢速超时等边界，不依赖外网、证书有效期或第三方可用性。
 *
 * 真实 TLS/证书链/SNI 由真机验收覆盖（开发板上 openssl s_client 可作
 * 独立 oracle），单元测试只固定可确定的部分。
 *
 * ## 覆盖的服务语义
 *
 *   1. http_ok 与状态码解耦 —— 404/500 同样是合法响应。
 *      这是整个 HTTPS 判定的基石：把状态码纳入 http_ok 会让
 *      endpoint 的业务语义伪装成网络故障。
 *   2. Location 头提取（Portal 重定向判定的输入）。
 *   3. 畸形响应不得被误判为合法响应。
 *   4. 超时受 deadline 约束，绝不无限期阻塞。
 */

#include <gtest/gtest.h>

#include "tls_probe_client.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

using namespace weaknet_dbus;

namespace {

/// 一对已连接的 socket（模拟 monitor 交给 TLS 层的 fd）
///
/// **必须设为非阻塞**：TlsProbeClient 的契约是"调用方提供非阻塞 fd"，
/// 它依赖 EAGAIN + poll 实现 deadline 约束。用阻塞 fd 会让 recv 无限
/// 等待，测试本身就会挂死 —— 这正是本测试第一次运行时的失败原因。
struct SocketPair {
    int client{-1};
    int server{-1};

    SocketPair() {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
            client = fds[0];
            server = fds[1];
            const int fl = ::fcntl(client, F_GETFL, 0);
            ::fcntl(client, F_SETFL, fl | O_NONBLOCK);
        }
    }
    ~SocketPair() {
        if (client >= 0) ::close(client);
        if (server >= 0) ::close(server);
    }
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
};

/// 在后台线程写入给定字节后关闭写端
void respondAsync(int fd, std::string payload) {
    std::thread([fd, payload] {
        size_t sent = 0;
        while (sent < payload.size()) {
            const ssize_t n = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        ::shutdown(fd, SHUT_WR);
    }).detach();
}

TlsProbeClient::Config plainConfig(uint32_t deadline_ms = 2000) {
    TlsProbeClient::Config c;
    c.use_tls = false;      // 明文路径可在 socketpair 上确定性验证
    c.deadline_ms = deadline_ms;
    c.path = "/";
    return c;
}

}  // namespace

// --- 1. 正常响应 ---

TEST(TlsProbeClientTest, ParsesSimpleOkResponse) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    respondAsync(sp.server,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello");

    auto r = TlsProbeClient::run(sp.client, "example.com", plainConfig());
    EXPECT_TRUE(r.http_attempted);
    EXPECT_TRUE(r.http_ok);
    EXPECT_EQ(r.status_code, 200);
    EXPECT_EQ(r.content_type, "text/html");
    EXPECT_EQ(r.body_snippet, "hello");
    EXPECT_EQ(r.body_bytes, 5u);
}

// --- 2. 关键语义：错误状态码仍是合法响应 ---

TEST(TlsProbeClientTest, ErrorStatusIsStillAValidResponse) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    respondAsync(sp.server, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");

    auto r = TlsProbeClient::run(sp.client, "example.com", plainConfig());
    EXPECT_EQ(r.status_code, 500);
    EXPECT_TRUE(r.http_ok)
        << "收到 500 证明 transport 存在；http_ok 不得与状态码绑定";
}

TEST(TlsProbeClientTest, NotFoundIsStillAValidResponse) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    respondAsync(sp.server, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");

    auto r = TlsProbeClient::run(sp.client, "example.com", plainConfig());
    EXPECT_EQ(r.status_code, 404);
    EXPECT_TRUE(r.http_ok);
}

// --- 3. Location 提取（Portal 判定输入） ---

TEST(TlsProbeClientTest, ExtractsLocationHeaderForPortalDetection) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    respondAsync(sp.server,
        "HTTP/1.1 302 Found\r\n"
        "Location: http://portal.example.net/login?next=1\r\n"
        "Content-Length: 0\r\n"
        "\r\n");

    auto r = TlsProbeClient::run(sp.client, "check.example.com", plainConfig());
    EXPECT_EQ(r.status_code, 302);
    EXPECT_TRUE(r.http_ok);
    EXPECT_EQ(r.location, "http://portal.example.net/login?next=1")
        << "Location 是门户重定向判定的唯一输入";
}

TEST(TlsProbeClientTest, HeaderLookupIsCaseInsensitive) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    respondAsync(sp.server,
        "HTTP/1.1 302 Found\r\n"
        "LOCATION: http://x.example/portal\r\n"
        "Content-Length: 0\r\n"
        "\r\n");

    auto r = TlsProbeClient::run(sp.client, "check.example.com", plainConfig());
    EXPECT_EQ(r.location, "http://x.example/portal");
}

// --- 4. 畸形响应不得被当成合法响应 ---

TEST(TlsProbeClientTest, MalformedStatusLineIsNotAValidResponse) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    respondAsync(sp.server, "GARBAGE NOT HTTP\r\n\r\n");

    auto r = TlsProbeClient::run(sp.client, "example.com", plainConfig());
    EXPECT_FALSE(r.http_ok);
    EXPECT_EQ(r.http_detail, "malformed_status_line");
}

TEST(TlsProbeClientTest, EmptyResponseIsNotAValidResponse) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    ::shutdown(sp.server, SHUT_WR);   // 立刻 EOF

    auto r = TlsProbeClient::run(sp.client, "example.com", plainConfig());
    EXPECT_FALSE(r.http_ok);
    EXPECT_EQ(r.http_detail, "empty_response");
}

TEST(TlsProbeClientTest, OutOfRangeStatusCodeIsRejected) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    respondAsync(sp.server, "HTTP/1.1 999 Bogus\r\n\r\n");

    auto r = TlsProbeClient::run(sp.client, "example.com", plainConfig());
    EXPECT_FALSE(r.http_ok);
    EXPECT_EQ(r.http_detail, "malformed_status_line");
}

// --- 5. 分片到达仍应正确解析 ---

TEST(TlsProbeClientTest, HandlesFragmentedResponse) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    std::thread([fd = sp.server] {
        const char* parts[] = {"HTTP/1.1 20", "0 OK\r\nContent-Len", "gth: 3\r\n\r\nabc"};
        for (const char* p : parts) {
            ::send(fd, p, std::strlen(p), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ::shutdown(fd, SHUT_WR);
    }).detach();

    auto r = TlsProbeClient::run(sp.client, "example.com", plainConfig(3000));
    EXPECT_TRUE(r.http_ok);
    EXPECT_EQ(r.status_code, 200);
    EXPECT_EQ(r.body_snippet, "abc");
}

// --- 6. 超时受 deadline 约束 ---

TEST(TlsProbeClientTest, ReadTimeoutHonorsDeadline) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    // 不发送任何数据，也不关闭 —— 模拟对端挂起

    const auto t0 = std::chrono::steady_clock::now();
    auto r = TlsProbeClient::run(sp.client, "example.com", plainConfig(300));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    EXPECT_FALSE(r.http_ok);
    EXPECT_EQ(r.http_detail, "http_read_timeout");
    EXPECT_LT(ms, 3000) << "必须受 deadline 约束，不得无限期阻塞";
}

// 头部完整但正文超时：transport 已证明存在，不应算失败。
// 这条防止"响应体很大/很慢"被误读成"HTTPS 不可用"。
TEST(TlsProbeClientTest, BodyTimeoutAfterCompleteHeadersIsStillValid) {
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    std::thread([fd = sp.server] {
        const char* hdr = "HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n";
        ::send(fd, hdr, std::strlen(hdr), 0);
        // 声明了 10 万字节但一个都不发，保持连接不关闭
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }).detach();

    auto r = TlsProbeClient::run(sp.client, "example.com", plainConfig(300));
    EXPECT_TRUE(r.http_ok)
        << "头部已完整即证明得到合法 HTTP 响应，正文不完整不影响 transport 判定";
    EXPECT_EQ(r.status_code, 200);
}

// --- 7. 无 TLS 依赖时的诚实降级 ---
//
// 注意：这里必须查询**运行期** TlsProbeClient::available()，
// 不能用 WEAKNET_HAVE_TLS 宏。该宏是 server_lib 的 PRIVATE 编译定义
// （设计如此：公共头不暴露 OpenSSL 类型，不把 TLS 依赖传播给消费者），
// 因此本测试编译单元看不到它。available() 才是对外可观测的契约。

TEST(TlsProbeClientTest, TlsUnavailableBuildReportsHonestly) {
    if (TlsProbeClient::available()) {
        GTEST_SKIP() << "本构建具备 TLS；该分支由无 TLS 构建覆盖";
    }
    SocketPair sp;
    ASSERT_GE(sp.client, 0);
    TlsProbeClient::Config c;
    c.use_tls = true;
    auto r = TlsProbeClient::run(sp.client, "example.com", c);
    EXPECT_FALSE(r.tls_attempted);
    EXPECT_FALSE(r.tls_ok);
    EXPECT_EQ(r.tls_detail, "no_tls_probe_capability");
    EXPECT_FALSE(r.http_ok);
}

// 具备 TLS 的构建里，对端始终不回数据时必须受 deadline 约束地失败，
// 而不是挂死 —— 这条保证 TLS 阶段不会拖垮整个探测线程。
TEST(TlsProbeClientTest, SilentPeerTimesOutInsteadOfHanging) {
    if (!TlsProbeClient::available()) {
        GTEST_SKIP() << "本构建无 TLS";
    }
    SocketPair sp;
    ASSERT_GE(sp.client, 0);

    TlsProbeClient::Config c;
    c.use_tls = true;
    c.deadline_ms = 500;

    const auto t0 = std::chrono::steady_clock::now();
    auto r = TlsProbeClient::run(sp.client, "example.com", c);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    EXPECT_TRUE(r.tls_attempted);
    EXPECT_FALSE(r.tls_ok);
    EXPECT_EQ(r.tls_detail, "tls_timeout");
    EXPECT_FALSE(r.http_attempted) << "TLS 未成功时不应进入 HTTP 阶段";
    EXPECT_LT(ms, 5000) << "TLS 握手必须受 deadline 约束";
}

// available() 是对外可观测契约，必须与实现一致
TEST(TlsProbeClientTest, AvailableIsQueryable) {
    // 本仓库的 ARM64 容器与 x86 开发机均已就位 OpenSSL，
    // 因此这里的期望是 true；若变为 false 说明构建接线被破坏。
    EXPECT_TRUE(TlsProbeClient::available())
        << "构建已接通 OpenSSL（见 server/CMakeLists.txt 的 weaknet TLS 探测）；"
           "为 false 说明 TLS 依赖丢失，HTTPS 探测会静默降级为 NO_CAPABILITY";
}
