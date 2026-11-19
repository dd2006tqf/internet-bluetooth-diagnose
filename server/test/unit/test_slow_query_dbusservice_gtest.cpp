/**
 * @file test_slow_query_dbusservice_gtest.cpp
 * @brief 慢查询卸载到工作线程的回归测试
 *
 * ============================================================================
 * 本套件钉住的是什么
 * ============================================================================
 *
 * `dbus_service.cpp` 的四个只读查询（GetHistory / QueryDeviceEvents /
 * QueryBtDataPath / QuerySiteIncidents）原先直接跑在 D-Bus 派发线程上。
 * 派发是**单线程串行**的：一条消息的回调不返回，整个服务就不响应任何其它
 * 方法调用。x86 满库（10000 行）实测 queryHistory p50 = 118ms、
 * p95 = 130ms，返回 8.5MB JSON；开发板 A55 单核约为该值的 3~5 倍。
 *
 * 所以缺陷不是"这条查询慢"，而是**连带**：同一窗口内的 HealthCheck、
 * Ping、GetNetworkExperience 全部排队。本套件的判据因此不是"查询要快"，
 * 而是**"查询在跑的时候，别的 D-Bus 方法仍然能被答复"**。
 *
 * ============================================================================
 * 手段：可阻塞的受控 job，而不是靠真实慢查询卡时间
 * ============================================================================
 *
 * 用真查询计时会把测试变成性能测试（CI 机器快慢不同就飘）。这里改为直连
 * 队列契约 `enqueueSlowQuery(conn, msg, run)`——把 run 换成一个由测试
 * **决定何时返回**的 lambda，于是"查询在途"这个状态可精确复现。
 *
 * 走真实 dbus-daemon（与 test_video_frame_dbus_gtest 同一手法）：回复的
 * 构造与发送发生在主线程，只有真的序列化过总线才能验证"结果送达且内容
 * 未损坏"。
 */

#include <gtest/gtest.h>

#include <dbus/dbus.h>

#include "active_connectivity_monitor.hpp"
#include "common.hpp"
#include "database_manager.hpp"
#include "dbus_service.hpp"
#include "looper.hpp"
#include "metrics/metrics_registry.hpp"
#include "monitor_plugin.hpp"
#include "net_info.hpp"
#include "network_quality_result.hpp"
#include "server.hpp"
#include "weak_netmgr.hpp"
#include "wireless_event_store.hpp"

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using weaknet_dbus::DbusService;
using weaknet_dbus::LinkQuality;
using weaknet_dbus::Looper;
using weaknet_dbus::NetInfo;
using weaknet_dbus::NetworkQualityLevel;
using weaknet_dbus::NetworkQualityResult;
using weaknet_dbus::ServerContext;

// ---------------------------------------------------------------------------
// 私有总线（与 test_video_frame_dbus_gtest / test_looper_dispatch_gtest 同构）
//
// 不抽公共头是刻意的：三个套件的断言对象不同（派发层 / 线协议 / 队列契约），
// 公共化只会让改动互相牵连。
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

// ===========================================================================
// 部分 A：队列契约（不需要总线）
//
// 直接调 enqueueSlowQuery 并传 conn=nullptr：replyError 对 nullptr 静默，
// 于是整个用例不产生任何总线流量，可以纯粹断言"受理 / 拒绝 / 计数"。
// ===========================================================================

class SlowQueryQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_unique<ServerContext>();
        service_ = std::make_unique<DbusService>(ctx_.get());
    }

    void TearDown() override {
        // 先放行阻塞中的 job，否则 worker 在 ~DbusService 的 join 上永远等下去
        release_.store(true);
        service_.reset();
        ctx_.reset();
    }

    /// 造一条"像一个真实方法调用"的消息（不发出去，只用于 reply 构造）
    static DBusMessage* makeCall(const char* method) {
        return dbus_message_new_method_call(
            weaknet_dbus::kBusName, weaknet_dbus::kObjectPath,
            weaknet_dbus::kInterface, method);
    }

    /// 一个会阻塞到 release_ 被置位的 job：让"在途"状态可精确复现。
    std::function<std::string()> blockingJob() {
        return [this]() {
            while (!release_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return std::string("[]");
        };
    }

    std::unique_ptr<ServerContext> ctx_;
    std::unique_ptr<DbusService> service_;
    std::atomic<bool> release_{false};
};

