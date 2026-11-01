/**
 * @file test_looper_dispatch_gtest.cpp
 * @brief Looper D-Bus 派发层行为的回归测试
 *
 * 背景（2026-11-01 修复的缺陷）：
 *   looper.cpp 原先在每次 poll 唤醒后只调用一次
 *   dbus_connection_read_write_dispatch(conn_, 0)，注释声称
 *   “若仍有数据到达，poll 会立即再次唤醒”。这个假设有两半都不成立：
 *
 *   1. 单次 read_write_dispatch 只派发**一条**消息（Test 2 断言），
 *      队列积压时要排队等后续轮次；
 *   2. 内部队列有积压但 socket 已读空时，poll **不再**报告 POLLIN
 *      （Test 3 断言），积压只能等下一条 socket 数据才被处理。
 *
 *   现场症状：GetInterfaces 延迟呈 13ms / 300ms / 1000ms / 3000ms 的离散阶梯，
 *   最坏 25s；而服务端日志时间戳证明延迟全部产生在 handleXxx 被调用之前
 *   （派发层排队），处理逻辑本身耗时近零。
 *
 * 修复（server/src/looper.cpp 的 dispatch_until_drained）：
 *   每轮事件循环派发到 DBUS_DISPATCH_COMPLETE，上限 64 轮防止饿死
 *   self-pipe 的停止信号；poll 超时但队列仍有积压时也主动排空。
 *
 * 测试隔离性：
 *   本套件不依赖环境里已有的会话总线（CI 与开发板上都没有），而是自己拉起
 *   一个 `dbus-daemon --session --nofork` 子进程，用完即杀。若连 dbus-daemon
 *   都不可用，测试显式 SKIP 并打印原因，绝不伪通过。
 */

#include <gtest/gtest.h>

#include "looper.hpp"

#include <dbus/dbus.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using weaknet_dbus::kMaxDispatchRounds;

/**
 * @brief 一次性拉起私有 dbus-daemon，提供不依赖环境的测试总线
 *
 * 为什么不用 DBUS_BUS_SESSION？
 *   CI（GitHub Actions）与开发板（systemd 服务）都没有会话总线，
 *   dbus_bus_get_private(DBUS_BUS_SESSION) 会失败，测试只能 SKIP——
 *   一条永远被跳过的回归测试等于没有回归测试。
 */
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
            // 子进程：把 stdout 指向管道写端，--print-address=1 把地址打到 stdout
            dup2(out[1], STDOUT_FILENO);
            ::close(out[0]);
            ::close(out[1]);
            signal(SIGPIPE, SIG_IGN);  // 父进程提前关管道时不要被杀死
            execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork",
                   "--print-address=1", static_cast<char*>(nullptr));
            _exit(127);  // execlp 失败
        }

        ::close(out[1]);
        // 读一行地址（最多等 5 秒）
        char buf[512];
        std::string line;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
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
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            // 最多等 2 秒，超时用 SIGKILL 兜底，避免僵尸进程拖住测试
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
    }

    pid_t pid_ = -1;
    std::string address_;
    std::string reason_;
};

/// 打开一条私有连接并完成注册
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

/**
 * @brief 统计收到的业务信号条数
 *
 * 只计本测试自己发的 `/com/example/WeakNet/Test` 接口信号。为什么不能数所有
 * 信号？连接注册时总线会下发 `org.freedesktop.DBus.NameAcquired`，
 * 若一并计入，基线会多出 1 条，让“8 条积压”变成“收到 9 条”的假失败。
 */
DBusHandlerResult countSignals(DBusConnection* conn, DBusMessage* msg, void* data) {
    (void)conn;
    int* count = static_cast<int*>(data);
    if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL) {
        const char* iface = dbus_message_get_interface(msg);
        if (iface && std::strcmp(iface, "com.example.WeakNet.Test") == 0) {
            ++(*count);
        }
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

/**
 * @brief 把 socket 上的字节全部读进 libdbus 内部队列，但**不派发**
 *
 * 为什么需要它？
 *   dbus_connection_read_write 只是"读"，读到的消息进入 libdbus 内部队列后
 *   才等 dispatch 派发。但一次 read_write 未必把 socket 读空——剩下的仍在
 *   内核 socket 缓冲区里。若不读空，队列里可能只有 1-2 条消息，
 *   "构造 N 条积压"的前提不成立（实测：发 68 条后立刻检查，队列里远不足 68 条，
 *   排空循环只跑了 27 轮就结束）。
 *
 * @return true 表示 socket 已空（数据全部进入内部队列）
 */
bool readSocketIntoQueue(DBusConnection* conn) {
    int fd = -1;
    if (!dbus_connection_get_socket(conn, &fd) || fd < 0) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        const int r = poll(&p, 1, 20);
        if (r == 0) return true;   // socket 已空：所有字节都在内部队列里
        if (r < 0) return errno == EINTR;
        dbus_connection_read_write(conn, 0);
    }
    return false;
}

