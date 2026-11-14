/**
 * @file test_video_frame_dbus_gtest.cpp
 * @brief GetVideoFrame / ReleaseVideoFrame 的 D-Bus 线协议回归测试
 *
 * ============================================================================
 * 为什么这个测试必须走**真实总线**，而不是直接调 handleXxx
 * ============================================================================
 *
 * 本套件要钉住的问题是「回复消息的签名与字段数」，而它**只存在于序列化层**：
 *
 *   - 直接调 `handleGetVideoFrame(conn, msg)` 只能看到「函数返回了 true」；
 *   - 只有让 libdbus 真的序列化、经 dbus-daemon 转发、再由客户端反序列化，
 *     签名错了才会暴露。
 *
 * 这不是假设。2026-11-14 实现时，无帧分支原本写 `fd = -1`，看起来完全合理
 * （负数表示无效）。实测结果：
 *
 *   append(DBUS_TYPE_UNIX_FD, -1) → 返回 0（libdbus 拒绝这个值）
 *
 * 当时代码忽略了返回值，于是消息**静默**少了 fd 字段，签名从 `huuuuuttu`
 * 变成 `uuuuuttu`。客户端按 `h` 读会读到后面的 `u`，且**不报错**——对照
 * 实验里客户端打印出 `arg0 type = 'u'`。
 *
 * 后果具体：面板会拿到一个「把宽度当成 fd」的回复，然后 close 一个它以为是
 * fd 的数字。不会崩溃、不打日志，只表现为「画面永远不出来」。
 *
 * 所以本测试的判据是**从客户端侧解析到的字段**，不是服务端的返回值。
 *
 * ============================================================================
 * 测试隔离
 * ============================================================================
 *
 * 与 test_looper_dispatch_gtest 同一手法：自己拉起 `dbus-daemon --session`
 * 子进程，用完即杀，不依赖环境里是否已有会话总线（CI 与板端都没有）。
 * 拉不起来就 SKIP 并打印原因，绝不伪通过。
 */

#include <gtest/gtest.h>

#include <dbus/dbus.h>

#include "common.hpp"
#include "dbus_service.hpp"
#include "server.hpp"
#include "video_frame_hub.hpp"

// ServerContext 的析构需要这些成员的完整类型（unique_ptr 的删除器要求）。
// 清单与 test_bt_events_lifecycle_gtest.cpp 保持一致——那里同样会构造
// ServerContext，缺一个就报 "invalid application of sizeof to incomplete type"。
#include "active_connectivity_monitor.hpp"
#include "database_manager.hpp"
#include "monitor_plugin.hpp"
#include "weak_netmgr.hpp"
#include "wireless_event_store.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using weaknet_dbus::DbusService;
using weaknet_dbus::ServerContext;

// ---------------------------------------------------------------------------
// 私有总线（与 test_looper_dispatch_gtest 同构）
//
// 不抽成共享头文件是刻意的：两个套件的断言对象完全不同（一个测派发层，
// 一个测线协议），公共化只会让二者的改动互相牵连。
// ---------------------------------------------------------------------------

class PrivateBus {
public:
    PrivateBus() {
        int out[2];
        if (pipe(out) != 0) {
            reason_ = std::string("pipe() failed: ") + std::strerror(errno);
            return;
        }
        pid_ = fork();
        if (pid_ < 0) {
            reason_ = std::string("fork() failed: ") + std::strerror(errno);
            ::close(out[0]);
            ::close(out[1]);
            return;
        }
        if (pid_ == 0) {
            dup2(out[1], STDOUT_FILENO);
            ::close(out[0]);
            ::close(out[1]);
            signal(SIGPIPE, SIG_IGN);
            execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork",
                   "--print-address=1", static_cast<char*>(nullptr));
            _exit(127);
        }
        ::close(out[1]);

