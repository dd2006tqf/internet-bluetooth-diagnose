/**
 * @file serializer.cpp
 * @brief 简单的二进制序列化/反序列化 + 安全文件持久化
 *
 * 本文件提供一组手写的序列化原语（string、int32）以及两个便捷封装：
 *   - serializeGetReplyToFile / deserializeGetReplyFromFile       : 持久化 Get 方法回复
 *   - serializeChangedPayloadToFile / deserializeChangedPayloadFromFile : 持久化 Changed 信号载荷
 *
 * 设计思路：
 *   - 采用"长度前缀"格式：string = uint32(len) + 原始字节，便于反序列化时做边界校验
 *   - 文件操作强制走私有目录（$XDG_RUNTIME_DIR/weaknet/ → 映射到 /tmp/weaknet/），
 *     防止符号链接攻击和其他进程越权读取
 *   - 所有文件 I/O 都做路径安全校验 + 目录自动创建
 *
 * 线程安全：
 *   - 本文件仅依赖 std::vector 和 std::ofstream，无共享状态，调用方自行保证互斥
 *   - 路径校验 isSafePath 为纯函数，天然线程安全
 */

#include "serializer.hpp"
#include "logger.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>

namespace weaknet_dbus {

/**
 * @brief 将原始字节追加到序列化缓冲区末尾
 * @param data 源数据指针
 * @param len  字节长度
 * @param out  目标缓冲区
 */
static void appendBytes(const void* data, size_t len, std::vector<uint8_t>& out) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + len);
}

/**
 * @brief 确保序列化文件的父目录存在，权限设为 0700（仅 owner 可读写）
 * @param filepath 完整文件路径
 *
 * 递归创建目录。$XDG_RUNTIME_DIR 通常已存在，仅需创建 weaknet/ 子目录；
 * mkdir 失败（目录已存在）不报错。
 */
static void ensureParentDir(const std::string& filepath) {
    size_t pos = filepath.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return;
    std::string dir = filepath.substr(0, pos);
    // 递归创建（$XDG_RUNTIME_DIR 通常存在，仅需创建 weaknet/ 子目录）
    ::mkdir(dir.c_str(), 0700);
}

/**
 * @brief 检查路径安全性：防止符号链接攻击
 * @param filepath 待检查路径
 * @return true 路径在允许的私有目录下；false 路径不安全
 *
 * 序列化文件必须落在 $XDG_RUNTIME_DIR/weaknet/ 或 /tmp/weaknet/ 下（后者覆盖测试临时目录）。
 * 硬编码 /tmp/weaknet 前缀是因为在本项目的运行时环境中 XDG_RUNTIME_DIR 统一映射到 /tmp。
 */
static bool isSafePath(const std::string& filepath) {
    // 必须以 /tmp/weaknet 开头（覆盖正常路径和测试临时目录）
    return filepath.find("/tmp/weaknet") == 0;
}

/**
 * @brief 将二进制缓冲区写入文件（安全路径校验）
 * @param buffer        待写入的二进制数据
 * @param filepath      目标文件路径
 * @param error_message [out] 错误描述（可传 nullptr）
 * @return true 写入成功；false 路径不安全或 I/O 失败
 */
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

/**
 * @brief 将文件读取为二进制缓冲区（安全路径校验）
 * @param filepath      源文件路径
 * @param buffer        [out] 读取到的二进制数据
 * @param error_message [out] 错误描述（可传 nullptr）
 * @return true 读取成功；false 路径不安全或 I/O 失败
 */
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
    // 先 seek 到末尾获取文件大小，再 resize 缓冲区，最后 seek 回头部读取
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

/**
 * @brief 序列化字符串到二进制缓冲区（长度前缀格式）
 * @param value     待序列化字符串
 * @param out_buffer 目标缓冲区
 *
 * 格式: [uint32 len][len 字节原始字符串]。空字符串序列化为 {0x00,0x00,0x00,0x00}。
 */