/**
 * @brief 向 connA 定向发送 n 条信号，并全部读进内部队列，制造派发积压
 *
 * 用单播（destination = connA 的唯一名）而不是广播 + match 规则，
 * 这样不需要额外注册 match，行为完全确定。
 *
 * @return 构造成功返回 true；false 表示前提不成立（测试应 SKIP 而非伪通过）
 */
bool buildBacklog(DBusConnection* connA, DBusConnection* connB, int n) {
    const char* dest = dbus_bus_get_unique_name(connA);
    if (!dest) return false;

    for (int i = 0; i < n; ++i) {
        DBusMessage* msg = dbus_message_new_signal("/com/example/WeakNet/Test",
                                                   "com.example.WeakNet.Test", "Ping");
        if (!msg) return false;
        dbus_uint32_t seq = static_cast<dbus_uint32_t>(i);
        dbus_message_append_args(msg, DBUS_TYPE_UINT32, &seq, DBUS_TYPE_INVALID);
        dbus_message_set_destination(msg, dest);
        dbus_connection_send(connB, msg, nullptr);
        dbus_message_unref(msg);
    }
    dbus_connection_flush(connB);

    // 只读不派发：数据进入内部队列，服务端“看得到但还没处理”
    if (!readSocketIntoQueue(connA)) return false;
    return dbus_connection_get_dispatch_status(connA) == DBUS_DISPATCH_DATA_REMAINS;
}


/**
 * @brief 测试夹具：持有私有总线与两条连接，统计收到的信号
 *
 * 每个 TEST_F 自己拉起一套总线，互不干扰。
 */
class LooperDispatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        bus_ = std::make_unique<PrivateBus>();
        if (!bus_->ok()) {
            GTEST_SKIP() << "无法拉起私有 dbus-daemon: " << bus_->reason();
        }
        connA_ = openPrivate(bus_->address());
        connB_ = openPrivate(bus_->address());
        ASSERT_NE(connA_, nullptr) << "无法打开测试连接 A";
        ASSERT_NE(connB_, nullptr) << "无法打开测试连接 B";
        dbus_connection_add_filter(connA_, countSignals, &received_, nullptr);

        // 清空连接注册期产生的握手消息（org.freedesktop.DBus.NameAcquired 等）。
        // 不清空的话，队列头部会压着这些系统信号，让“单次派发只处理一条”的
        // 断言读到 0 条业务消息（实测暴露过这个假失败）。
        for (int i = 0; i < kMaxDispatchRounds; ++i) {
            if (!dbus_connection_read_write_dispatch(connA_, 0)) break;
            if (dbus_connection_get_dispatch_status(connA_) != DBUS_DISPATCH_DATA_REMAINS) break;
        }
        received_ = 0;  // 系统信号不计入业务计数
    }

    void TearDown() override {
        closePrivate(connA_);
        closePrivate(connB_);
        bus_.reset();
    }

    std::unique_ptr<PrivateBus> bus_;
    DBusConnection* connA_ = nullptr;
    DBusConnection* connB_ = nullptr;
    int received_ = 0;
};

/**
 * @brief 契约 1：一次“派发直到清空”必须处理完所有已到达消息
 *
 * 断言：排空循环退出时队列为空，且实际收到的信号数等于构造的积压条数。
 *      如果代码退回“只派发一次”，processed 会明显小于 n，本测试失败。
 */
TEST_F(LooperDispatchTest, DispatchUntilCompleteDrainsWholeQueue) {
    constexpr int kN = 8;
    ASSERT_TRUE(buildBacklog(connA_, connB_, kN)) << "未能构造派发积压，测试前提不成立";

    ASSERT_TRUE(weaknet_dbus::dispatchUntilDrained(connA_, kMaxDispatchRounds))
        << "排空过程中连接意外断开";
    EXPECT_EQ(dbus_connection_get_dispatch_status(connA_), DBUS_DISPATCH_COMPLETE)
        << "排空后队列仍有残留";
    EXPECT_EQ(received_, kN)
        << "只处理了 " << received_ << "/" << kN << " 条消息（单次派发的回归）";
}

