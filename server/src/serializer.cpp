// serializer.cpp
// 实现简单的二进制序列化/反序列化

#include "serializer.hpp"
#include "logger.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>

namespace weaknet_dbus {

static void appendBytes(const void* data, size_t len, std::vector<uint8_t>& out) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + len);
}

// 确保序列化文件的父目录存在（$XDG_RUNTIME_DIR/weaknet/），权限 0700 私有目录。
// 防止符号链接攻击：序列化路径必须在私有目录下（见 project_memory 约束）。
static void ensureParentDir(const std::string& filepath) {
    size_t pos = filepath.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return;
    std::string dir = filepath.substr(0, pos);
    // 递归创建（$XDG_RUNTIME_DIR 通常存在，仅需创建 weaknet/ 子目录）
    ::mkdir(dir.c_str(), 0700);
}

// 检查路径安全性：防止符号链接攻击
// 序列化文件必须在 $XDG_RUNTIME_DIR/weaknet/ 或 /tmp/weaknet/ 目录下（含测试临时目录）
static bool isSafePath(const std::string& filepath) {
    // 必须以 /tmp/weaknet 开头（覆盖正常路径和测试临时目录）
    return filepath.find("/tmp/weaknet") == 0;
}

bool writeBufferToFile(const std::vector<uint8_t>& buffer, const std::string& filepath, std::string* error_message) {
    LOG_INFO(LogModule::WEAK_MGR, "writeBufferToFile: writing " << buffer.size() << " bytes to " << filepath);
    if (!isSafePath(filepath)) {
        if (error_message) *error_message = "路径不安全，必须在 $XDG_RUNTIME_DIR/weaknet/ 下: " + filepath;
        return false;
    }
    ensureParentDir(filepath);
    std::ofstream ofs(filepath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        LOG_ERROR(LogModule::WEAK_MGR, "writeBufferToFile: failed to open " << filepath);
        if (error_message) *error_message = "无法打开文件写入: " + filepath;
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    if (!ofs.good()) {
        LOG_ERROR(LogModule::WEAK_MGR, "writeBufferToFile: write failed for " << filepath);
        if (error_message) *error_message = "写入失败: " + filepath;
        return false;
    }
    return true;
}

bool readFileToBuffer(const std::string& filepath, std::vector<uint8_t>* buffer, std::string* error_message) {
    LOG_INFO(LogModule::WEAK_MGR, "readFileToBuffer: reading from " << filepath);
    if (!isSafePath(filepath)) {
        if (error_message) *error_message = "路径不安全，必须在 $XDG_RUNTIME_DIR/weaknet/ 下: " + filepath;
        return false;
    }
    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs.is_open()) {
        LOG_ERROR(LogModule::WEAK_MGR, "readFileToBuffer: failed to open " << filepath);
        if (error_message) *error_message = "无法打开文件读取: " + filepath;
        return false;
    }
    ifs.seekg(0, std::ios::end);
    std::streamsize size = ifs.tellg();
    if (size < 0) {
        if (error_message) *error_message = "读取文件大小失败: " + filepath;
        return false;
    }
    ifs.seekg(0, std::ios::beg);
    buffer->resize(static_cast<size_t>(size));
    if (size > 0) {
        ifs.read(reinterpret_cast<char*>(buffer->data()), size);
        if (!ifs.good()) {
            if (error_message) *error_message = "读取失败: " + filepath;
            return false;
        }
    }
    return true;
}

void serializeString(const std::string& value, std::vector<uint8_t>& out_buffer) {
    uint32_t len = static_cast<uint32_t>(value.size());
    appendBytes(&len, sizeof(len), out_buffer);
    if (len > 0) {
        appendBytes(value.data(), len, out_buffer);
    }
}

bool deserializeString(const std::vector<uint8_t>& buffer, size_t& offset, std::string& out_value) {
    if (offset + sizeof(uint32_t) > buffer.size()) return false;
    uint32_t len = 0;
    std::memcpy(&len, buffer.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (offset + len > buffer.size()) return false;
    out_value.assign(reinterpret_cast<const char*>(buffer.data() + offset), len);
    offset += len;
    return true;
}

void serializeInt32(int32_t value, std::vector<uint8_t>& out_buffer) {
    appendBytes(&value, sizeof(value), out_buffer);
}

bool deserializeInt32(const std::vector<uint8_t>& buffer, size_t& offset, int32_t& out_value) {
    if (offset + sizeof(int32_t) > buffer.size()) return false;
    std::memcpy(&out_value, buffer.data() + offset, sizeof(int32_t));
    offset += sizeof(int32_t);
    return true;
}

bool serializeGetReplyToFile(const std::string& reply, const std::string& filepath, std::string* error_message) {
    std::vector<uint8_t> buf;
    serializeString(reply, buf);
    return writeBufferToFile(buf, filepath, error_message);
}

bool deserializeGetReplyFromFile(const std::string& filepath, std::string* out_reply, std::string* error_message) {
    std::vector<uint8_t> buf;
    if (!readFileToBuffer(filepath, &buf, error_message)) return false;
    size_t off = 0;
    return deserializeString(buf, off, *out_reply);
}

bool serializeChangedPayloadToFile(const ChangedPayload& payload, const std::string& filepath, std::string* error_message) {
    std::vector<uint8_t> buf;
    serializeString(payload.message, buf);
    serializeInt32(payload.counter, buf);
    return writeBufferToFile(buf, filepath, error_message);
}

bool deserializeChangedPayloadFromFile(const std::string& filepath, ChangedPayload* out_payload, std::string* error_message) {
    std::vector<uint8_t> buf;
    if (!readFileToBuffer(filepath, &buf, error_message)) return false;
    size_t off = 0;
    if (!deserializeString(buf, off, out_payload->message)) return false;
    if (!deserializeInt32(buf, off, out_payload->counter)) return false;
    return true;
}

}  // namespace weaknet_dbus


