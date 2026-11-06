/**
 * @file data_source.hpp
 * @brief 取数层：调用 libweaknet 的 C API 拿原始 JSON
 *
 * ## 为什么单独一层
 *
 * 数据层（`parse_xxx`）只做 JSON → POD 的纯转换，它不知道数据从哪来。
 * 取数层负责「去拿 JSON」这件事，是唯一直接调用 `weaknet_*` 的地方。
 * 解析由调用方（主线程槽函数）调 `parse_xxx()` 完成，本层不解析。
 *
 * 分开的好处：数据层可以在没有 D-Bus、没有服务端的机器上单测；
 * 取数层可以在板端用真实服务验证。
 *
 * ## 为什么必须后台线程
 *
 * `weaknet_get_*` 内部走 D-Bus 同步调用（`dbus_connection_send_with_reply_and_block`），
 * 在服务端忙或 D-Bus 卡住时会阻塞数秒。如果在 Qt 主线程调用，界面会冻结。
 *
 * ## 缓冲区策略
 *
 * C API 的签名是 `(buffer, buffer_size, error_buffer, error_size)`，
 * 返回值只表示调用成败，不告诉你需要多大缓冲区。看板的 JSON 可能较大
 * （schema v2 快照含 13 个维度 + evidence，历史 100 条），因此采用
 * **先试小缓冲区、遇到疑似截断就扩大重试**的策略。
 *
 * 注意：C API 没有明确的「缓冲区不足」错误码，截断时可能返回 true 但
 * JSON 不完整。因此这里用 `JsonReader::valid()` 判断完整性，不合法就翻倍重试。
 */

#ifndef WEAKNET_QT_DASHBOARD_DATA_SOURCE_HPP
#define WEAKNET_QT_DASHBOARD_DATA_SOURCE_HPP

#include <cstddef>
#include <string>

namespace weaknet {
namespace dashboard {

/**
 * @brief 一次取数操作的结果
 *
 * 把「成功/失败」和「失败原因」显式分开，避免调用方拿着空字符串猜。
 */
struct FetchResult {
    /// C API 是否返回成功
    bool ok = false;

    /// 失败原因（ok 为 true 时为空）
    std::string error;

    /// 原始 JSON（调试用；失败时可能为空或被截断）
    std::string json;
};

/**
 * @brief 取数层
 *
 * 所有方法都是**阻塞**的，必须在后台线程调用，不要放在 UI 线程。
 */
class DataSource {
public:
    DataSource();
    ~DataSource();

    // 禁止拷贝：内部持有 weaknet_init/cleanup 的配对状态
    DataSource(const DataSource&) = delete;
    DataSource& operator=(const DataSource&) = delete;

    /**
     * @brief 初始化 libweaknet 并建立 D-Bus 连接
     *
     * 幂等：重复调用不会重复初始化。
     *
     * @return true 成功；false 失败（可用 last_error() 取原因）
     */
    bool init();

    /**
     * @brief 释放 D-Bus 连接
     *
     * 析构时自动调用。
     */
    void cleanup();

    /// 最近一次 init() 失败的原因
    const std::string& last_error() const { return last_error_; }

    // -----------------------------------------------------------------------
    // 取数接口（全部阻塞）
    // -----------------------------------------------------------------------

    /**
     * @brief 取网络体验快照（schema v2）
     *
     * 对应 D-Bus `GetNetworkExperience`。
     * 服务端在无快照时也会返回合法的 UNKNOWN JSON，调用方用
     * `parse_experience()` + `available()` 判断，不在这里解析。
     */
    FetchResult fetch_experience_raw() const;

    /**
     * @brief 取诊断事实
     */
    FetchResult fetch_diagnosis_raw() const;

    /**
     * @brief 取历史趋势
     *
     * @param interface 网卡名；空字符串表示全部
     * @param limit     最多返回条数（服务端上限 10000，且 <=0 会被钳制为 1）
     */
    FetchResult fetch_history_raw(const std::string& interface, int limit) const;

private:
    /**
     * @brief 通用取数模板：调用 C API 并按需扩大缓冲区
     *
     * @param caller 形如 `[](char* b, size_t n, char* e, size_t en) { return weaknet_xxx(b, n, e, en); }`
     */
    template <typename Caller>
    FetchResult invoke(Caller caller) const;

    bool initialized_ = false;
    std::string last_error_;
};

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_DATA_SOURCE_HPP