/**
 * @brief 契约 2：单次 read_write_dispatch 只派发一条消息
 *
 * 这条断言是修复的核心依据。若未来 libdbus 改变行为（一次派发多条），
 * 本测试会失败并提醒重新评估“为什么要循环”。用 2 条而不是更多，
 * 是为了让断言在“一次派发 N 条”的实现上也能明确暴露。
 */
TEST_F(LooperDispatchTest, SingleDispatchHandlesExactlyOneMessage) {
    constexpr int kN = 4;
    ASSERT_TRUE(buildBacklog(connA_, connB_, kN)) << "未能构造派发积压，测试前提不成立";

    const int before = received_;
    dbus_connection_read_write_dispatch(connA_, 0);
    const int after = received_;
    const DBusDispatchStatus status = dbus_connection_get_dispatch_status(connA_);

    EXPECT_EQ(after - before, 1)
        << "单次 read_write_dispatch 处理了 " << (after - before) << " 条消息";
    EXPECT_EQ(status, DBUS_DISPATCH_DATA_REMAINS)
        << "还有积压时状态应为 DATA_REMAINS（证明单次派发未清空）";
}

/**
 * @brief 契约 3：内部队列有积压时，poll 不再报告 socket 可读
 *
 * 这是“只处理 POLLIN 分支”的致命缺陷证据：积压期间 poll 会一路超时，
 * 主线程空转等待下一条 socket 数据。修复必须在 poll 超时分支里也检查
 * dispatch_status 并主动排空。
 */
TEST_F(LooperDispatchTest, PollDoesNotWakeForInternalBacklog) {
    constexpr int kN = 8;
    ASSERT_TRUE(buildBacklog(connA_, connB_, kN)) << "未能构造派发积压，测试前提不成立";

    int fd = -1;
    ASSERT_TRUE(dbus_connection_get_socket(connA_, &fd));
    ASSERT_GE(fd, 0);

    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    const int ret = poll(&p, 1, 300);

    EXPECT_EQ(ret, 0)
        << "poll 因内部积压被唤醒（revents=0x" << std::hex << p.revents
        << "）——若 libdbus 行为改变，looper 的超时分支排空可简化";
    // socket 无数据，但内部队列仍未清空：这正是“只等 POLLIN”会卡住的状态
    EXPECT_EQ(dbus_connection_get_dispatch_status(connA_), DBUS_DISPATCH_DATA_REMAINS);
}

/**
 * @brief 契约 4：轮次上限必须存在且是有限值
 *
 * 无界 while 会在持续有内部消息时饿死 self-pipe 的停止信号，导致
 * SIGTERM/SIGINT 失效（systemd 停服只能等 TimeoutStopSec 后 SIGKILL）。
 * 本测试把常量语义固定为可执行文档。
 */
TEST_F(LooperDispatchTest, DispatchRoundLimitIsBoundedAndFinite) {
    static_assert(kMaxDispatchRounds > 0, "上限必须为正");
    static_assert(kMaxDispatchRounds <= 1024, "上限过大将导致停止信号饥饿");

    // 构造超过上限的积压（上限 + 4），验证循环能被上限截断而不是无限收敛。
    //
    // 关键：断言"恰好处理了 max_rounds 条"，而不是只断言"还有积压"。
    // 只断言"还有积压"是假通过——单次派发（旧缺陷）同样会留下积压，
    // 于是该测试在旧代码上也会绿。按条数断言后：
    //   - 旧行为（只派发一次）→ 只处理 1 条 ≠ 64 → 失败
    //   - 无界循环（忽略上限）→ 处理 68 条 ≠ 64 → 失败
    //   - 正确实现 → 恰好 64 条 → 通过
    const int kOver = kMaxDispatchRounds + 4;
    ASSERT_TRUE(buildBacklog(connA_, connB_, kOver)) << "未能构造超上限积压";

    int processed_before = received_;
    ASSERT_TRUE(weaknet_dbus::dispatchUntilDrained(connA_, kMaxDispatchRounds))
        << "排空过程中连接意外断开";
    const int processed = received_ - processed_before;

    EXPECT_EQ(processed, kMaxDispatchRounds)
        << "一轮最多处理 " << kMaxDispatchRounds << " 条，实际处理 " << processed
        << " 条（少于上限=没排空；多于上限=上限未生效）";
    EXPECT_EQ(dbus_connection_get_dispatch_status(connA_), DBUS_DISPATCH_DATA_REMAINS)
        << "超过上限时应有剩余积压";
}

}  // namespace