        char buf[512];
        std::string line;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const ssize_t n = ::read(out[0], buf, sizeof(buf));
            if (n > 0) {
                line.append(buf, static_cast<size_t>(n));
                const auto nl = line.find('\n');
                if (nl != std::string::npos) {
                    line.resize(nl);
                    break;
                }
            } else if (n == 0) {
                break;
            } else if (errno != EINTR) {
                break;
            }
        }
        ::close(out[0]);

        if (line.empty()) {
            reason_ = "dbus-daemon 未输出总线地址（未安装或启动失败）";
            killDaemon();
            return;
        }
        address_ = line;
    }

    ~PrivateBus() { killDaemon(); }
    PrivateBus(const PrivateBus&) = delete;
    PrivateBus& operator=(const PrivateBus&) = delete;

    bool ok() const { return !address_.empty() && pid_ > 0; }
    const std::string& address() const { return address_; }
    const std::string& reason() const { return reason_; }

private:
    void killDaemon() {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGTERM);
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return;
            }
            ::usleep(100 * 1000);
        }
        ::kill(pid_, SIGKILL);
        (void)::waitpid(pid_, &status, 0);
        pid_ = -1;
    }

    pid_t pid_ = -1;
    std::string address_;
    std::string reason_;
};

DBusConnection* openPrivate(const std::string& address) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection* c = dbus_connection_open_private(address.c_str(), &err);
    if (!c) {
        dbus_error_free(&err);
        return nullptr;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);
    if (!dbus_bus_register(c, &err)) {
        dbus_error_free(&err);
        dbus_connection_close(c);
        dbus_connection_unref(c);
        return nullptr;
    }
    return c;
}

void closePrivate(DBusConnection* c) {
    if (!c) return;
    dbus_connection_close(c);
    dbus_connection_unref(c);
}

/// 客户端侧解析出的一帧回复（字段顺序与 `(huuuuuuttu)` 一一对应）
struct FrameReply {
    int fd{-2};                ///< -2 = 没读到（与合法的 fd 值区分开）
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t uv_offset{0};     ///< UV 平面偏移，事实值（本板 1640x1232 → 2030336）
    uint32_t fourcc{0};
    uint32_t size{0};
    uint64_t sequence{0};
    uint64_t timestamp_ns{0};
    uint32_t slot{0};
    std::string signature;     ///< 实际签名，断言失败时用于给出可读信息
};

/**
 * @brief 在客户端侧按位置解析 GetVideoFrame 回复
 *
 * **不做任何容错**：字段少一个或类型不符就返回 false，并把实际签名留在
 * out.signature。这正是本测试的价值——它读到的就是面板会读到的。
 */
bool parseFrameReply(DBusMessage* r, FrameReply& out) {
    const char* sig = dbus_message_get_signature(r);
    out.signature = sig ? sig : "";

    DBusMessageIter it;
    if (!dbus_message_iter_init(r, &it)) return false;

    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UNIX_FD) return false;
    dbus_message_iter_get_basic(&it, &out.fd);
    if (!dbus_message_iter_next(&it)) return false;

    // 后续 9 个字段：每步都断言类型，类型不符立即失败。
    // 「类型不符却继续往下读」正是签名错位时的表现，必须让它失败。
    struct Step {
        int expected_type;
        void* dst;
    };
    const Step steps[] = {
        {DBUS_TYPE_UINT32, &out.width},
        {DBUS_TYPE_UINT32, &out.height},
        {DBUS_TYPE_UINT32, &out.stride},
        {DBUS_TYPE_UINT32, &out.uv_offset},
        {DBUS_TYPE_UINT32, &out.fourcc},
        {DBUS_TYPE_UINT32, &out.size},
        {DBUS_TYPE_UINT64, &out.sequence},
        {DBUS_TYPE_UINT64, &out.timestamp_ns},
        {DBUS_TYPE_UINT32, &out.slot},
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        if (dbus_message_iter_get_arg_type(&it) != steps[i].expected_type) return false;
        dbus_message_iter_get_basic(&it, steps[i].dst);
        if (i + 1 < sizeof(steps) / sizeof(steps[0])) {
            if (!dbus_message_iter_next(&it)) return false;
        }
    }
    // 不应有多余字段：多出来同样说明与约定不符
    return !dbus_message_iter_next(&it);
}

// ---------------------------------------------------------------------------
// 测试夹具：一条私有总线 + 服务端连接 + 客户端连接 + 真实的 DbusService
// ---------------------------------------------------------------------------

