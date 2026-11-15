/**
 * @file video_clip_muxer.cpp
 * @brief MP4 封装实现：memfd + fork/exec ffmpeg，不落盘、不重编码
 *
 * 三处实现层面的决策，都在下面各自的注释里展开：
 *   1. 用 memfd 而不是临时文件（不占闪存、不留残骸、无需清理）；
 *   2. 用 memfd 而不是管道（管道不可 seek，mp4 的 moov 写不进去；
 *      且 stderr 走管道有写满阻塞死锁的风险）；
 *   3. 帧率用**有理数**传给 ffmpeg（`-framerate 570000/30711`），
 *      使容器时长精确等于真实跨度，而不是被小数四舍五入带偏。
 */

#include "video_clip_muxer.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "logger.hpp"

namespace weaknet {
namespace {

// LogModule 是个**命名空间**（不是枚举），所以用命名空间别名而不是
// using-declaration ——后者只对命名空间内的名字有效，直接写 `using X::Y`
// 会被编译期拒绝（实测：`using-declaration may not name namespace`）。
namespace LogModule = weaknet_dbus::LogModule;

/// 子进程的等待上限。
///
/// 为什么必须有上限：封装跑在上传线程里，而上传线程是**唯一的**上传线程。
/// 一个卡死的 ffmpeg 会让整条留证上行永久停摆，且日志里只会留下一句
/// 「开始上传」再没有任何后续——那是最难归因的一种故障（看起来像网络卡住）。
/// 90 KB / 570 帧实测 0.26 s，30 s 已经给了百倍余量；超时即 SIGKILL 并报错。
constexpr int kMuxTimeoutMs = 30 * 1000;

/// ffmpeg stderr 只保留这么多个字节写进错误信息。
///
/// 不截断会让一条报错变成几十行日志（ffmpeg 在坏输入时会打印整段解码器上下文），
/// 而排障真正需要的通常只是第一句「Invalid data found」。
constexpr size_t kStderrKeepBytes = 512;

/// 读整个 fd 的内容（从当前偏移读到末尾后复位）。
///
/// 用于 memfd：它不是流，取大小再读是可靠的。返回空 vector 表示读失败或为空，
/// 两者在调用处都按「没有内容」处理——对 stderr 是「没有报错信息」，
/// 对输出是「没产出文件」，都是安全方向。
std::vector<uint8_t> readAllFromFd(int fd) {
    off_t size = ::lseek(fd, 0, SEEK_END);
    if (size <= 0) return {};
    if (::lseek(fd, 0, SEEK_SET) != 0) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    size_t got = 0;
    while (got < buf.size()) {
        const ssize_t n = ::read(fd, buf.data() + got, buf.size() - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    buf.resize(got);
    return buf;
}

/// 创建一块匿名内存文件并写入数据。
/// @return fd；失败返回 -1（`errno` 保留原因）
int createMemfdWithData(const char* name, const uint8_t* data, size_t size) {
    // MFD_CLOEXEC 是刻意的：子进程不需要继承这个 fd 编号，它通过
    // `/proc/self/fd/0` 这条**路径**重新打开同一块内存。继承原始编号会让
    // 「子进程手里有几个 fd」取决于父进程的开销顺序，难以排查。
    const int fd = ::memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) return -1;
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n <= 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return -1;
        }
        written += static_cast<size_t>(n);
    }
    if (::lseek(fd, 0, SEEK_SET) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// 推导传给 ffmpeg 的 `-framerate` 参数（有理数形式 `num/den`）。
///
/// 为什么用有理数而不是小数：ffmpeg 允许 `-framerate N/D`，于是
/// 「N = 帧数×1000、D = 跨度毫秒」这一对整数直接把容器时长钉成
/// `帧数 × 跨度 / (帧数×1000) = 跨度/1000` 秒，**与真实跨度逐毫秒相等**。
/// 换成小数（如 18.56）则会引入一个恒定的相对误差，570 帧的片段
/// 实测时长从 30.711 s 漂到 30.712 s——误差虽小，却没理由白吃。
///
/// 退化条件：帧数 < 2 或跨度为 0 时无从推导，回落到 25/1 并在**失败时**
/// 由错误信息说明；成功时容器照样可用，只是时间轴按名义帧率算。
std::string deriveFramerateRational(uint32_t frame_count, uint32_t span_ms) {
    if (frame_count < 2 || span_ms == 0) return "25/1";
    const uint64_t num = static_cast<uint64_t>(frame_count) * 1000ull;
    const uint64_t den = static_cast<uint64_t>(span_ms);
    return std::to_string(num) + "/" + std::to_string(den);
}

}  // namespace

bool looksLikeMp4(const uint8_t* data, size_t size) {
    // ISO/IEC 14496-12 的 `ftyp` box：前 4 字节是 box 长度，紧接着 4 字节是
    // box 类型。所以类型一定落在 [4, 8)。Annex-B 的起始码在最前 4 字节内
    // （`00 00 00 01` 或 `00 00 01`），不可能在这里拼出 ASCII `ftyp`。
    if (data == nullptr || size < 12) return false;
    return data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p';
}

VideoClipMuxResult muxAnnexBToMp4(const uint8_t* data, size_t size,
                                  uint32_t frame_count, uint32_t span_ms,
                                  const std::string& ffmpeg_path) {
    VideoClipMuxResult result;
    if (data == nullptr || size == 0) {
        result.error = "码流为空，无需封装";
        return result;
    }
    if (ffmpeg_path.empty()) {
        result.error = "ffmpeg 路径为空";
        return result;
    }

    // ---- 1. 备好三块匿名内存：输入、输出、stderr ----
    //
    // 输出必须是**可 seek** 的目标：mp4 的 `moov` box 要回填偏移量，
    // 而 `pipe:1` 被 ffmpeg 的 pipe 协议标记为不可 seek，会直接报
    // `muxer does not support non seekable output`（实测）。memfd 是
    // 有 inode 的普通文件语义，可 seek，且不落闪存。
    const int in_fd = createMemfdWithData("weaknet-mux-in", data, size);
    if (in_fd < 0) {
        result.error = std::string("输入 memfd 创建失败: ") + std::strerror(errno);
        return result;
    }
    const int out_fd = ::memfd_create("weaknet-mux-out", MFD_CLOEXEC);
    const int err_fd = ::memfd_create("weaknet-mux-err", MFD_CLOEXEC);
    if (out_fd < 0 || err_fd < 0) {
        result.error = std::string("输出 memfd 创建失败: ") + std::strerror(errno);
        if (out_fd >= 0) ::close(out_fd);
        if (err_fd >= 0) ::close(err_fd);
        ::close(in_fd);
        return result;
    }

    // 子进程会用 dup2 把这三个 fd 摆到 0/1/2 上，所以它们必须都不在 0..2。
    // memfd_create 返回最小可用 fd，而服务的 0/1/2 已被 systemd 占住，
    // 实际不会撞上；但把它当假设就等于让一个不可见的前置条件决定功能是否可用。
    if (in_fd < 3 || out_fd < 3 || err_fd < 3) {
        result.error = "memfd 拿到了 0..2 的编号，无法安全 dup2";
        ::close(in_fd);
        ::close(out_fd);
        ::close(err_fd);
        return result;
    }

    // ---- 2. 组装 argv（必须在 fork 之前，子进程里只能用异步信号安全函数）----
    //
    // 逐项说明几个不显然的参数：
    //   -f h264     显式指定**输入容器**。不指定时 ffmpeg 会去探测，
    //               而裸码流的探测在短片段上有猜错的可能；
    //   -nostdin    不让 ffmpeg 去读 stdin——它的 fd 0 已经是我们的输入了；
    //   -c copy     不重编码，只搬字节；
    //   +faststart  把 moov 挪到文件头，让浏览器/播放器可以边下边播。
    //               云端下载端点是一次性给全量，但运营端在浏览器里播放时
    //               仍需要 moov 在前才能立即开始，而不是等整段下完；
    //   -f mp4      显式指定**输出容器**。这条是实测踩出来的：输出路径是
    //               `/proc/self/fd/1`，没有扩展名，ffmpeg 无法据此推断格式，
    //               会报 `Unable to find a suitable output format`。
    const std::string rate = deriveFramerateRational(frame_count, span_ms);
    const std::string argv_list[] = {
        ffmpeg_path,
        "-hide_banner",
        "-v", "error",
        "-nostdin",
        "-y",
        "-framerate", rate,
        "-f", "h264",
        "-i", "/proc/self/fd/0",
        "-c", "copy",
        "-movflags", "+faststart",
        "-f", "mp4",
        "/proc/self/fd/1",
    };
    std::vector<char*> argv;
    argv.reserve(std::size(argv_list) + 1);
    // const_cast 在这里是安全的：execv 只读这些字符串，且它们生命周期覆盖到
    // 等待子进程结束之后（`argv_list` 是栈上 std::string，未越出本函数）。
    for (const std::string& s : argv_list) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    // ---- 3. fork + exec ----
    const pid_t pid = ::fork();
    if (pid < 0) {
        result.error = std::string("fork 失败: ") + std::strerror(errno);
        ::close(in_fd);
        ::close(out_fd);
        ::close(err_fd);
        return result;
    }

    if (pid == 0) {
        // 子进程：只允许异步信号安全调用（dup2 / execv / _exit 都在白名单内）。
        // 本进程是多线程的，fork 后继承的锁状态不可信，任何可能加锁的调用
        // （malloc、glog、std::string 构造）都可能死锁。
        ::dup2(in_fd, STDIN_FILENO);
        ::dup2(out_fd, STDOUT_FILENO);
        ::dup2(err_fd, STDERR_FILENO);
        ::execv(argv[0], argv.data());
        _exit(127);
    }

    // ---- 4. 父进程等待，带上限 ----
    int status = 0;
    bool timed_out = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kMuxTimeoutMs);
    for (;;) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            // ECHILD 表示本进程不持有该子进程（被别处回收或 SIGCHLD 被设成
            // SA_NOCLDWAIT）。此时无从判断退出码，只能靠产物的 ftyp 兜底——
            // 下面的校验会给出结论，所以这里不当成失败。
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);   // 收尸，避免留僵尸
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // ---- 5. 取产物与诊断 ----
    std::vector<uint8_t> out_bytes = readAllFromFd(out_fd);
    const std::vector<uint8_t> err_bytes = readAllFromFd(err_fd);
    ::close(in_fd);
    ::close(out_fd);
    ::close(err_fd);

