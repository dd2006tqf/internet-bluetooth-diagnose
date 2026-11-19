/**
 * @file dbus_service.hpp
 * @brief D-Bus 服务层：方法处理
 *
 * DbusService 封装了 libdbus-1 C API 的样板代码，提供：
 *   - register_on_connection() 将自身方法表注册到 DBusConnection
 *   - handleXxx() 系列方法：供静态回调函数转发调用（避免头文件暴露 dbus 类型）
 *
 * 设计模式：
 *   D-Bus 要求回调函数签名为自由函数，且不允许捕获 this。
 *   因此使用静态函数 MessageHandlerStatic → 从 DBusConnection user_data
 *   取回 DbusService* → 调用成员函数 handleXxx。
 *
 * O3-B 说明：此前本类还提供 emitChanged()/emitSpecificSignal()/
 * emitNetworkQualitySignal()（含 sendSignalInternal 重试底层与 send_mutex_
 * 串行化），用于发射 Changed 等 7 个信号，这些已于 2026-10-19 整段删除；
 * 客户端侧的订阅 API（weaknet_subscribe_* / weaknet_check_*）也已于
 * 2026-11-01 一并删除，不再是"暂留待处置"的状态。
 *
 * 不发信号是**架构选择**而非能力缺失：对外产出是迟滞平滑过的结论型快照，
 * 信号无投递确认且无法表达「不知道」，消费者漏一条就永久错。详见
 * dbus_service.cpp 文件头的 O3-B 说明。
 */

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <cstddef>

struct DBusConnection;
struct DBusMessage;

// 前置声明监控器类型（仅用于指针成员，避免头文件依赖）
namespace weaknet_dbus {
class DnsMonitor;
class WifiPacketLossMonitor;
class HttpLatencyMonitor;
class ProcessNetProfiler;
}

namespace weaknet_dbus {

class ServerContext;  ///< 前置声明

/**
 * @brief D-Bus 服务实现
 *
 * 构造时接收 ServerContext 指针（非 owning），
 * 所有 handleXxx 方法通过 ctx_ 访问 weak_mgr / bt_monitor / eBPF 监控器。
 */
class DbusService {
public:
    /**
     * @brief 构造：绑定 ServerContext
     * @param ctx 全局上下文（非 owning，必须在 DbusService 析构前保持有效）
     */
    explicit DbusService(ServerContext* ctx);

    /// 析构时停止查询 worker：它持有 ctx_ 的使用权，必须在 ctx_ 失效前退出。
    ~DbusService();

    /**
     * @brief 停止查询 worker 并 join（幂等）。
     *
     * **必须由关停路径显式调用**，不能只依赖析构：ServerContext 的成员
     * 析构顺序是声明逆序，`db_mgr`（靠后声明）会先于 `service` 被销毁，
     * 而 worker 正在执行的查询恰恰解引用 `db_mgr`。若等到 ~DbusService
     * 才 join，中间那段窗口就是 use-after-free。
     *
     * 调用点在 Looper::run() 返回之后、任何共享资源释放之前。
     */
    void stopSlowQueryWorker();

    /**
     * @brief 统一的慢查询入队入口；clamp/校验由各 handler 完成后调用。
     *
     * 队列满或已关停时**立即**回复错误并返回 true（调用已处理，只是结果是
     * 拒绝），绝不无限排队——调用方等 10 秒才失败比立刻知道"服务端忙"更糟。
     *
     * 设为 public 方便单测直接构造受控 job（真实验证队列上限语义），
     * 与 applyPendingActions / pendingActionResultCount 同一惯例。
     */
    bool enqueueSlowQuery(DBusConnection* conn, DBusMessage* msg,
                          std::function<std::string()> run);

    /// 当前未答复的查询数（排队 + 在途）。仅供测试与诊断，生产路径不消费。
    size_t slowQueryQueueDepthForTest() const {
        std::lock_guard<std::mutex> lock(slow_query_mutex_);
        return slow_query_pending_;
    }

    /**
     * @brief 注册方法分发器到 D-Bus 连接
     *
     * 注册 kObjectPath 对象路径和 kMethodXxx 方法的分发回调。
     * 内部通过 dbus_connection_add_filter 安装一个全局过滤器，
     * 在过滤器中通过 DBUS_MESSAGE_TYPE_METHOD_CALL 判断并路由。
     *
     * @param conn 已注册服务名的 DBusConnection
     * @return true 注册成功，false 失败（通常是 conn 为空）
     */
    bool register_on_connection(DBusConnection* conn);

