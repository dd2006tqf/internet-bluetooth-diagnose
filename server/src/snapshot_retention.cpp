/**
 * @file snapshot_retention.cpp
 * @brief 板端静帧目录回收：按总字节上限，从最旧开始删
 *
 * 设计取舍见头文件。这里只补三个实现层面的决定：
 *
 * 1. **不递归**：落盘布局由 `SnapshotService` 固定为两层，只扫这一层。
 * 2. **`.jpg.tmp` 也算**：原子写崩在 rename 之前会留下半成品，它比正式文件
 *    更该被清掉（它永远不会被改回正式文件——rename 只有写入线程会做，而那次
 *    写入已经失败了）。
 * 3. **空目录顺带删**：事故目录是整个拷贝的单位，文件删空后留着它只会让
 *    目录数无限增长，而 `du` 看起来"还好"。
 */

#include "snapshot_retention.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace weaknet {
namespace {

/// 文件名形如 `<ms>_<seq>.jpg` 或 `<ms>_<seq>.jpg.tmp`。
///
/// 返回毫秒前缀（解析失败为 0）。**不**校验整个文件名：写入端是本进程，
/// 但回收端不该假设目录里只有自己写的东西——任何解析失败的文件都排在
/// 最前面（ms=0），宁可先删来历不明的东西。
uint64_t parseNameMs(const std::string& name) {
    uint64_t value = 0;
    size_t digits = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c < '0' || c > '9') break;
        // 溢出保护：墙钟毫秒是 13 位，超过 20 位一定不是我们的文件。
        if (digits >= 20) return 0;
        value = value * 10 + static_cast<uint64_t>(c - '0');
        ++digits;
    }
    if (digits == 0) return 0;
    // 必须是 `<数字>_` 或 `<数字>.`；纯粹一串数字（比如目录名）不算。
    if (digits >= name.size()) return 0;
    const char next = name[digits];
    if (next != '_' && next != '.') return 0;
    return value;
}

bool endsWith(const std::string& value, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (value.size() < n) return false;
    return value.compare(value.size() - n, n, suffix) == 0;
}

bool isSnapshotFile(const std::string& name) {
    return endsWith(name, ".jpg") || endsWith(name, ".jpg.tmp");
}

/// 目录是否为空（忽略 . 和 ..）。用于删完文件后清理空目录。
bool dirIsEmpty(const std::string& path) {
    DIR* d = ::opendir(path.c_str());
    if (!d) return false;
    bool empty = true;
    struct dirent* e = nullptr;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        empty = false;
        break;
    }
    ::closedir(d);
    return empty;
}

}  // namespace

bool scanSnapshotFiles(const std::string& root, std::vector<SnapshotFileEntry>* out) {
    if (out == nullptr) return false;
    out->clear();
    if (root.empty()) return true;

    DIR* d = ::opendir(root.c_str());
    if (!d) {
        // 目录不存在不是错误：还没拍过任何一张。
        return errno == ENOENT;
    }

    struct dirent* e = nullptr;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string incident = e->d_name;
        if (incident == "." || incident == "..") continue;

        const std::string dir = root + "/" + incident;
        struct stat st_dir;
        if (::stat(dir.c_str(), &st_dir) != 0 || !S_ISDIR(st_dir.st_mode)) {
            // 根目录下的非目录条目不是我们写的（或是一次失败留下的 tmp）。
            // 不删它：本模块只负责自己那块盘的占用，不清理别人的东西。
            continue;
        }

        DIR* sub = ::opendir(dir.c_str());
        if (!sub) continue;
        struct dirent* se = nullptr;
        while ((se = ::readdir(sub)) != nullptr) {
            const std::string name = se->d_name;
            if (name == "." || name == "..") continue;
            if (!isSnapshotFile(name)) continue;

            const std::string path = dir + "/" + name;
            struct stat st;
            if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

            SnapshotFileEntry entry;
            entry.path = path;
            entry.incident = incident;
            entry.size_bytes = static_cast<uint64_t>(st.st_size);
            entry.name_ms = parseNameMs(name);
            out->push_back(std::move(entry));
        }
        ::closedir(sub);
    }
    ::closedir(d);

    // 显式排序，不依赖 readdir 顺序。同一毫秒内按路径字典序，
    // 保证同一输入在任何板子上得到同一个删除顺序。
    std::sort(out->begin(), out->end(),
              [](const SnapshotFileEntry& a, const SnapshotFileEntry& b) {
                  if (a.name_ms != b.name_ms) return a.name_ms < b.name_ms;
                  return a.path < b.path;
              });
    return true;
}

