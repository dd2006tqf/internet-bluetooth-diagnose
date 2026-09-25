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
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
 * @brief 确保私有目录存在，权限设为 0700（仅 owner 可读写）
 * @param dir 允许的私有目录（$XDG_RUNTIME_DIR/weaknet 或 /tmp/weaknet）
 *
 * $XDG_RUNTIME_DIR 或 /tmp 通常已存在，仅需创建 weaknet/ 子目录；
 * 目录已存在时 mkdir 返回 EEXIST，视为成功。
 */
static void ensureParentDir(const std::string& dir) {
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        LOG_ERROR(LogModule::WEAK_MGR, "ensureParentDir: mkdir " << dir << " failed: " << strerror(errno));
    }
}

/**
 * @brief 检查路径安全性：仅允许 $XDG_RUNTIME_DIR/weaknet/ 或 /tmp/weaknet/ 下的直接子文件
 * @param filepath 待检查路径
 * @return true 文件的父目录恰好是允许的私有目录；false 路径不安全
 *
 * 父目录必须与允许目录逐字节相等（不做前缀匹配，防止 /tmp/weaknet_evil/ 这类
 * 相似前缀绕过），且文件名部分不得再包含 '/'（不允许更深嵌套）。
 */
static bool isSafePath(const std::string& filepath) {
    const size_t pos = filepath.find_last_of('/');
    if (pos == std::string::npos || pos == 0) return false;
    if (filepath.find('/', pos + 1) != std::string::npos) return false;
    const std::string dir = filepath.substr(0, pos);
    if (dir == "/tmp/weaknet") return true;
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg && dir == std::string(xdg) + "/weaknet") return true;
    return false;
}

/**
 * @brief 以"不跟随符号链接"的方式打开安全路径下的文件（只读）
 * @param filepath      完整文件路径（须通过 isSafePath 校验）
 * @param error_message [out] 失败原因（可传 nullptr）
 * @return fd 成功；-1 路径不安全或打开失败（含符号链接攻击场景）
 *
 * 加固点：
 *   - 目录用 open(O_DIRECTORY|O_NOFOLLOW) 打开 —— 私有目录本身被替换为符号链接时直接失败；
 *   - 文件用 openat(...O_NOFOLLOW) 打开 —— 目标是符号链接（含悬空链接）时返回 ELOOP；
 *   - 打开与读写之间不存在"检查后再打开"的窗口：openat 相对 dirfd 定位，攻击者无法
 *     通过替换父目录组件把读取重定向到别的目录。
 *
 * 说明：写入路径不共用本函数。writeBufferToFile 需要 tmp+fsync+rename 三步，
 * 本函数的单次 openat 无法表达，因此写入已独立实现。此前这里保留的
 * forWrite=true 分支（O_CREAT|O_TRUNC 直写目标）在没有调用者之后成为死代码，
 * 且正是"非原子写"缺陷的来源，已删除。
 */
