/**
 * @file serializer.hpp
 * @brief 简单的序列化/反序列化工具
 *
 * 提供字符串、int32 的基础编解码，以及 Changed 信号和 Get 回复的文件持久化。
 * 设计为最小实现：无外部依赖、固定小端序、配合 D-Bus 信号的 text format 做旁路存储。
 *
 * 格式约定：
 *   字符串: [u32 len][bytes...]   无 null 终止符
 *   int32:  小端序 4 字节
 *   文件头: 'WNDS' magic + u32 version（防误读旧版本文件）
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weaknet_dbus {

/**
 * @brief Changed 信号的载荷结构（与 D-Bus 信号载荷同构）
 *
 * 序列化到文件时，方便另一个进程（如日志回放工具）读取并重建信号。
 */
struct ChangedPayload {
    std::string message;   ///< 信号文本消息
    int32_t counter;       ///< 计数器，单调递增，用于演示变化的次数
};

// ==================== 基础工具 ====================

/**
 * @brief 将内存缓冲区覆盖写入文件（原子语义：先写临时文件再 rename）
 *
 * 原子写入策略：write tmp → fsync tmp → rename tmp → filepath。
 * 避免进程中断时 filepath 处于半写入状态。
 *
 * @param buffer        要写入的数据
 * @param filepath      目标路径
 * @param error_message 可选：失败时填入错误描述
 * @return true 成功
 */
bool writeBufferToFile(const std::vector<uint8_t>& buffer, const std::string& filepath, std::string* error_message);

/// 从文件读出全部内容到缓冲区
bool readFileToBuffer(const std::string& filepath, std::vector<uint8_t>* buffer, std::string* error_message);

/**
 * @brief 将字符串编码为 [u32 len][bytes...] 格式追加到 out_buffer
 *
 * 字符串本身不带 null 终止符，由前缀 u32 指明长度。
 *
 * @param value      要编码的字符串
 * @param out_buffer 输出缓冲区（追加模式）
 */
void serializeString(const std::string& value, std::vector<uint8_t>& out_buffer);

/**
 * @brief 从缓冲区指定偏移处解码字符串，读取后推进 offset
 *
 * @param buffer    输入缓冲区
 * @param offset    输入（当前偏移）/输出（读取后偏移）
 * @param out_value 输出解码结果
 * @return true 成功；false 缓冲区长度不足或 offset 已到末尾
 */
bool deserializeString(const std::vector<uint8_t>& buffer, size_t& offset, std::string& out_value);

/**
 * @brief 小端序 int32 编解码
 *
 * 选择小端序是因为：x86/ARM 主流平台都是小端，网络传输也是小端。
 * 大端序平台移植时需要手动翻转字节序（当前项目只在 ARM64 运行，安全）。
 */
void serializeInt32(int32_t value, std::vector<uint8_t>& out_buffer);
bool deserializeInt32(const std::vector<uint8_t>& buffer, size_t& offset, int32_t& out_value);

// ==================== 高层封装 ====================

/**
 * @brief 将 Get 方法回复序列化到文件
 *
 * 文件格式: [magic 'WNDS'][u32 version=1][u32 len][string bytes]
 */
bool serializeGetReplyToFile(const std::string& reply, const std::string& filepath, std::string* error_message);
bool deserializeGetReplyFromFile(const std::string& filepath, std::string* out_reply, std::string* error_message);

/// Changed 信号的文件持久化版本（同 magic + version 头）
bool serializeChangedPayloadToFile(const ChangedPayload& payload, const std::string& filepath, std::string* error_message);
bool deserializeChangedPayloadFromFile(const std::string& filepath, ChangedPayload* out_payload, std::string* error_message);

}  // namespace weaknet_dbus