size_t pruneSnapshotDir(const std::string& root,
                        const SnapshotRetentionConfig& config,
                        const std::string& keep_path,
                        SnapshotRetentionResult* result) {
    SnapshotRetentionResult local;
    SnapshotRetentionResult* r = result != nullptr ? result : &local;
    *r = SnapshotRetentionResult{};

    // 0 = 不回收。默认关闭，理由见头文件：没人复核过的数字不该删除证据。
    if (config.max_total_bytes == 0) return 0;
    if (root.empty()) return 0;

    std::vector<SnapshotFileEntry> files;
    if (!scanSnapshotFiles(root, &files)) {
        fprintf(stderr, "[snapshot-retention] cannot scan %s: %s\n",
                root.c_str(), std::strerror(errno));
        return 0;
    }

    uint64_t total = 0;
    for (const auto& f : files) total += f.size_bytes;
    r->bytes_before = total;

    if (total <= config.max_total_bytes) {
        r->bytes_after = total;
        return 0;
    }

    // 从最旧的开始删，直到「剩下的」装得下。
    // keep_path 一定跳过：它可能是唯一一张图，而删掉它等于「拍了立刻销毁」。
    const size_t budget = config.max_files_per_run == 0 ? files.size()
                                                        : config.max_files_per_run;
    size_t deleted = 0;
    uint64_t freed = 0;
    std::vector<std::string> touched_dirs;

    for (const auto& f : files) {
        if (total - freed <= config.max_total_bytes) break;
        if (deleted >= budget) break;
        if (!keep_path.empty() && f.path == keep_path) continue;

        if (::unlink(f.path.c_str()) != 0) {
            // 删不掉只记数：权限/忙都可能，下一轮还会看到它。
            ++r->delete_failures;
            fprintf(stderr, "[snapshot-retention] unlink %s failed: %s\n",
                    f.path.c_str(), std::strerror(errno));
            continue;
        }
        ++deleted;
        freed += f.size_bytes;
        touched_dirs.push_back(f.path.substr(0, f.path.rfind('/')));
    }

    // 顺带清掉变空的事故目录，避免目录数无限增长。失败无所谓——
    // 空目录不占空间，只是让 ls 难看。
    std::sort(touched_dirs.begin(), touched_dirs.end());
    touched_dirs.erase(std::unique(touched_dirs.begin(), touched_dirs.end()),
                       touched_dirs.end());
    for (const auto& dir : touched_dirs) {
        if (dir.empty()) continue;
        if (dirIsEmpty(dir) && ::rmdir(dir.c_str()) == 0) {
            ++r->dirs_removed;
        }
    }

    r->bytes_after = total - freed;
    r->files_deleted = deleted;
    r->bytes_freed = freed;
    // 「还超」不等于失败：可能是 keep_path 撑着（上限比一张图还小），
    // 也可能是本次预算用完（剩下的下一轮再删）。两种情况都靠日志让人看见。
    r->still_over_limit = r->bytes_after > config.max_total_bytes;
    if (r->still_over_limit) {
        fprintf(stderr,
                "[snapshot-retention] still over limit after pruning: "
                "root=%s limit=%llu after=%llu deleted=%zu failures=%zu\n",
                root.c_str(),
                static_cast<unsigned long long>(config.max_total_bytes),
                static_cast<unsigned long long>(r->bytes_after),
                deleted,
                r->delete_failures);
    }
    return deleted;
}

}  // namespace weaknet