    // ---------- 方法实现（供静态分发函数调用） ----------

    // 基础查询方法
    bool handleListInterfaces(DBusConnection* conn, DBusMessage* msg);       ///< ListInterfaces: 返回网卡名数组
    bool handleHealthCheck(DBusConnection* conn, DBusMessage* msg);          ///< HealthCheck: 执行完整健康检查
    bool handleGetDiagnosis(DBusConnection* conn, DBusMessage* msg);         ///< GetDiagnosis: 返回端侧确定性机器诊断事实 JSON
    bool handleExecuteAction(DBusConnection* conn, DBusMessage* msg);        ///< ExecuteAction: 执行白名单安全动作
    bool handlePing(DBusConnection* conn, DBusMessage* msg);                 ///< Ping: 对指定主机执行 ICMP

    // 蓝牙相关方法
    bool handleGetBluetoothDevices(DBusConnection* conn, DBusMessage* msg);   ///< GetBluetoothDevices: 返回设备列表
    bool handleGetBluetoothAdapter(DBusConnection* conn, DBusMessage* msg);   ///< GetBluetoothAdapter: 返回适配器状态
    bool handleGetBluetoothAudioQuality(DBusConnection* conn, DBusMessage* msg); ///< GetBluetoothAudioQuality: 返回指定设备音频质量与 eBPF 融合诊断 JSON
    bool handleQueryDeviceEvents(DBusConnection* conn, DBusMessage* msg);     ///< QueryDeviceEvents: 查询规范化无线设备事件（只读）
    bool handleQueryBtDataPath(DBusConnection* conn, DBusMessage* msg);       ///< QueryBtDataPath: 查询工业蓝牙数据通道历史指标（Phase 1b）
    bool handleQuerySiteIncidents(DBusConnection* conn, DBusMessage* msg);    ///< QuerySiteIncidents: 查询区域级异常事件（Phase 3a，只读）
    bool handleGetCoexistenceConflict(DBusConnection* conn, DBusMessage* msg);
    bool handleGetNetworkExperience(DBusConnection* conn, DBusMessage* msg);          ///< GetNetworkExperience: 返回 schema v2 权威评估快照（只读，不重新 evaluate）   ///< GetCoexistenceConflict: 返回 Wi-Fi 与蓝牙 2.4GHz 冲突诊断 JSON

    // 视频帧分发（Plan B：单一所有者 + 句柄分发）
    bool handleGetVideoFrame(DBusConnection* conn, DBusMessage* msg);        ///< GetVideoFrame: 返回最新帧的 dma-buf fd + 几何 + sequence
    bool handleReleaseVideoFrame(DBusConnection* conn, DBusMessage* msg);    ///< ReleaseVideoFrame: 归还槽位（不归还会阻塞中枢发布）

    // eBPF 监控数据方法
    bool handleGetDnsStats(DBusConnection* conn, DBusMessage* msg);           ///< GetDnsStats: DNS 监控最近统计
    bool handleGetWifiLossStats(DBusConnection* conn, DBusMessage* msg);      ///< GetWifiLossStats: Wi-Fi 丢包归因
    bool handleGetHttpLatencyStats(DBusConnection* conn, DBusMessage* msg);  ///< GetHttpLatencyStats: HTTP TTFB 统计
    bool handleGetProcessProfiling(DBusConnection* conn, DBusMessage* msg);  ///< GetProcessProfiling: 进程画像
    bool handleGetSkbDropStats(DBusConnection* conn, DBusMessage* msg);      ///< GetSkbDropStats: Socket/skb 丢包归因
    bool handleGetTcpRttStats(DBusConnection* conn, DBusMessage* msg);       ///< GetTcpRttStats: 被动 TCP RTT 统计
    bool handleGetTcpRetransStats(DBusConnection* conn, DBusMessage* msg);   ///< GetTcpRetransStats: TCP 连接级重传统计
    bool handleGetTcpConnStats(DBusConnection* conn, DBusMessage* msg);      ///< GetTcpConnStats: TCP 连接生命周期统计
    bool handleGetEbpfMonitorHealth(DBusConnection* conn, DBusMessage* msg);  ///< GetEbpfMonitorHealth: 各监控器健康状态