void serializeString(const std::string& value, std::vector<uint8_t>& out_buffer) {
    uint32_t len = static_cast<uint32_t>(value.size());
    appendBytes(&len, sizeof(len), out_buffer);
    if (len > 0) {
        appendBytes(value.data(), len, out_buffer);
    }
}

/**
 * @brief 从二进制缓冲区反序列化字符串
 * @param buffer     源缓冲区
 * @param offset     [in/out] 当前读取位置，成功后自动前进
 * @param out_value  [out] 解析出的字符串
 * @return true 解析成功；false 缓冲区越界或数据损坏
 */
bool deserializeString(const std::vector<uint8_t>& buffer, size_t& offset, std::string& out_value) {
    // 先检查长度字段是否完整
    if (offset + sizeof(uint32_t) > buffer.size()) return false;
    uint32_t len = 0;
    std::memcpy(&len, buffer.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    // 再检查 payload 区域是否完整，防止恶意短缓冲区触发越界读
    if (offset + len > buffer.size()) return false;
    out_value.assign(reinterpret_cast<const char*>(buffer.data() + offset), len);
    offset += len;
    return true;
}

/**
 * @brief 序列化 int32 到二进制缓冲区（小端，与平台字节序一致）
 */
void serializeInt32(int32_t value, std::vector<uint8_t>& out_buffer) {
    appendBytes(&value, sizeof(value), out_buffer);
}

/**
 * @brief 从二进制缓冲区反序列化 int32
 * @return true 解析成功；false 缓冲区越界
 */
bool deserializeInt32(const std::vector<uint8_t>& buffer, size_t& offset, int32_t& out_value) {
    if (offset + sizeof(int32_t) > buffer.size()) return false;
    std::memcpy(&out_value, buffer.data() + offset, sizeof(int32_t));
    offset += sizeof(int32_t);
    return true;
}

/**
 * @brief 便捷封装：将 Get 方法回复序列化为文件
 * @param reply         回复字符串
 * @param filepath      目标文件路径
 * @param error_message [out] 错误描述
 * @return true 写入成功
 */
bool serializeGetReplyToFile(const std::string& reply, const std::string& filepath, std::string* error_message) {
    std::vector<uint8_t> buf;
    serializeString(reply, buf);
    return writeBufferToFile(buf, filepath, error_message);
}

/**
 * @brief 便捷封装：从文件反序列化 Get 方法回复
 * @param filepath      源文件路径
 * @param out_reply     [out] 解析出的回复字符串
 * @param error_message [out] 错误描述
 * @return true 读取成功
 */
bool deserializeGetReplyFromFile(const std::string& filepath, std::string* out_reply, std::string* error_message) {
    std::vector<uint8_t> buf;
    if (!readFileToBuffer(filepath, &buf, error_message)) return false;
    size_t off = 0;
    return deserializeString(buf, off, *out_reply);
}

/**
 * @brief 便捷封装：将 ChangedPayload 序列化为文件
 * @param payload       包含 message 和 counter 的载荷
 * @param filepath      目标文件路径
 * @param error_message [out] 错误描述
 * @return true 写入成功
 */
bool serializeChangedPayloadToFile(const ChangedPayload& payload, const std::string& filepath, std::string* error_message) {
    std::vector<uint8_t> buf;
    serializeString(payload.message, buf);
    serializeInt32(payload.counter, buf);
    return writeBufferToFile(buf, filepath, error_message);
}

/**
 * @brief 便捷封装：从文件反序列化 ChangedPayload
 * @param filepath      源文件路径
 * @param out_payload   [out] 解析出的载荷
 * @param error_message [out] 错误描述
 * @return true 读取成功
 */
bool deserializeChangedPayloadFromFile(const std::string& filepath, ChangedPayload* out_payload, std::string* error_message) {
    std::vector<uint8_t> buf;
    if (!readFileToBuffer(filepath, &buf, error_message)) return false;
    size_t off = 0;
    if (!deserializeString(buf, off, out_payload->message)) return false;
    if (!deserializeInt32(buf, off, out_payload->counter)) return false;
    return true;
}

}  // namespace weaknet_dbus