class VideoFrameDbusTest : public ::testing::Test {
protected:
    void SetUp() override {
        bus_ = std::make_unique<PrivateBus>();
        if (!bus_->ok()) {
            GTEST_SKIP() << "无法拉起私有 dbus-daemon: " << bus_->reason();
        }

        server_conn_ = openPrivate(bus_->address());
        ASSERT_NE(server_conn_, nullptr) << "无法打开服务端连接";

        // 与 server.cpp 真实启动路径一致：先占服务名，再注册对象路径。
        DBusError err;
        dbus_error_init(&err);
        const int rc = dbus_bus_request_name(
            server_conn_, weaknet_dbus::kBusName,
            DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
        if (rc < 0) {
            dbus_error_free(&err);
        }
        ASSERT_GE(rc, 0) << "无法注册服务名（rc=" << rc << "）";

        ctx_ = std::make_unique<ServerContext>();
        service_ = std::make_unique<DbusService>(ctx_.get());
        ASSERT_TRUE(service_->register_on_connection(server_conn_));

        // 转两次，让服务名的 NameAcquired 与路径注册在总线上生效
        for (int i = 0; i < 4; ++i) {
            dbus_connection_read_write_dispatch(server_conn_, 20);
        }

        client_conn_ = openPrivate(bus_->address());
        ASSERT_NE(client_conn_, nullptr) << "无法打开客户端连接";
        // 清掉客户端握手消息
        for (int i = 0; i < 4; ++i) {
            dbus_connection_read_write_dispatch(client_conn_, 20);
        }
    }

    void TearDown() override {
        service_.reset();
        ctx_.reset();
        closePrivate(client_conn_);
        closePrivate(server_conn_);
        bus_.reset();
    }

    /// 在服务端连接上派发待处理消息（服务端回调在这里被调用）
    void pumpServer(int rounds = 8) {
        for (int i = 0; i < rounds; ++i) {
            dbus_connection_read_write_dispatch(server_conn_, 20);
        }
    }

    /// 客户端发一次 GetVideoFrame 并解析回复。返回 false 表示调用失败。
    bool callGetVideoFrame(FrameReply& out, std::string* error_name = nullptr) {
        DBusMessage* msg = dbus_message_new_method_call(
            weaknet_dbus::kBusName, weaknet_dbus::kObjectPath,
            weaknet_dbus::kInterface, weaknet_dbus::kMethodGetVideoFrame);
        EXPECT_NE(msg, nullptr);
        if (!msg) return false;

        dbus_connection_send(client_conn_, msg, nullptr);
        dbus_message_unref(msg);
        dbus_connection_flush(client_conn_);

        // 服务端派发请求 → 生成回复；客户端派发回复
        pumpServer();

        DBusMessage* reply = nullptr;
        for (int i = 0; i < 60; ++i) {
            dbus_connection_read_write_dispatch(client_conn_, 20);
            reply = dbus_connection_pop_message(client_conn_);
            if (reply) break;
        }
        if (!reply) return false;

        if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
            if (error_name) {
                const char* n = dbus_message_get_error_name(reply);
                *error_name = n ? n : "";
            }
            dbus_message_unref(reply);
            return false;
        }
        const bool ok = parseFrameReply(reply, out);
        dbus_message_unref(reply);
        return ok;
    }