// 队列达到上限后，后续查询必须**立刻**被拒，而不是继续堆着。
//
// 这条断言的意义：现场终端等 10 秒才失败，比立刻知道"服务端忙"更糟；
// 而排队中的每条查询都握着一份可能 8.5MB 的结果，无限排队就是内存放大器。
TEST_F(SlowQueryQueueTest, RejectsBeyondLimitInsteadOfQueueingForever) {
    // 第一条占住 worker，使后续条目滞留在队列里
    DBusMessage* first = makeCall(weaknet_dbus::kMethodGetHistory);
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(service_->enqueueSlowQuery(nullptr, first, blockingJob()));
    dbus_message_unref(first);

    // 塞满剩余额度（上限按"未答复总数"计，含在途那条）
    std::vector<DBusMessage*> queued;
    for (size_t i = 1; i < DbusService::kMaxSlowQueryQueueForTest; ++i) {
        DBusMessage* m = makeCall(weaknet_dbus::kMethodGetHistory);
        ASSERT_NE(m, nullptr);
        EXPECT_TRUE(service_->enqueueSlowQuery(nullptr, m, []() {
            return std::string("[]");
        })) << "第 " << i << " 条应当被受理";
        queued.push_back(m);
    }
    EXPECT_EQ(service_->slowQueryQueueDepthForTest(),
              DbusService::kMaxSlowQueryQueueForTest);

    // 超额这一条：返回 true（调用已被处理）但计数不增加
    DBusMessage* overflow = makeCall(weaknet_dbus::kMethodGetHistory);
    ASSERT_NE(overflow, nullptr);
    EXPECT_TRUE(service_->enqueueSlowQuery(nullptr, overflow, []() {
        return std::string("[]");
    }));
    dbus_message_unref(overflow);
    EXPECT_EQ(service_->slowQueryQueueDepthForTest(),
              DbusService::kMaxSlowQueryQueueForTest)
        << "被拒的查询不得计入未答复额度";

    for (DBusMessage* m : queued) dbus_message_unref(m);
}

// 空 job 必须被拒，且**不占用额度**：一个不可执行的 job 在任何语义下
// 都不该被算作"用户等待中的查询"。
TEST_F(SlowQueryQueueTest, RejectsEmptyJob) {
    DBusMessage* m = makeCall(weaknet_dbus::kMethodGetHistory);
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(service_->enqueueSlowQuery(nullptr, m, nullptr));
    dbus_message_unref(m);
    EXPECT_EQ(service_->slowQueryQueueDepthForTest(), 0u);
}

// 关停后必须拒绝新查询：此刻主线程不再派发，任何受理都是无法送达的承诺。
TEST_F(SlowQueryQueueTest, RejectsAfterStop) {
    release_.store(true);
    service_->stopSlowQueryWorker();

    DBusMessage* m = makeCall(weaknet_dbus::kMethodGetHistory);
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(service_->enqueueSlowQuery(nullptr, m, []() {
        return std::string("[]");
    }));
    dbus_message_unref(m);
    EXPECT_EQ(service_->slowQueryQueueDepthForTest(), 0u);
}

// ===========================================================================
// 部分 B：真总线上的端到端语义
//
// 这一部分验证的正是缺陷本身：**一条慢查询在途时，别的方法能否被答复**。
// 只有真总线 + 真 Looper 才能表达这个时序关系。
// ===========================================================================

