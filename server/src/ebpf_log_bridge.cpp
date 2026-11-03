/**
 * @file ebpf_log_bridge.cpp
 * @brief libbpf 打印回调 → 项目日志 的桥接实现
 *
 * 关键实现细节（都不是可选的）：
 *
 * 1. **必须按行拆开**。libbpf 把整段 BPF 校验器日志放在**一次** print 调用里
 *    （`"libbpf: prog 'x': -- BEGIN PROG LOAD LOG --\n%s-- END PROG LOAD LOG --\n"`），
 *    单次输出动辄上千行、上百 KB。直接当一行处理会被截断成垃圾。
 *
 * 2. **必须做两遍 vsnprintf**。校验器日志远超任何栈缓冲区，
 *    第一遍只为拿到所需长度，第二遍才真正格式化（va_list 用 va_copy 复制）。
 *
 * 3. **分级**。以 `libbpf: ` 开头的行是 libbpf 自己的结论（`failed to load: -13`、
 *    `map 'x': failed to create` 等），按回调级别进入 WARNING/INFO；
 *    其余行是校验器原文，只在 debug 级别输出，否则会把 journald 冲掉。
 *
 * 4. **缓存只留结论行**。生产环境不开 debug 时，失败处靠 takeLibbpfError()
 *    把 `libbpf: ...` 结论拼进自己的失败日志 —— 这就是"根因不丢失"的落点。
 *    缓存为 thread_local：open/load 是同步调用，失败原因必然属于当前线程，
 *    全局缓存会在多监控器并发初始化时互相覆盖。
 */

#include "ebpf_log_bridge.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "logger.hpp"

#if defined(__has_include)
#  if __has_include(<bpf/libbpf.h>)
#    define WEAKNET_HAVE_LIBBPF_HEADER 1
extern "C" {
#include <bpf/libbpf.h>
}
#  else
#    define WEAKNET_HAVE_LIBBPF_HEADER 0
#  endif
#else
#  define WEAKNET_HAVE_LIBBPF_HEADER 0
#endif

namespace weaknet_dbus {

namespace {

/// 缓存行数上限：足够覆盖"libbpf 的若干条结论"，又不会淹没日志
constexpr std::size_t kKeepLines = 8;

/// 单行截断长度：map dump 之类的行可能非常长
constexpr std::size_t kMaxLineChars = 220;

/// 单次 print 调用的输出上限（字节）。超过部分丢弃并标注，防止异常情况下吃光内存。
constexpr std::size_t kMaxCallChars = 256u * 1024u;

/// 单次 print 调用中最多转发多少行校验器原文（仅在 debug 打开时才逐行输出）
constexpr std::size_t kMaxVerifierLines = 512;

/// libbpf 自身消息的前缀（校验器原文没有这个前缀）
constexpr const char* kLibbpfPrefix = "libbpf:";
constexpr std::size_t kLibbpfPrefixLen = 7;

/// 当前线程缓存的 libbpf 结论行（已去掉 `libbpf: ` 前缀与空白）
thread_local std::vector<std::string> t_recent_lines;

/// 去掉多余空白并限制长度（用于单行净化）
std::string squeeze(const char* text, std::size_t len) {
    std::string out;
    out.reserve(len < kMaxLineChars ? len : kMaxLineChars);
    bool prev_space = false;
    for (std::size_t i = 0; i < len; ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!out.empty() && !prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
            continue;
        }
        out.push_back(c);
        prev_space = false;
        if (out.size() >= kMaxLineChars) {
            out.append("...");
            break;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

void remember(const std::string& line) {
    if (line.empty()) return;
    t_recent_lines.push_back(line);
    if (t_recent_lines.size() > kKeepLines) {
        t_recent_lines.erase(
            t_recent_lines.begin(),
            t_recent_lines.begin() + static_cast<long>(t_recent_lines.size() - kKeepLines));
    }
}

#if WEAKNET_HAVE_LIBBPF_HEADER

/// 把一整段（可能含换行的）libbpf 输出按行分流到日志与缓存
void emitLines(enum libbpf_print_level level, const char* text, std::size_t len) {
    std::size_t verifier_lines = 0;
    std::size_t skipped = 0;

    std::size_t begin = 0;
    while (begin <= len) {
        std::size_t end = begin;
        while (end < len && text[end] != '\n') ++end;

        const std::string line = squeeze(text + begin, end - begin);
        if (!line.empty()) {
            const bool is_libbpf_msg = line.compare(0, kLibbpfPrefixLen, kLibbpfPrefix) == 0;
            if (is_libbpf_msg) {
                // libbpf 自己的结论：这是根因所在，既进日志也进缓存
                remember(squeeze(line.c_str() + kLibbpfPrefixLen,
                                 line.size() - kLibbpfPrefixLen));
                switch (level) {
                    case LIBBPF_WARN:
                        LOG_WARNING(LogModule::EBPF, line);
                        break;
                    case LIBBPF_INFO:
                        LOG_INFO(LogModule::EBPF, line);
                        break;
                    case LIBBPF_DEBUG:
                        LOG_DEBUG(LogModule::EBPF, line);
                        break;
                    default:
                        LOG_INFO(LogModule::EBPF, line);
                        break;
                }
            } else if (++verifier_lines <= kMaxVerifierLines) {
                // 校验器原文：只在 debug 打开时可见，避免正常运行时刷屏
                LOG_DEBUG(LogModule::EBPF, "verifier: " << line);
            } else {
                ++skipped;
            }
        }

        if (end >= len) break;
        begin = end + 1;
    }

    if (skipped > 0) {
        LOG_DEBUG(LogModule::EBPF, "verifier: (" << skipped << " more lines suppressed)");
    }
}

int libbpfLogCallback(enum libbpf_print_level level, const char* fmt, va_list args) {
    // 第一遍：只为拿长度。libbpf 的整段校验器日志远超任何栈缓冲区。
    va_list probe;
    va_copy(probe, args);
    const int needed = std::vsnprintf(nullptr, 0, fmt, probe);
    va_end(probe);
    if (needed <= 0) return needed;

    const std::size_t cap =
        (static_cast<std::size_t>(needed) < kMaxCallChars) ? static_cast<std::size_t>(needed)
                                                          : kMaxCallChars;

    std::string buf(cap + 1, '\0');
    va_list render;
    va_copy(render, args);
    const int written = std::vsnprintf(buf.data(), buf.size(), fmt, render);
    va_end(render);
    if (written < 0) return written;

    emitLines(level, buf.c_str(), buf.size() - 1);
    return needed;
}

#endif  // WEAKNET_HAVE_LIBBPF_HEADER

std::once_flag g_install_once;

}  // namespace

void installLibbpfLogBridge() {
#if WEAKNET_HAVE_LIBBPF_HEADER
    std::call_once(g_install_once, []() {
        // 返回旧回调（默认打 stderr），丢弃即可：我们要独占这条通道，
        // 否则每段校验器日志会同时出现在 stderr 与项目日志里，成倍放大。
        libbpf_set_print(&libbpfLogCallback);
        LOG_DEBUG(LogModule::EBPF, "libbpf print bridge installed");
    });
#endif
}

std::string takeLibbpfError() {
    if (t_recent_lines.empty()) return {};

    std::string joined = " (libbpf: ";
    for (std::size_t i = 0; i < t_recent_lines.size(); ++i) {
        if (i) joined += " | ";
        joined += t_recent_lines[i];
    }
    joined += ")";

    t_recent_lines.clear();
    return joined;
}

void clearLibbpfError() {
    t_recent_lines.clear();
}

}  // namespace weaknet_dbus