    std::string stderr_text(reinterpret_cast<const char*>(err_bytes.data()),
                            err_bytes.size());
    if (stderr_text.size() > kStderrKeepBytes) stderr_text.resize(kStderrKeepBytes);

    if (timed_out) {
        result.error = "ffmpeg 超时 " + std::to_string(kMuxTimeoutMs) + "ms 被强制终止";
        if (!stderr_text.empty()) result.error += "；stderr=" + stderr_text;
        return result;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        // 127 是 execv 失败的约定码，也是最可能的部署问题：板端没装 ffmpeg。
        // 单独识别它，因为「没装」的处置是补依赖，「码流坏」的处置是查编码器。
        result.error = "无法执行 ffmpeg（" + ffmpeg_path + "）：未安装或不可执行";
        return result;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        result.error = "ffmpeg 退出码 " + std::to_string(WIFEXITED(status)
                                                       ? WEXITSTATUS(status)
                                                       : -1);
        if (!stderr_text.empty()) result.error += "：" + stderr_text;
        return result;
    }
    if (!looksLikeMp4(out_bytes.data(), out_bytes.size())) {
        // 退出码 0 却没有 ftyp：不做这个校验就会把一段不是 MP4 的字节
        // 当成 MP4 上传，云端存下来、运营点开播不了，而日志全是绿的。
        result.error = "ffmpeg 声称成功，但产物不是 MP4（无 ftyp box）";
        return result;
    }

    LOG_INFO(LogModule::VIDEO,
             "视频留证封装完成: in=" << size << "B out=" << out_bytes.size()
             << "B frames=" << frame_count << " span=" << span_ms
             << "ms rate=" << rate);
    result.ok = true;
    result.bytes = std::move(out_bytes);
    return result;
}

}  // namespace weaknet