class SlowQueryDbusTest : public ::testing::Test {
protected:
    void SetUp() override {
        bus_ = std::make_unique<PrivateBus>();
        if (!bus_->ok()) {
            GTEST_SKIP() << "无法拉起私有 dbus-daemon: " << bus_->reason();
        }

        server_conn_ = openPrivate(bus_->address());
        ASSERT_NE(server_conn_, nullptr) << "无法打开服务端连接";

        DBusError err;
        dbus_error_init(&err);
        const int rc = dbus_bus_request_name(
            server_conn_, weaknet_dbus::kBusName,
            DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
        if (rc < 0) dbus_error_free(&err);
        ASSERT_GE(rc, 0) << "无法注册服务名（rc=" << rc << "）";

        ctx_ = std::make_unique<ServerContext>();
        // 最小可用的 weak_mgr：ListInterfaces 会读它
        ctx_->weak_mgr = std::make_unique<weaknet_dbus::WeakNetMgr>();
        ctx_->weak_mgr->updateInterfaces({});

        service_ = std::make_unique<DbusService>(ctx_.get());
        ASSERT_TRUE(service_->register_on_connection(server_conn_));

        for (int i = 0; i < 4; ++i) {
            dbus_connection_read_write_dispatch(server_conn_, 20);
        }

        client_conn_ = openPrivate(bus_->address());
        ASSERT_NE(client_conn_, nullptr) << "无法打开客户端连接";
        for (int i = 0; i < 4; ++i) {
            dbus_connection_read_write_dispatch(client_conn_, 20);
        }
    }

    void TearDown() override {
        release_.store(true);
        // 先停 worker 再拆连接：worker 可能正要往 server_conn_ 回复
        if (service_) service_->stopSlowQueryWorker();
        service_.reset();
        ctx_.reset();
        closePrivate(client_conn_);
        closePrivate(server_conn_);
        bus_.reset();
    }

    /// 服务端派发一轮（含 drainWakeups 触发的回复发送）
    void pumpServer() {
        dbus_connection_read_write_dispatch(server_conn_, 20);
        if (auto* l = Looper::current()) l->drainWakeups();
    }

    /// 客户端发一条 ListInterfaces 并等回复，返回耗时（毫秒）；失败返回 -1。
    ///
    /// ListInterfaces 是理想的探针：它只读 weak_mgr 的接口列表，与数据库
    /// 完全无关，因此测到的延迟纯粹来自"派发线程是否被别的东西占着"。
    long long callListInterfacesLatencyMs(int budget_ms = 400) {
        DBusMessage* msg = dbus_message_new_method_call(
            weaknet_dbus::kBusName, weaknet_dbus::kObjectPath,
            weaknet_dbus::kInterface, weaknet_dbus::kMethodListInterfaces);
        if (!msg) return -1;
        const auto t0 = std::chrono::steady_clock::now();

        dbus_connection_send(client_conn_, msg, nullptr);
        dbus_message_unref(msg);
        dbus_connection_flush(client_conn_);

        DBusMessage* reply = nullptr;
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count() < budget_ms) {
            pumpServer();
            dbus_connection_read_write_dispatch(client_conn_, 5);
            reply = dbus_connection_pop_message(client_conn_);
            if (reply) break;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (reply) dbus_message_unref(reply);
        return reply ? elapsed : -1;
    }

    /// 发一条 GetHistory 并把回复（或错误名）取回
    struct HistoryReply {
        bool got{false};
        bool is_error{false};
        std::string error_name;
        std::string payload;
        std::string signature;
    };

    HistoryReply callGetHistory(int32_t limit, int budget_ms = 600) {
        HistoryReply out;
        DBusMessage* msg = dbus_message_new_method_call(
            weaknet_dbus::kBusName, weaknet_dbus::kObjectPath,
            weaknet_dbus::kInterface, weaknet_dbus::kMethodGetHistory);
        if (!msg) return out;
        const char* iface = "";
        const char* start = "";
        const char* end = "";
        dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
                                 DBUS_TYPE_STRING, &start,
                                 DBUS_TYPE_STRING, &end,
                                 DBUS_TYPE_INT32, &limit, DBUS_TYPE_INVALID);
        dbus_connection_send(client_conn_, msg, nullptr);
        dbus_message_unref(msg);
        dbus_connection_flush(client_conn_);

        DBusMessage* reply = nullptr;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count() < budget_ms) {
            pumpServer();
            dbus_connection_read_write_dispatch(client_conn_, 5);
            reply = dbus_connection_pop_message(client_conn_);
            if (reply) break;
        }
        if (!reply) return out;

        out.got = true;
        const char* sig = dbus_message_get_signature(reply);
        out.signature = sig ? sig : "";
        if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
            out.is_error = true;
            const char* n = dbus_message_get_error_name(reply);
            out.error_name = n ? n : "";
        } else {
            DBusMessageIter it;
            if (dbus_message_iter_init(reply, &it) &&
                dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                const char* s = nullptr;
                dbus_message_iter_get_basic(&it, &s);
                if (s) out.payload = s;
            }
        }
        dbus_message_unref(reply);
        return out;
    }

    std::unique_ptr<PrivateBus> bus_;
    DBusConnection* server_conn_ = nullptr;
    DBusConnection* client_conn_ = nullptr;
    std::unique_ptr<ServerContext> ctx_;
    std::unique_ptr<DbusService> service_;
    std::atomic<bool> release_{false};
};