    // 历史数据
    bool handleGetHistory(DBusConnection* conn, DBusMessage* msg);            ///< GetHistory: SQLite 历史查询

    // 运行时配置
    bool handleSetMonitorParam(DBusConnection* conn, DBusMessage* msg);       ///< SetMonitorParam: 校验+原子提交
    bool handleGetMonitorParam(DBusConnection* conn, DBusMessage* msg);       ///< GetMonitorParam: 返回 JSON

    // 监控器生命周期控制
    bool handleListMonitors(DBusConnection* conn, DBusMessage* msg);
    bool handleGetMonitorStatus(DBusConnection* conn, DBusMessage* msg);
    bool handleEnableMonitor(DBusConnection* conn, DBusMessage* msg);
    bool handleDisableMonitor(DBusConnection* conn, DBusMessage* msg);
    bool handleRestartMonitor(DBusConnection* conn, DBusMessage* msg);
    bool handleSaveMonitorOverrides(DBusConnection* conn, DBusMessage* msg);
    bool handleGetUpdateStatus(DBusConnection* conn, DBusMessage* msg);  ///< GetUpdateStatus: OTA 本地升级状态（只读）

    bool handleMonitorOperation(DBusConnection* conn, DBusMessage* msg,
                                const char* operation);

private:
    /**
     * @brief 内部辅助：要求调用方为 root (UID 0)，否则回 ACCESS_DENIED 并返回 false。
     *
     * 用于所有"改变设备行为"的方法（执行动作、运行时调参、监控器生命周期）。
     * 这些方法会改变设备主动连接谁、探测频率或评估是否运行，而
     * `com.example.WeakNet.conf` 的 default 上下文允许任意本地用户发送调用，
     * 因此必须逐方法校验，不能只依赖总线策略。
     *
     * 只读查询方法（Get/HealthCheck/GetDiagnosis 等）不调用本函数。
     */
    bool requireRootCaller(DBusConnection* conn, DBusMessage* msg, const char* method_name);

    /**
     * @brief 内部辅助：向调用方回复字符串数组
     * @param conn D-Bus 连接
     * @param msg  原始方法调用消息（用于创建 reply）
     * @param arr  要返回的字符串数组
     */
    bool replyStringArray(DBusConnection* conn, DBusMessage* msg, const std::vector<std::string>& arr);

private:
    ServerContext* ctx_;     ///< 全局上下文非 owning 指针

    // ---------- 视频帧租约的跨进程记账 ----------
    //
    // ========================================================================
    // 为什么需要按 sender 记账，而不是「调用方自己管好自己的租约」
    // ========================================================================
    //
    // `VideoFrameHub` 的租约协议要求消费者 `release(slot)`。同一进程内这
    // 很容易做到（RAII），但 **D-Bus 是跨进程的**：服务端只知道「谁调了
    // GetVideoFrame」，完全不知道对端什么时候读完、什么时候崩。
    //
    // 如果只依赖中枢的 `lease_timeout_ms`（默认 2s）兜底，会有两个后果：
    //
    //   1. 面板以 10fps 取帧、每帧都忘记 release，2s 内会占满 3 个槽 →
    //      中枢 `skipped_busy` 开始增长 → **发布停摆**。也就是「消费者不会
    //      用」这种现象直接变成了「采集侧故障」，与项目反复强调的
    //      「观测缺陷不得栽给被观测对象」背道而驰。
    //   2. `reclaimed` 计数会把「忘记归还」和真正的「消费者崩溃」混在一起，
    //      失去诊断价值。
    //
    // 解决办法：**服务端替每个 sender 记住它当前持有的那一个槽**，同一个
    // sender 再次取帧时先自动归还上一次的。于是：
    //
    //   - 单个消费者最多同时持 1 个槽，无论它多健忘；
    //   - N 个消费者 → 最多 N 个槽（本板 3 个槽，足够面板 + 编码器）；
    //   - 消费者崩溃时它只浪费 1 个槽，且超时回收仍会把它清掉，
    //     `reclaimed` 也因此只反映真正的异常。
    //
    // 键用 D-Bus 唯一名（`:1.42`）。连接断开后名字会被总线回收，理论上
    // 之后另一个客户端可能复用同名——但此时那条旧租约早已被超时回收，
    // 自动归还也是幂等的（`release()` 对未知 slot 直接返回），不会误伤。
    struct VideoFrameClientLease {
        uint32_t slot{UINT32_MAX};   ///< 该 sender 当前持有的槽（UINT32_MAX = 无）
        uint64_t sequence{0};        ///< 该租约对应的帧序号（用于诊断日志）
    };
    std::mutex video_lease_mutex_;
    std::unordered_map<std::string, VideoFrameClientLease> video_leases_;

