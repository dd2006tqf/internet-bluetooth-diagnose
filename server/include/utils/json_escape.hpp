/**
 * @file utils/json_escape.hpp
 * @brief JSON 字符串转义工具函数
 *
 * 将任意 C++ 字符串转换为合法的 JSON 字符串值，
 * 处理 \、"、\n、\r、\t、以及 U+0000 ~ U+001F 控制字符。
 *
 * 实现参考：RFC 8259 §7. Strings
 *   - " → \"
 *   - \ → \\
 *   - 退格 → \b   (U+0008)
 *   - 换页 → \f   (U+000C)
 *   - 换行 → \n   (U+000A)
 *   - 回车 → \r   (U+000D)
 *   - 制表 → \t   (U+0009)
 *   - 其他控制字符 → \uXXXX（4 位十六进制）
 *
 * @note 本函数只做转义，不添加外层引号；调用方应自行拼接：
 *       json_str = "\"" + escapeJsonString(raw) + "\"";
 *
 * @note 实现与 database_manager.cpp 和 net_info.cpp 中的内嵌转义逻辑一致，
 *       统一到此处便于复用和测试。
 */

#pragma once

#include <string>

namespace weaknet_utils {

/**
 * @brief JSON 字符串转义
 *
 * 纯函数，无副作用，线程安全。
 * 输入字符串按字节处理；假设输入是 UTF-8 编码，多字节 UTF-8 字符的
 * 高字节（>= 0x80）不做转义，原样保留（UTF-8 自身已处理）。
 *
 * @param s  原始字符串（可以是任意内容，包括二进制）
 * @return   转义后的字符串（可安全嵌入 JSON 双引号内）
 */
std::string escapeJsonString(const std::string& s);

}  // namespace weaknet_utils