static int openSafeFile(const std::string& filepath, std::string* error_message) {
    if (!isSafePath(filepath)) {
        if (error_message) *error_message = "路径不安全，必须直接位于 $XDG_RUNTIME_DIR/weaknet/ 或 /tmp/weaknet/ 下: " + filepath;
        return -1;
    }
    const size_t pos = filepath.find_last_of('/');
    const std::string dir = filepath.substr(0, pos);
    const std::string name = filepath.substr(pos + 1);

    const int dirfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) {
        LOG_ERROR(LogModule::WEAK_MGR, "openSafeFile: open dir " << dir << " failed: " << strerror(errno));
        if (error_message) *error_message = "无法安全打开私有目录: " + dir + " (" + strerror(errno) + ")";
        return -1;
    }
    const int fd = ::openat(dirfd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ::close(dirfd);
    if (fd < 0) {
        const std::string reason = (errno == ELOOP) ? "路径包含符号链接（已拒绝）" : strerror(errno);
        LOG_ERROR(LogModule::WEAK_MGR, "openSafeFile: open " << filepath << " failed: " << reason);
        if (error_message) *error_message = "无法安全打开文件: " + filepath + " (" + reason + ")";
    }
    return fd;
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
        if (error_message) *error_message = "路径不安全，必须直接位于 $XDG_RUNTIME_DIR/weaknet/ 或 /tmp/weaknet/ 下: " + filepath;
        return false;
    }

    const size_t pos = filepath.find_last_of('/');
    const std::string dir = filepath.substr(0, pos);
    const std::string name = filepath.substr(pos + 1);
    ensureParentDir(dir);

    const int dirfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) {
        LOG_ERROR(LogModule::WEAK_MGR, "writeBufferToFile: open dir " << dir << " failed: " << strerror(errno));
        if (error_message) *error_message = "无法安全打开私有目录: " + dir + " (" + strerror(errno) + ")";
        return false;
    }

    // 原子写：写临时文件 → fsync → renameat 覆盖目标。
    //
    // 此前实现是 O_CREAT|O_TRUNC 直写目标文件，进程在写入中途被终止就会留下
    // 半截文件，而头文件一直宣称"先写临时文件再 rename"。这里补上真实实现。
    //
    // 临时名带 pid，避免同一进程的并发写互相踩；O_EXCL 保证不与残留文件混用。
    // renameat 的目标若是指向别处的符号链接，替换的是链接本身而非其指向的
    // 文件（不会写入 /etc/passwd 之类），比原先 O_NOFOLLOW 直接报 ELOOP 更宽容，
    // 但同样不会越权写入。
    const std::string tmpname = name + ".tmp." + std::to_string(::getpid());
    const int fd = ::openat(dirfd, tmpname.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        const std::string reason = (errno == ELOOP) ? "路径包含符号链接（已拒绝）" : strerror(errno);
        LOG_ERROR(LogModule::WEAK_MGR, "writeBufferToFile: open temp " << tmpname << " failed: " << reason);
        if (error_message) *error_message = "无法创建临时文件: " + tmpname + " (" + reason + ")";
        ::close(dirfd);
        return false;
    }

    size_t off = 0;
    bool ok = true;
    while (off < buffer.size()) {
        const ssize_t n = ::write(fd, buffer.data() + off, buffer.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        off += static_cast<size_t>(n);
    }
    // 先落盘再 rename：否则崩溃后可能出现"rename 完成但数据未落盘"的空文件。
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;

    if (ok && ::renameat(dirfd, tmpname.c_str(), dirfd, name.c_str()) != 0) {
        LOG_ERROR(LogModule::WEAK_MGR, "writeBufferToFile: renameat failed: " << strerror(errno));
        ok = false;
    }
    if (!ok) {
        // 失败时清掉临时文件，不留残渣（目录项本身的持久化不在此保证范围）。
        ::unlinkat(dirfd, tmpname.c_str(), 0);
        ::close(dirfd);
        LOG_ERROR(LogModule::WEAK_MGR, "writeBufferToFile: write failed for " << filepath);
        if (error_message) *error_message = "写入失败: " + filepath;
        return false;
    }
    // 目录 fsync：让 rename 产生的目录项变更也落盘（否则掉电后可能回退到旧内容）。
    ::fsync(dirfd);
    ::close(dirfd);
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
    const int fd = openSafeFile(filepath, error_message);
    if (fd < 0) return false;

    // 循环读至 EOF（不信任文件预读大小，处理读取中途文件变化/部分读）
    std::vector<uint8_t> data;
    uint8_t chunk[8192];
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (n == 0) break;
        data.insert(data.end(), chunk, chunk + n);
    }
    ::close(fd);

    if (!ok) {
        if (error_message) *error_message = "读取失败: " + filepath;
        return false;
    }
    *buffer = std::move(data);
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