    /// 内部辅助：自动归还该 sender 上一次的租约（幂等；无租约时什么都不做）
    void releaseClientLeaseLocked(const std::string& sender);

    // ---------- 慢查询卸载到工作线程 ----------
    //
    // ========================================================================
    // 为什么这些查询不能留在派发线程上
    // ========================================================================
    //
    // D-Bus 派发是**单线程串行**的：一条消息的回调执行期间，整个服务不响应
    // 任何其它方法调用。历史测量（x86，10000 行满库）：queryHistory 的
    // p50 = 118ms、p95 = 130ms，且返回 8.5MB JSON。开发板是 8 核 A55，
    // 单核约为该数值的 3~5 倍。在现场终端选择 13h 窗口（恰好就是
    // kMaxHistoryLimit = 10000）时，主线程会被独占数百毫秒。
    //
    // 后果不是"慢"，而是**连带**：同一窗口内的 HealthCheck、Ping、
    // GetNetworkExperience 全部排队，看板上的"当前状态"跟着卡住。这与
    // Looper 那三个派发坑（25s → 1.3ms）是同一类缺陷。
    //
    // 设计取舍：
    //   - 只有 4 条**纯只读**查询走这条路径（GetHistory / QueryDeviceEvents /
    //     QueryBtDataPath / QuerySiteIncidents）。写操作、配置变更、监控器
    //     生命周期一律留在主线程——它们要修改跨线程共享状态，移走会把
    //     线程模型搞乱，而它们的耗时本来就不随数据量增长。
    //   - 单工作线程 + 有界队列。SQLite 侧已有 FULLMUTEX + busy_timeout，
    //     但**串行执行**保证了并发查询不会互相抢锁把延迟叠起来。
    //   - 队列满时立刻回 BUSY 错误，而不是无限排队：调用方等 10 秒才失败，
    //     比立刻知道"服务端忙"更糟。
    struct SlowQueryJob {
        DBusMessage* msg{nullptr};   ///< 持有引用（已 dbus_message_ref）
        DBusConnection* conn{nullptr};   ///< 回复所用的连接（注册时那条）
        std::function<std::string()> run;  ///< 在 worker 线程执行，返回 JSON 文本
    };

    /// worker 线程主循环。
    void slowQueryWorkerLoop();
    /// 把回复送回主线程发送（可在 worker 线程调用）。
    void replyOnMainThread(DBusConnection* conn, DBusMessage* msg, std::string result);
    /// 启动/停止 worker（幂等）。
    void startSlowQueryWorker();

    /// 主线程的 Looper（构造时捕获）。回复必须经它排队回主线程发送。
    /// 不能在工作线程里调 Looper::current()：那是 thread_local 懒构造，
    /// 会凭空造出一个永远不被 run() 的新实例，任务入队后无人消费，
    /// 表现为"查询成功但客户端永远收不到回复"。
    class Looper* looper_{nullptr};

    std::thread slow_query_thread_;
    mutable std::mutex slow_query_mutex_;
    std::condition_variable slow_query_cv_;
    std::deque<SlowQueryJob> slow_query_queue_;
    bool slow_query_stopping_{false};
    /// 已受理但尚未答复调用方的查询数（排队 + 在途）。
    /// 用"未答复"而不是"排队深度"做上限：后者会随 worker 取走动作漂移，
    /// 无法表达"最多 N 条查询同时在内存里"这个真正要防的东西。
    size_t slow_query_pending_{0};
    /// 队列上限：4 条并发查询已足够（现场终端一次只发一条）。
    static constexpr size_t kMaxSlowQueryQueue = 4;

public:
    /// 供单测断言上限语义（避免测试里重复写死 4 而与实现漂移）。
    static constexpr size_t kMaxSlowQueryQueueForTest = kMaxSlowQueryQueue;

private:
};

}  // namespace weaknet_dbus
