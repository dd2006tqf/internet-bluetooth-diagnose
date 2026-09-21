#pragma once

#include <string>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <random>

namespace weaknet {

/**
 * @brief 全局稳定证据 ID 生成服务
 *
 * 遵循严格契约：
 * 正常状态: ev_<device_id>_e<epoch>_<domain>_s<seq>_<idx>
 * 容灾降级: ev_<device_id>_<boot_uuid>_<domain>_s<seq>_<idx>
 *
 * 跨重启、单设备唯一；结合 device_id 保证云端汇聚时不碰撞。
 */
class EvidenceIdGenerator {
public:
    EvidenceIdGenerator(std::string device_id, uint64_t epoch, bool epoch_valid = true, std::string boot_uuid = "")
        : device_id_(std::move(device_id)), epoch_(epoch), epoch_valid_(epoch_valid), boot_uuid_(std::move(boot_uuid)) {
        if (device_id_.empty()) {
            device_id_ = "unknown_device";
        }
        if (!epoch_valid_ && boot_uuid_.empty()) {
            boot_uuid_ = generateRandomUuid();
        }
    }

    std::string generate(const std::string& domain, uint64_t snapshot_seq, uint32_t item_index) const {
        std::ostringstream oss;
        oss << "ev_" << device_id_ << "_";
        if (epoch_valid_) {
            oss << "e" << epoch_ << "_";
        } else {
            oss << boot_uuid_ << "_";
        }
        oss << domain << "_s" << snapshot_seq << "_" << std::setw(2) << std::setfill('0') << item_index;
        return oss.str();
    }

    const std::string& deviceId() const { return device_id_; }
    uint64_t epoch() const { return epoch_; }
    bool isEpochValid() const { return epoch_valid_; }
    const std::string& bootUuid() const { return boot_uuid_; }

private:
    static std::string generateRandomUuid() {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dis;
        uint64_t part1 = dis(gen);
        uint64_t part2 = dis(gen);
        std::ostringstream oss;
        oss << std::hex << std::setfill('0')
            << std::setw(16) << part1
            << std::setw(16) << part2;
        return oss.str().substr(0, 16);
    }

    std::string device_id_;
    uint64_t epoch_{0};
    bool epoch_valid_{true};
    std::string boot_uuid_;
};

} // namespace weaknet
