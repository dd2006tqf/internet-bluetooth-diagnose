/**
 * @file data_source.cpp
 * @brief DataSource 实现
 */

#include "data_source.hpp"

#include <cstring>
#include <vector>

#include "json_reader.hpp"
#include "weaknet_client.h"

namespace weaknet {
namespace dashboard {

namespace {

/// 初始缓冲区大小。schema v2 快照典型 3-8 KB，先给 8 KB。
constexpr size_t kInitialBuffer = 8 * 1024;

/**
 * @brief 缓冲区上限
 *
 * 由 4 MB 上调到 12 MB。原因不是"多给点总没错"，而是有一个具体的
 * 满载需求装不下：
 *
 *   服务端 `kMaxHistoryLimit` = 10000 点（这是任何窗口的硬上界）
 *   真机实测每行 JSON 约 784 字节（kBoardRealHistory 的字段形状）
 *   → 10000 × 784 ≈ 7.5 MB，超过原上限 4 MB。
 *
 * 超过上限时的行为是**报错**（不是静默截断），所以原有代码不会给出假数据；
 * 代价是满载查询永远失败。既然时间窗切换是白名单批准的现场功能，
 * 就得让它真能工作。
 *
 * ## 为什么按 10000 点算而不是按某个窗口的常态算
 *
 * 板端实测（2026-11-08）：24h 请求在历史有大量空档时只取回 2610 条 /
 * 2.19 MB。若按这个常态定上限，就会在设备连续运行、数据变密之后突然失败。
 * 稀疏是偶然，满载才是要保证的上界。
 *
 * 取 12 MB 而不是刚好 7.5 MB：留出约 1.6 倍余量，因为
 *   - 每行含 iface / rssi_source / score_model 等字符串字段，长度可变
 *   - 全部网卡查询（interface 为空）时行数不变但字段可能更长
 *   - assessment_profile 从 UNSPECIFIED 变成 INTERNET_ACCESS 会让每行长几字节
 *
 * 内存代价：这块缓冲只存在于取数线程的栈外堆上，且只在一次调用期间存在，
 * 板端（1GB+ 内存）完全承受得起。相比之下，静默少画几个小时更不可接受。
 */
constexpr size_t kMaxBuffer = 12 * 1024 * 1024;

/// 错误信息缓冲区大小
constexpr size_t kErrorBufferSize = 512;

} // namespace

DataSource::DataSource() = default;

DataSource::~DataSource() {
    cleanup();
}

bool DataSource::init() {
    if (initialized_) {
        return true;
    }
    last_error_.clear();
    if (!weaknet_init()) {
        // 必须 cleanup，否则本层将永久卡死在失败状态。
        //
        // weaknet_init() 在 client.cpp:1348 的实现是：g_client 非空时直接
        // return g_client->isConnected()，而 connected_ 只在 initConnection()
        // 里赋值一次。连接失败后 g_client 仍留在全局，于是后续每次
        // weaknet_init() 都走 "already initialized" 分支、永远返回 false。
        //
        // weaknet_cleanup() 把 g_client 置空并 disconnect，下一次 init() 才会
        // 真正重建连接。副作用是它会调用 Logger::shutdown()，下次 init 再
        // InitGoogleLogging()——已实测 Init/Shutdown 反复循环安全。
        weaknet_cleanup();
        last_error_ = "weaknet_init() failed: cannot establish D-Bus connection";
        return false;
    }
    initialized_ = true;
    return true;
}

void DataSource::cleanup() {
    if (initialized_) {
        // weaknet_cleanup() 释放内部 D-Bus 连接。
        // 多次调用是安全的，但这里仍用标志位确保配对。
        weaknet_cleanup();
        initialized_ = false;
    }
}

// ---------------------------------------------------------------------------
// 通用取数模板
// ---------------------------------------------------------------------------

template <typename Caller>
FetchResult DataSource::invoke(Caller caller) const {
    FetchResult result;

    if (!initialized_) {
        result.error = "DataSource not initialized; call init() first";
        return result;
    }

    std::vector<char> buffer(kInitialBuffer, '\0');
    std::vector<char> error(kErrorBufferSize, '\0');

    while (true) {
        // 每轮重置，避免上一轮的残留内容干扰判断
        std::fill(buffer.begin(), buffer.end(), '\0');
        std::fill(error.begin(), error.end(), '\0');

        const bool ok = caller(buffer.data(), buffer.size(),
                               error.data(), error.size());

        // error 缓冲区非空表示服务端或库层报了明确错误
        const std::string error_text(error.data());

        if (!ok) {
            result.error = error_text.empty()
                               ? "C API returned false without error message"
                               : error_text;
            result.json.assign(buffer.data());
            return result;
        }

        // 成功时要确认内容完整：C API 不报告截断，只能自己判断。
        // 判定标准：非空，且 JsonReader 认为结构闭合。
        std::string candidate(buffer.data());

        if (candidate.empty()) {
            result.error = "C API returned true but buffer is empty";
            return result;
        }

        // 内容长度接近缓冲区上限 → 极可能被截断，扩大重试
        const bool near_full = candidate.size() + 1 >= buffer.size();
        JsonReader reader(candidate);
        const bool complete = reader.valid();

        if (complete && !near_full) {
            result.ok = true;
            result.json = std::move(candidate);
            return result;
        }

        if (buffer.size() >= kMaxBuffer) {
            // 已达上限仍不完整：如实报告，不假装成功
            result.error = "response exceeds max buffer (" +
                           std::to_string(kMaxBuffer) + " bytes) or is malformed";
            result.json = std::move(candidate);
            return result;
        }

        buffer.resize(buffer.size() * 2);
    }
}

// ---------------------------------------------------------------------------
// 具体接口
// ---------------------------------------------------------------------------

FetchResult DataSource::fetch_experience_raw() const {
    return invoke([](char* b, size_t n, char* e, size_t en) {
        return weaknet_get_network_experience(b, n, e, en);
    });
}

FetchResult DataSource::fetch_diagnosis_raw() const {
    return invoke([](char* b, size_t n, char* e, size_t en) {
        return weaknet_get_diagnosis(b, n, e, en);
    });
}

FetchResult DataSource::fetch_history_raw(const std::string& interface,
                                          int limit,
                                          const std::string& start,
                                          const std::string& end) const {
    return invoke([&interface, limit, &start, &end](char* b, size_t n, char* e,
                                                    size_t en) {
        // start / end 传空串表示不限时间范围
        return weaknet_get_history(interface.c_str(), start.c_str(), end.c_str(),
                                   limit, b, n, e, en);
    });
}

} // namespace dashboard
} // namespace weaknet