    /// 客户端发一次 ReleaseVideoFrame(slot)
    bool callReleaseVideoFrame(uint32_t slot, bool* released_out = nullptr) {
        DBusMessage* msg = dbus_message_new_method_call(
            weaknet_dbus::kBusName, weaknet_dbus::kObjectPath,
            weaknet_dbus::kInterface, weaknet_dbus::kMethodReleaseVideoFrame);
        EXPECT_NE(msg, nullptr);
        if (!msg) return false;
        dbus_uint32_t s = slot;
        dbus_message_append_args(msg, DBUS_TYPE_UINT32, &s, DBUS_TYPE_INVALID);
        dbus_connection_send(client_conn_, msg, nullptr);
        dbus_message_unref(msg);
        dbus_connection_flush(client_conn_);

        pumpServer();

        DBusMessage* reply = nullptr;
        for (int i = 0; i < 60; ++i) {
            dbus_connection_read_write_dispatch(client_conn_, 20);
            reply = dbus_connection_pop_message(client_conn_);
            if (reply) break;
        }
        if (!reply) return false;
        if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
            dbus_message_unref(reply);
            return false;
        }
        if (released_out) {
            dbus_bool_t b = FALSE;
            DBusMessageIter it;
            if (dbus_message_iter_init(reply, &it) &&
                dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_BOOLEAN) {
                dbus_message_iter_get_basic(&it, &b);
            }
            *released_out = (b == TRUE);
        }
        dbus_message_unref(reply);
        return true;
    }

    /// 挂一个已 configure 且已 publish 的中枢，返回发布时用的源缓冲
    std::vector<uint8_t> attachHubWithOneFrameAndPublish(
        uint32_t w = 64, uint32_t h = 32, uint32_t stride = 64,
        uint32_t uv_offset = 0) {
        // 槽位容量必须真的装得下 UV 平面：带对齐空洞时 offset 会大于
        // stride*height，若仍按 stride*h*3/2 分配，UV 平面尾部就在槽位外，
        // 中枢会记 truncated——那会掩盖本用例真正要验的东西。
        const size_t uv_off = (uv_offset == 0)
                                  ? static_cast<size_t>(stride) * h
                                  : uv_offset;
        const size_t size = uv_off + static_cast<size_t>(stride) * (h / 2);
        ctx_->video_hub = std::make_unique<weaknet::VideoFrameHub>();
        EXPECT_TRUE(ctx_->video_hub->configure(w, h, stride, 0x3231564e /*NV12*/, size,
                                               uv_offset));

        std::vector<uint8_t> src(size, 0x5A);
        // sequence / timestamp 用可辨识的值，确保客户端读到的就是这一帧
        EXPECT_TRUE(ctx_->video_hub->publish(src.data(), src.size(), 4242, 987654321));
        return src;
    }

    std::unique_ptr<PrivateBus> bus_;
    DBusConnection* server_conn_ = nullptr;
    DBusConnection* client_conn_ = nullptr;
    std::unique_ptr<ServerContext> ctx_;
    std::unique_ptr<DbusService> service_;
};

// ---------------------------------------------------------------------------
// 契约 1：没有帧时也必须返回 10 字段、arg0 为 UNIX_FD 的回复
//
// 这是本套件存在的原因。若代码退回 `fd = -1`，parseFrameReply 会因 arg0
// 不是 UNIX_FD 而失败，signature 会打印成 `uuuuuuttu`。
// ---------------------------------------------------------------------------

TEST_F(VideoFrameDbusTest, NoFrameStillReturnsUnixFdWithFullSignature) {
    // 挂一个「已 configure 但从未 publish」的中枢：acquire() 返回 false，
    // 这是生产中最常见的无帧情形（摄像头刚启动、还没出第一帧）。
    // 若 hub 为 nullptr 走的是 error reply 分支，测不到本契约。
    ctx_->video_hub = std::make_unique<weaknet::VideoFrameHub>();
    ASSERT_TRUE(ctx_->video_hub->configure(64, 32, 64, 0x3231564e, 64 * 32 * 3 / 2));

    FrameReply r;
    std::string err;
    const bool ok = callGetVideoFrame(r, &err);

    ASSERT_TRUE(ok) << "解析失败。实际签名 = '" << r.signature
                    << "'；若为 'uuuuuttu' 说明无帧分支又用上了 fd=-1"
                    << "（libdbus 会拒绝该值并静默丢掉字段）。err=" << err;
    EXPECT_EQ(r.signature, "huuuuuuttu");
    EXPECT_GE(r.fd, 0) << "无帧时必须给一个有效（但为空）的 fd，不能给 -1";
    EXPECT_EQ(r.size, 0u) << "无帧判据：size 必须为 0";
    EXPECT_EQ(r.slot, UINT32_MAX) << "无帧判据：slot 必须为 UINT32_MAX";
    EXPECT_EQ(r.width, 0u);
    EXPECT_EQ(r.height, 0u);

    // 无帧回复里的 fd 是空 memfd：可 mmap 但长度为 0，调用方不应读它。
    // 这里只验证它是个可用句柄。
    //
    // 注意 F_GETFD 的返回值：成功时返回**文件描述符标志位**（不是 0）。本例
    // 拿到了 FD_CLOEXEC(=1)，因为中枢与 memfd 都用了 CLOEXEC。断言 >= 0 才
    // 正确；断言 == 0 是我第一版写错的判据（真机上 memfd 带 CLOEXEC 会假失败）。
    EXPECT_GE(::fcntl(r.fd, F_GETFD), 0)
        << "无帧 fd 必须是可用的句柄: " << std::strerror(errno);
    ::close(r.fd);
}

