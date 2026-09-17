/**
 * @file network_epoch_store.cpp
 * @brief 网络代次的跨重启持久化（实现）
 *
 * 设计取舍见头文件。实现上只依赖标准库与 POSIX 文件 API，
 * 不引入 JSON/配置库——这里只有一个整数，格式越简单越不易写坏。
 */

#include "network_epoch_store.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "logger.hpp"

namespace weaknet {

NetworkEpochStore::NetworkEpochStore(std::string state_path)
    : state_path_(std::move(state_path)) {}

bool NetworkEpochStore::readState(uint64_t* out) const {
    std::ifstream in(state_path_);
    if (!in.is_open()) {
        return false;
    }
    std::string raw;
    std::getline(in, raw);
    // 去掉可能的首尾空白（手改过的文件常见）
    const auto begin = raw.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return false;
    }
    const auto end = raw.find_last_not_of(" \t\r\n");
    raw = raw.substr(begin, end - begin + 1);

    // 严格解析：只接受纯十进制数字。strtoull 会接受 "12abc" 这类前缀，
    // 那会让"写坏的文件"被当成有效值，正是本类要防的情形。
    if (raw.empty()) return false;
    for (char c : raw) {
        if (c < '0' || c > '9') return false;
    }
    errno = 0;
    char* parse_end = nullptr;
    const unsigned long long value = std::strtoull(raw.c_str(), &parse_end, 10);
    if (errno != 0 || parse_end == nullptr || *parse_end != '\0') {
        return false;
    }
    // 0 不是一个有效代次：服务端契约要求 network_epoch >= 0，但 0 意味着
    // "未初始化"，而历史数据里 epoch=1 一定存在过，复用 1 是危险的。
    if (value == 0) {
        return false;
    }
    *out = static_cast<uint64_t>(value);
    return true;
}

bool NetworkEpochStore::writeState(uint64_t value) const {
    // 先写临时文件再 rename：rename 在同一文件系统上是原子的，
    // 因此掉电只可能留下完整的旧文件或完整的新文件，不会出现半截内容。
    const std::string tmp_path = state_path_ + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << value << "\n";
        out.flush();
        if (!out.good()) {
            return false;
        }
    }
    if (std::rename(tmp_path.c_str(), state_path_.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

uint64_t NetworkEpochStore::open() {
    uint64_t previous = 0;
    const bool have_state = readState(&previous);

    // 文件是否存在，决定"读不出值"属于哪种情形：
    // 文件在 ⇒ 内容损坏；文件不在 ⇒ 真正的首次运行。
    // 两者绝不能混为一谈——把损坏当首次会让代次退回 1，
    // 而 epoch=1 的历史键几乎必然已存在，等于重新触发数据丢失。
    std::ifstream probe(state_path_);
    const bool file_exists = probe.is_open();
    probe.close();

    uint64_t next = 0;
    if (have_state) {
        next = previous + 1;
    } else if (file_exists) {
        next = kRecoveryEpoch;
        LOG_WARNING(weaknet_dbus::LogModule::SYSTEM,
                    "network epoch state unreadable, restarting from " << kRecoveryEpoch
                                                                      << ": " << state_path_);
    } else {
        // 首次运行：与既有部署的 (epoch=1, seq=N) 历史保持一致，
        // 不制造一次性的键空间跳变。
        next = 1;
    }

    if (!writeState(next)) {
        // 写盘失败 ⇒ 本次用掉的代次无法被记住，下次重启就不会推进。
        // 此时返回低位代次有实质风险（epoch=1 必然已用过），故升到恢复位；
        // 但仍继续上报——停止上报的代价比一次代次跳变大得多。
        if (next <= 1) {
            next = kRecoveryEpoch;
        }
        LOG_WARNING(weaknet_dbus::LogModule::SYSTEM,
                    "network epoch state could not be persisted: " << state_path_);
    }

    current_ = next;
    LOG_INFO(weaknet_dbus::LogModule::SYSTEM, "network epoch for this run: " << current_);
    return current_;
}

}  // namespace weaknet