// ---------------------------------------------------------------------------
// 契约 1：查询在途时，派发线程必须仍然能答复别的方法
//
// 这是整套改动的**唯一**目的。改动前：GetHistory 的回调在派发线程里跑完
// 才返回，期间 ListInterfaces 完全无响应；改动后：回调只是入队就返回。
//
// 判据不依赖真实慢查询的耗时——用一个"阻塞到测试放行"的 job 精确构造
// "在途"状态，再断言 ListInterfaces 仍能在远小于阻塞时长内得到答复。
// ---------------------------------------------------------------------------
TEST_F(SlowQueryDbusTest, DispatchThreadStaysResponsiveWhileQueryInFlight) {
    DBusMessage* call = dbus_message_new_method_call(
        weaknet_dbus::kBusName, weaknet_dbus::kObjectPath,
        weaknet_dbus::kInterface, weaknet_dbus::kMethodGetHistory);
    ASSERT_NE(call, nullptr);

    // 一条永不返回（直到 release_）的受控查询：占用 worker，不占用主线程
    ASSERT_TRUE(service_->enqueueSlowQuery(server_conn_, call, [this]() {
        while (!release_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return std::string("[]");
    }));
    dbus_message_unref(call);

    // 先把 GetHistory 这条请求派发掉（回调只是入队，应当立刻返回）
    pumpServer();
    pumpServer();

    // 现在"查询在途"。别的 D-Bus 方法必须照常答复。
    const long long latency = callListInterfacesLatencyMs();
    EXPECT_GE(latency, 0) << "查询在途期间 ListInterfaces 完全没有回复——"
                             "说明查询仍占着派发线程";
    EXPECT_LT(latency, 200)
        << "查询在途期间 ListInterfaces 耗时 " << latency
        << "ms：派发线程被阻塞（预期应是个位数毫秒）";
}

// ---------------------------------------------------------------------------
// 契约 2：结果必须原样送达
//
// 异步化的最大风险是"回复丢了 / 内容截断"。这里用真实 SQLite 建库，
// 走完整链路（handler → 队列 → worker 查询 → 主线程发回复），断言客户端
// 拿到的 JSON 与直接查库的结果一致。
// ---------------------------------------------------------------------------
TEST_F(SlowQueryDbusTest, AsyncReplyDeliversSamePayloadAsDirectQuery) {
    const std::string db_path =
        "/tmp/weaknet_slowq_" + std::to_string(::getpid()) + ".db";
    std::remove(db_path.c_str());
    ctx_->db_mgr = std::make_unique<weaknet_dbus::DatabaseManager>(db_path);
    ASSERT_TRUE(ctx_->db_mgr->isOpen());

    // 插两行可辨识的数据
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (int i = 0; i < 2; ++i) {
        NetInfo info("slowq" + std::to_string(i));
        info.setRttMs(10 + i);
        info.setJitterMs(1.5);
        info.setRssiDbm(-50);
        info.setTcpLossRate(0.1);
        info.setQuality(LinkQuality::Good);
        info.setUsingNow(i == 0);
        info.setMetricSampleTimes(now_ms, now_ms, now_ms, now_ms, now_ms);
        info.markMetricUpdated(1, now_ms);
        NetworkQualityResult overall;
        overall.score = 80.0;
        overall.level = NetworkQualityLevel::GOOD;
        overall.levelName = "GOOD";
        ASSERT_TRUE(ctx_->db_mgr->insertSnapshot(
            "slowq" + std::to_string(i), info, overall, 1, now_ms,
            info.rttSampleTsMs(), info.rssiSampleTsMs(),
            info.jitterSampleTsMs(), info.tcpLossSampleTsMs(),
            info.trafficSampleTsMs()));
    }
    const std::string direct = ctx_->db_mgr->queryHistory("", "", "", 100);
    ASSERT_NE(direct, "[]");

    const HistoryReply reply = callGetHistory(100);
    ASSERT_TRUE(reply.got) << "没有收到 GetHistory 回复";
    ASSERT_FALSE(reply.is_error) << "收到错误回复: " << reply.error_name;
    EXPECT_EQ(reply.signature, "s") << "回复必须只有一个字符串参数";
    EXPECT_EQ(reply.payload, direct)
        << "异步路径的回复内容必须与直接查库完全一致（不得截断/重排）";

    // 额度必须在回复送达后归还。若不归还，跑满 4 次后队列永久为满、
    // 所有查询一律 Busy——这条断言是那条泄漏的唯一哨兵。
    for (int i = 0; i < 100 && service_->slowQueryQueueDepthForTest() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(service_->slowQueryQueueDepthForTest(), 0u)
        << "回复送达后额度未归还：累计 4 次查询后将永久 Busy";

    std::remove(db_path.c_str());
}

// ---------------------------------------------------------------------------
// 契约 3：队列满时立刻回 BUSY，而不是让客户端等到超时
// ---------------------------------------------------------------------------
TEST_F(SlowQueryDbusTest, QueueFullRepliesBusyImmediately) {
    // 用受控 job 把额度占满
    std::vector<DBusMessage*> holds;
    for (size_t i = 0; i < DbusService::kMaxSlowQueryQueueForTest; ++i) {
        DBusMessage* m = dbus_message_new_method_call(
            weaknet_dbus::kBusName, weaknet_dbus::kObjectPath,
            weaknet_dbus::kInterface, weaknet_dbus::kMethodGetHistory);
        ASSERT_NE(m, nullptr);
        ASSERT_TRUE(service_->enqueueSlowQuery(server_conn_, m, [this]() {
            while (!release_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return std::string("[]");
        }));
        holds.push_back(m);
    }
    ASSERT_EQ(service_->slowQueryQueueDepthForTest(),
              DbusService::kMaxSlowQueryQueueForTest);

    // 此刻客户端再发一条：必须**立刻**拿到 Busy 错误
    const auto t0 = std::chrono::steady_clock::now();
    const HistoryReply reply = callGetHistory(100);
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    ASSERT_TRUE(reply.got) << "队列满时客户端不应等到超时";
    EXPECT_TRUE(reply.is_error) << "队列满必须回错误，而不是排队";
    EXPECT_EQ(reply.error_name, "com.example.WeakNet.Error.Busy");
    EXPECT_LT(elapsed, 200) << "拒绝必须是即时的，实测 " << elapsed << "ms";

    for (DBusMessage* m : holds) dbus_message_unref(m);
}

// ---------------------------------------------------------------------------
// 契约 4：关停后不再有悬挂的回复
//
// 关停路径必须把排队项全部丢弃并 unref。若只置标志不清理，那些 DBusMessage
// 引用会随进程一直悬着；更糟的是若 worker 在 Looper 停后仍 post，
// 任务会被静默丢弃而消息引用泄漏。
//
// 放在 Part A（conn=nullptr）而不是总线部分：本用例验的是计数与队列清理
// 这两个纯逻辑，不需要任何总线流量。用真实总线反而会踩到 libdbus 的一条
// 断言——`dbus_message_new_method_return/error` 要求入站消息 serial != 0，
// 而测试自造的消息从未过总线、serial 恒为 0。生产路径不受影响（handler
// 收到的消息必然来自总线），但测试不该依赖这个前置条件。
// ---------------------------------------------------------------------------
TEST_F(SlowQueryQueueTest, StopDiscardsQueuedJobsAndDropsPendingCount) {
    for (size_t i = 0; i < DbusService::kMaxSlowQueryQueueForTest; ++i) {
        DBusMessage* m = makeCall(weaknet_dbus::kMethodGetHistory);
        ASSERT_NE(m, nullptr);
        ASSERT_TRUE(service_->enqueueSlowQuery(nullptr, m, blockingJob()));
        dbus_message_unref(m);
    }
    ASSERT_EQ(service_->slowQueryQueueDepthForTest(),
              DbusService::kMaxSlowQueryQueueForTest);

    release_.store(true);
    service_->stopSlowQueryWorker();

    EXPECT_EQ(service_->slowQueryQueueDepthForTest(), 0u)
        << "关停后额度必须清零，否则计数与队列不一致";

    // 关停后再入队必须被拒（且不增加计数）
    DBusMessage* after = makeCall(weaknet_dbus::kMethodGetHistory);
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(service_->enqueueSlowQuery(nullptr, after, []() {
        return std::string("[]");
    }));
    dbus_message_unref(after);
    EXPECT_EQ(service_->slowQueryQueueDepthForTest(), 0u);
}

}  // namespace