// ---------------------------------------------------------------------------
// 契约 2：有帧时返回真实几何 + sequence，且 fd 能 mmap 出我们写进去的像素
// ---------------------------------------------------------------------------

TEST_F(VideoFrameDbusTest, FrameReturnsGeometrySequenceAndMappableFd) {
    const uint32_t w = 64, h = 32, stride = 64;
    auto src = attachHubWithOneFrameAndPublish(w, h, stride);
    const size_t expected_size = src.size();

    FrameReply r;
    std::string err;
    ASSERT_TRUE(callGetVideoFrame(r, &err))
        << "有帧时必须成功。signature='" << r.signature << "' err=" << err;

    EXPECT_EQ(r.signature, "huuuuuuttu");
    EXPECT_GE(r.fd, 0);
    EXPECT_EQ(r.width, w);
    EXPECT_EQ(r.height, h);
    EXPECT_EQ(r.stride, stride) << "stride 必须原样透传，不能由 width 推导";
    EXPECT_EQ(r.uv_offset, 0u)
        << "uv_offset 是事实值；0 表示紧凑布局（stride*height），默认情形应原样透传";
    EXPECT_EQ(r.fourcc, 0x3231564eu);
    EXPECT_EQ(r.size, expected_size);
    EXPECT_EQ(r.sequence, 4242u);
    EXPECT_EQ(r.timestamp_ns, 987654321u);
    EXPECT_NE(r.slot, UINT32_MAX);

    void* p = ::mmap(nullptr, r.size, PROT_READ, MAP_SHARED, r.fd, 0);
    ASSERT_NE(p, MAP_FAILED) << "对端必须能 mmap 这个 fd: " << std::strerror(errno);
    const auto* bytes = static_cast<const uint8_t*>(p);
    // 首字节与末字节都校验：只查首字节会漏掉「长度给错」的缺陷
    EXPECT_EQ(bytes[0], 0x5A);
    EXPECT_EQ(bytes[r.size - 1], 0x5A);
    ::munmap(p, r.size);
    ::close(r.fd);
}

// ---------------------------------------------------------------------------
// 契约 2.5：uv_offset 必须跨进程原样到达
//
// 这是整条链路上最容易静默出错的一环：即使服务端算对了、中枢存对了，
// 只要 D-Bus 这一跳丢了它（字段顺序错位、或对端按 stride*height 重新推导），
// 面板就会把驱动的对齐空洞当色度读——U=V=0，画面严重偏色，而**不报任何错**。
//
// 用非零且有辨识度的偏移，确保它不可能被「默认为 0」蒙对。
// ---------------------------------------------------------------------------

TEST_F(VideoFrameDbusTest, UvOffsetCrossesProcessBoundary) {
    const uint32_t w = 64, h = 32, stride = 64;
    const uint32_t hole = 256;                   // 模拟驱动的对齐空洞
    const uint32_t uv_off = stride * h + hole;   // 严格大于 stride*height

    auto src = attachHubWithOneFrameAndPublish(w, h, stride, uv_off);
    ASSERT_GT(uv_off, stride * h) << "本用例要求偏移严格大于 stride*height";

    FrameReply r;
    std::string err;
    ASSERT_TRUE(callGetVideoFrame(r, &err))
        << "signature='" << r.signature << "' err=" << err;

    EXPECT_EQ(r.uv_offset, uv_off)
        << "uv_offset 必须原样跨过 D-Bus；丢了它面板会把对齐空洞当色度读。"
        << " 实际签名='" << r.signature << "'";

    ::close(r.fd);
}

