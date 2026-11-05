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

/// 缓冲区上限。防止异常情况下无限翻倍。
/// 服务端 GetHistory 的 D-Bus 消息上限是 128 MB，但看板不需要那么大。
constexpr size_t kMaxBuffer = 4 * 1024 * 1024;

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
            result.buffer_used = buffer.size();
            return result;
        }

        // 成功时要确认内容完整：C API 不报告截断，只能自己判断。
        // 判定标准：非空，且 JsonReader 认为结构闭合。
        std::string candidate(buffer.data());

        if (candidate.empty()) {
            result.error = "C API returned true but buffer is empty";
            result.buffer_used = buffer.size();
            return result;
        }

        // 内容长度接近缓冲区上限 → 极可能被截断，扩大重试
        const bool near_full = candidate.size() + 1 >= buffer.size();
        JsonReader reader(candidate);
        const bool complete = reader.valid();

        if (complete && !near_full) {
            result.ok = true;
            result.json = std::move(candidate);
            result.buffer_used = buffer.size();
            return result;
        }

        if (buffer.size() >= kMaxBuffer) {
            // 已达上限仍不完整：如实报告，不假装成功
            result.error = "response exceeds max buffer (" +
                           std::to_string(kMaxBuffer) + " bytes) or is malformed";
            result.json = std::move(candidate);
            result.buffer_used = buffer.size();
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
                                          int limit) const {
    return invoke([&interface, limit](char* b, size_t n, char* e, size_t en) {
        // start / end 传空串表示不限时间范围
        return weaknet_get_history(interface.c_str(), "", "", limit,
                                   b, n, e, en);
    });
}

} // namespace dashboard
} // namespace weaknet
