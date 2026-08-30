/**
 * @file json_escape.cpp
 * @brief JSON 字符串转义工具函数实现
 *
 * 本文件提供 escapeJsonString() 函数，将任意 C++ 字符串按 RFC 8259
 * (JSON) 规范进行转义，使得结果可以安全嵌入 JSON 字符串字面量中。
 *
 * 转义规则（摘自 RFC 8259, Section 7）：
 *   - '"'  → \"    双引号
 *   - '\\' → \\    反斜杠
 *   - '\b' → \b    退格 (U+0008)
 *   - '\f' → \f    换页 (U+000C)
 *   - '\n' → \n    换行 (U+000A)
 *   - '\r' → \r    回车 (U+000D)
 *   - '\t' → \t    水平制表 (U+0009)
 *   - 其他 < 0x20 的控制字符 → \uXXXX 形式
 *   - ≥ 0x20 的非特殊字符 → 原样保留（包括 UTF-8 多字节序列）
 *
 * 线程安全：纯函数，无共享状态，可在任意线程安全调用
 */

#include "utils/json_escape.hpp"
#include <cstdio>

namespace weaknet_utils {

/**
 * @brief 将字符串按 JSON 规范转义，返回可直接嵌入 JSON 字面量的新字符串
 * @param s 待转义的原始字符串（可以含任意字节，包括 UTF-8 和控制字符）
 * @return 转义后的字符串；调用方需自行在外层加双引号包裹
 *
 * 性能：out.reserve(s.size() + 2) 预留了"最坏情况"（所有字符都需要转义，
 * 每字符最多 6 字节 \uXXXX）的保守上界，但实际 UTF-8 场景下大部分字符
 * 无需转义，reserve 开销远低于逐个 rehash 的代价。
 */
std::string escapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;   // 双引号必须转义
            case '\\': out += "\\\\"; break;   // 反斜杠必须转义
            case '\b': out += "\\b";  break;   // 退格
            case '\f': out += "\\f";  break;   // 换页
            case '\n': out += "\\n";  break;   // 换行
            case '\r': out += "\\r";  break;   // 回车
            case '\t': out += "\\t";  break;   // 水平制表
            default:
                // 所有 ASCII 控制字符（0x00 ~ 0x1F）统一用 \uXXXX 形式转义
                // 这样处理也天然兼容 UTF-8 多字节序列：高字节 ≥ 0x80 时不会误触发分支
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;   // 其他所有字符（包括可打印 ASCII 和 UTF-8 多字节）原样保留
                }
                break;
        }
    }
    return out;
}

}  // namespace weaknet_utils