// ---------------------------------------------------------------------------
// 契约 3：ReleaseVideoFrame 归还后中枢真的可以继续发布
//
// 为什么这条重要：中枢的语义是「槽位没归还就不覆写」。如果 Release 只是
// 回了个 true 却没真正转达给中枢，中枢会在 3 帧后永久停摆，而 D-Bus 上
// 一切正常——这正是最难查的那类故障。
// ---------------------------------------------------------------------------

TEST_F(VideoFrameDbusTest, ReleaseActuallyFreesSlotInHub) {
    const uint32_t w = 64, h = 32, stride = 64;
    auto src = attachHubWithOneFrameAndPublish(w, h, stride);

    FrameReply r;
    ASSERT_TRUE(callGetVideoFrame(r)) << "signature='" << r.signature << "'";
    ASSERT_NE(r.slot, UINT32_MAX);
    ::close(r.fd);

    const uint64_t released_before = ctx_->video_hub->stats().released;

    bool released = false;
    ASSERT_TRUE(callReleaseVideoFrame(r.slot, &released));
    EXPECT_TRUE(released) << "归还自己持有的 slot 必须返回 true";
    EXPECT_EQ(ctx_->video_hub->stats().released, released_before + 1)
        << "D-Bus 的 ReleaseVideoFrame 必须真的转达给中枢，不能只回 true";
}

// ---------------------------------------------------------------------------
// 契约 4：归还他人 / 未知 slot 被拒绝，且不影响中枢计数
//
// 这不是苛求——slot 号是调用方自己报的数，若原样透传给中枢，任何本地用户
// 都能释放别人正在读的槽，中枢随即覆写，对方读到撕裂画面。
// ---------------------------------------------------------------------------

TEST_F(VideoFrameDbusTest, ReleaseForeignSlotIsRejectedAndHarmless) {
    const uint32_t w = 64, h = 32, stride = 64;
    attachHubWithOneFrameAndPublish(w, h, stride);

    FrameReply r;
    ASSERT_TRUE(callGetVideoFrame(r)) << "signature='" << r.signature << "'";
    ::close(r.fd);

    const uint64_t released_before = ctx_->video_hub->stats().released;

    bool released = true;
    ASSERT_TRUE(callReleaseVideoFrame(r.slot + 7, &released));
    EXPECT_FALSE(released) << "报错 slot 必须被拒绝，而不是释放他人的槽";
    EXPECT_EQ(ctx_->video_hub->stats().released, released_before)
        << "被拒绝的归还不能真的调用中枢 release()";

    // 正确归还原来的 slot 仍然有效（说明拒绝没有破坏记账）
    bool ok_release = false;
    ASSERT_TRUE(callReleaseVideoFrame(r.slot, &ok_release));
    EXPECT_TRUE(ok_release);
}

// ---------------------------------------------------------------------------
// 契约 5：同一 sender 再次取帧会自动归还上一次，不会永久占槽
//
// 这是「健忘的消费者」防护：面板即使从不 Release，每次 GetVideoFrame 也会
// 自动归还原槽，于是它在任意时刻最多占 1 个槽，中枢不会停摆。
// ---------------------------------------------------------------------------

TEST_F(VideoFrameDbusTest, RepeatedGetAutoReleasesPreviousLease) {
    const uint32_t w = 64, h = 32, stride = 64;
    auto src = attachHubWithOneFrameAndPublish(w, h, stride);

    FrameReply first;
    ASSERT_TRUE(callGetVideoFrame(first)) << "signature='" << first.signature << "'";
    ::close(first.fd);

    const uint64_t released_after_first = ctx_->video_hub->stats().released;

    // 发布新帧（中枢会因限速跳过；这里直接再取一次即可）
    FrameReply second;
    ASSERT_TRUE(callGetVideoFrame(second)) << "signature='" << second.signature << "'";
    ::close(second.fd);

    EXPECT_GT(ctx_->video_hub->stats().released, released_after_first)
        << "同一 sender 第二次取帧必须自动归还上一次的槽";
}

}  // namespace
