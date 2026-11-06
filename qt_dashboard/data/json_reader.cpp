/**
 * @file json_reader.cpp
 * @brief JsonReader 实现
 */

#include "json_reader.hpp"

#include <cctype>
#include <cstdlib>

namespace weaknet {
namespace dashboard {

namespace {

/**
 * @brief 判断字符是否为 JSON 空白
 *
 * 只认 RFC 8259 定义的四种：空格、水平制表、换行、回车。
 * 不用 std::isspace，因为它还认垂直制表和换页，JSON 不允许。
 */
inline bool isJsonSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

JsonReader::JsonReader(const std::string& json) : json_(json) {}

// ---------------------------------------------------------------------------
// 基础扫描
// ---------------------------------------------------------------------------

size_t JsonReader::skipWhitespace(size_t pos) const {
    while (pos < json_.size() && isJsonSpace(json_[pos])) {
        ++pos;
    }
    return pos;
}

bool JsonReader::valid() const {
    size_t begin = skipWhitespace(0);
    if (begin >= json_.size()) {
        return false;
    }
    // 只接受对象或数组开头的 JSON
    if (json_[begin] != '{' && json_[begin] != '[') {
        return false;
    }
    // 末尾必须能闭合。不校验中间内容，那是 getXxx 的事。
    size_t end = json_.size();
    while (end > begin && isJsonSpace(json_[end - 1])) {
        --end;
    }
    if (end <= begin) {
        return false;
    }
    const char last = json_[end - 1];
    return last == '}' || last == ']';
}

size_t JsonReader::skipString(size_t pos) const {
    if (pos >= json_.size() || json_[pos] != '"') {
        return pos;
    }
    ++pos;
    while (pos < json_.size()) {
        if (json_[pos] == '\\') {
            // 跳过转义字符。\uXXXX 的六字符形式也一并跳过，
            // 这里不需要真的解码，只要不误判引号即可。
            pos += 2;
            continue;
        }
        if (json_[pos] == '"') {
            return pos + 1;
        }
        ++pos;
    }
    return json_.size();
}

size_t JsonReader::skipContainer(size_t pos) const {
    if (pos >= json_.size()) {
        return pos;
    }
    const char open = json_[pos];
    if (open != '{' && open != '[') {
        return pos;
    }
    const char close = (open == '{') ? '}' : ']';

    int depth = 0;
    while (pos < json_.size()) {
        const char c = json_[pos];
        if (c == '"') {
            pos = skipString(pos);
            continue;
        }
        if (c == open) {
            ++depth;
        } else if (c == close) {
            --depth;
            if (depth == 0) {
                return pos + 1;
            }
        }
        ++pos;
    }
    return json_.size();
}

size_t JsonReader::skipValue(size_t pos) const {
    pos = skipWhitespace(pos);
    if (pos >= json_.size()) {
        return pos;
    }
    const char c = json_[pos];
    if (c == '"') {
        return skipString(pos);
    }
    if (c == '{' || c == '[') {
        return skipContainer(pos);
    }
    // 数字 / true / false / null：一直吃到分隔符
    while (pos < json_.size()) {
        const char d = json_[pos];
        if (d == ',' || d == '}' || d == ']' || isJsonSpace(d)) {
            break;
        }
        ++pos;
    }
    return pos;
}

std::string JsonReader::parseStringAt(size_t pos, size_t* end_out) const {
    std::string out;
    if (pos >= json_.size() || json_[pos] != '"') {
        if (end_out) *end_out = pos;
        return out;
    }
    ++pos;
    while (pos < json_.size()) {
        const char c = json_[pos];
        if (c == '\\') {
            if (pos + 1 >= json_.size()) {
                break;
            }
            const char esc = json_[pos + 1];
            switch (esc) {
                case '"':  out.push_back('"');  pos += 2; break;
                case '\\': out.push_back('\\'); pos += 2; break;
                case '/':  out.push_back('/');  pos += 2; break;
                case 'b':  out.push_back('\b'); pos += 2; break;
                case 'f':  out.push_back('\f'); pos += 2; break;
                case 'n':  out.push_back('\n'); pos += 2; break;
                case 'r':  out.push_back('\r'); pos += 2; break;
                case 't':  out.push_back('\t'); pos += 2; break;
                case 'u': {
                    // \uXXXX：只处理 BMP 内的 ASCII 范围，其余替换为 '?'
                    // 看板展示的场景里，中文走 UTF-8 原样字节，不走 \u 转义
                    if (pos + 5 >= json_.size()) {
                        pos = json_.size();
                        break;
                    }
                    char hex[5] = {json_[pos + 2], json_[pos + 3],
                                   json_[pos + 4], json_[pos + 5], '\0'};
                    const unsigned long cp = std::strtoul(hex, nullptr, 16);
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else {
                        out.push_back('?');
                    }
                    pos += 6;
                    break;
                }
                default:
                    out.push_back(esc);
                    pos += 2;
                    break;
            }
            continue;
        }
        if (c == '"') {
            if (end_out) *end_out = pos + 1;
            return out;
        }
        out.push_back(c);
        ++pos;
    }
    if (end_out) *end_out = json_.size();
    return out;
}

std::vector<std::pair<size_t, size_t>>
JsonReader::splitTopLevel(size_t begin, size_t end) const {
    std::vector<std::pair<size_t, size_t>> parts;
    size_t pos = begin;
    size_t part_start = begin;
    while (pos < end) {
        const char c = json_[pos];
        if (c == '"') {
            pos = skipString(pos);
            continue;
        }
        if (c == '{' || c == '[') {
            pos = skipContainer(pos);
            continue;
        }
        if (c == ',') {
            parts.emplace_back(part_start, pos);
            part_start = pos + 1;
        }
        ++pos;
    }
    if (part_start < end) {
        parts.emplace_back(part_start, end);
    }
    return parts;
}

// ---------------------------------------------------------------------------
// 路径查找
// ---------------------------------------------------------------------------

size_t JsonReader::locate(const std::string& path) const {
    if (path.empty()) {
        return std::string::npos;
    }

    // 把 "a.b.c" 拆成 ["a","b","c"]
    std::vector<std::string> segments;
    {
        size_t start = 0;
        while (start <= path.size()) {
            const size_t dot = path.find('.', start);
            if (dot == std::string::npos) {
                segments.push_back(path.substr(start));
                break;
            }
            segments.push_back(path.substr(start, dot - start));
            start = dot + 1;
        }
    }

    // 从根开始逐层下钻
    size_t pos = skipWhitespace(0);
    if (pos >= json_.size() || json_[pos] != '{') {
        return std::string::npos;
    }
    size_t container_end = skipContainer(pos);
    ++pos;  // 跳过 '{'

    for (size_t seg_i = 0; seg_i < segments.size(); ++seg_i) {
        const std::string& key = segments[seg_i];
        bool found = false;

        while (pos < container_end) {
            pos = skipWhitespace(pos);
            if (pos >= container_end || json_[pos] == '}') {
                break;
            }
            if (json_[pos] != '"') {
                // 结构异常，放弃
                return std::string::npos;
            }

            size_t key_end = 0;
            const std::string cur_key = parseStringAt(pos, &key_end);
            pos = skipWhitespace(key_end);
            if (pos >= json_.size() || json_[pos] != ':') {
                return std::string::npos;
            }
            pos = skipWhitespace(pos + 1);

            if (cur_key == key) {
                if (seg_i + 1 == segments.size()) {
                    return pos;  // 命中最后一层
                }
                // 还有下一层：当前值必须是对象
                if (pos >= json_.size() || json_[pos] != '{') {
                    return std::string::npos;
                }
                container_end = skipContainer(pos);
                ++pos;
                found = true;
                break;
            }

            // 不是目标 key，跳过这个值继续找
            pos = skipValue(pos);
            pos = skipWhitespace(pos);
            if (pos < json_.size() && json_[pos] == ',') {
                ++pos;
            }
        }

        if (!found) {
            return std::string::npos;
        }
    }
    return std::string::npos;
}

// ---------------------------------------------------------------------------
// 取值
// ---------------------------------------------------------------------------

bool JsonReader::has(const std::string& path) const {
    const size_t pos = locate(path);
    if (pos == std::string::npos) {
        return false;
    }
    // 存在但值为 null 时视为不存在
    if (json_.compare(pos, 4, "null") == 0) {
        return false;
    }
    return true;
}

std::string JsonReader::getString(const std::string& path,
                                  const std::string& fallback) const {
    const size_t pos = locate(path);
    if (pos == std::string::npos || pos >= json_.size() || json_[pos] != '"') {
        return fallback;
    }
    size_t end = 0;
    return parseStringAt(pos, &end);
}

double JsonReader::getDouble(const std::string& path, double fallback) const {
    const size_t pos = locate(path);
    if (pos == std::string::npos || pos >= json_.size()) {
        return fallback;
    }
    const char c = json_[pos];
    if (!(c == '-' || c == '+' || (c >= '0' && c <= '9'))) {
        return fallback;
    }
    const char* begin = json_.c_str() + pos;
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin) {
        return fallback;
    }
    return value;
}

int JsonReader::getInt(const std::string& path, int fallback) const {
    const double v = getDouble(path, static_cast<double>(fallback));
    return static_cast<int>(v);
}

bool JsonReader::getBool(const std::string& path, bool fallback) const {
    const size_t pos = locate(path);
    if (pos == std::string::npos) {
        return fallback;
    }
    if (json_.compare(pos, 4, "true") == 0) {
        return true;
    }
    if (json_.compare(pos, 5, "false") == 0) {
        return false;
    }
    return fallback;
}

std::vector<std::string>
JsonReader::getStringArray(const std::string& path) const {
    std::vector<std::string> out;
    const size_t pos = locate(path);
    if (pos == std::string::npos || pos >= json_.size() || json_[pos] != '[') {
        return out;
    }
    const size_t end = skipContainer(pos);
    size_t cursor = pos + 1;
    while (cursor < end) {
        cursor = skipWhitespace(cursor);
        if (cursor >= end || json_[cursor] == ']') {
            break;
        }
        if (json_[cursor] == '"') {
            size_t str_end = 0;
            out.push_back(parseStringAt(cursor, &str_end));
            cursor = str_end;
        } else {
            cursor = skipValue(cursor);
        }
        cursor = skipWhitespace(cursor);
        if (cursor < end && json_[cursor] == ',') {
            ++cursor;
        }
    }
    return out;
}

std::vector<std::string>
JsonReader::getObjectArray(const std::string& path) const {
    std::vector<std::string> out;
    const size_t pos = locate(path);
    if (pos == std::string::npos || pos >= json_.size() || json_[pos] != '[') {
        return out;
    }
    const size_t end = skipContainer(pos);
    size_t cursor = pos + 1;
    while (cursor < end) {
        cursor = skipWhitespace(cursor);
        if (cursor >= end || json_[cursor] == ']') {
            break;
        }
        const size_t elem_begin = cursor;
        cursor = skipValue(cursor);
        out.push_back(json_.substr(elem_begin, cursor - elem_begin));
        cursor = skipWhitespace(cursor);
        if (cursor < end && json_[cursor] == ',') {
            ++cursor;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 顶层数组支持
// ---------------------------------------------------------------------------

std::vector<std::string> JsonReader::getRootObjectArray() const {
    std::vector<std::string> out;
    const size_t begin = skipWhitespace(0);
    if (begin >= json_.size() || json_[begin] != '[') {
        return out;
    }
    const size_t end = skipContainer(begin);
    size_t cursor = begin + 1;
    while (cursor < end) {
        cursor = skipWhitespace(cursor);
        if (cursor >= end || json_[cursor] == ']') {
            break;
        }
        const size_t elem_begin = cursor;
        cursor = skipValue(cursor);
        out.push_back(json_.substr(elem_begin, cursor - elem_begin));
        cursor = skipWhitespace(cursor);
        if (cursor < end && json_[cursor] == ',') {
            ++cursor;
        }
    }
    return out;
}

} // namespace dashboard
} // namespace weaknet
