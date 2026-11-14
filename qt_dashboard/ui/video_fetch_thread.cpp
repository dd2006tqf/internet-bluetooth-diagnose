/**
 * @file video_fetch_thread.cpp
 * @brief 后台取帧线程实现
 */

#include "video_fetch_thread.hpp"

#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include "weaknet_client.h"

namespace weaknet {
namespace dashboard {

namespace {
/// 取帧失败后放慢一点再试，避免服务没起来时把 CPU 打满
constexpr int kFailureBackoffMs = 500;

/// 连续失败多少次之后回到正常节奏（避免一次抖动就永久降速）
constexpr int kMaxBackoffRetries = 10;
}  // namespace

VideoFetchThread::VideoFetchThread(QObject* parent) : QThread(parent) {}

VideoFetchThread::~VideoFetchThread() { stop(); }

void VideoFetchThread::setIntervalMs(int ms) {
    // 下限 16ms（约 60fps）：比这更快只会让 D-Bus 排队，取回来的还是同一帧。
    // 上限 1000ms（1fps）：再慢就不像"实时画面"了。
    if (ms < 16) ms = 16;
    if (ms > 1000) ms = 1000;
    interval_ms_ = ms;
}

void VideoFetchThread::stop() {
    requestInterruption();
    // 等当前这一轮结束。最长等一个 D-Bus 超时（5s）+ 余量。
    if (!wait(10000)) {
        // 不等就 terminate 会留下持锁的线程，比起"退出慢一点"危险得多。
        // 这里保留一个明确的行为：不 terminate，让进程退出时自然收尾。
    }
}

void VideoFetchThread::run() {
    int consecutive_failures = 0;

    // ========================================================================
    // 自己保证库已初始化 —— 这是真机上抓到的一个缺陷
    // ========================================================================
    //
    // `weaknet_init()` 之前只由 DataFetchThread 在它自己的线程里调用。
    // 但本线程与 DataFetchThread 是两个**独立**的 QThread，谁先跑由调度决定。
    // 实测（2026-11-14 板端 --screenshot）：视频线程先跑，g_client 还是空的，
    // 于是每一轮都 `weaknet_get_video_frame` 失败，画面框永远停在占位态，
    // 而趋势数据（另一个线程）一切正常——看起来像"取帧功能没实现"。
    //
    // 所以这里不能假设别人初始化过，必须自己确保。`weaknet_init()` 是幂等的
    // 且内部有互斥，两个线程各调一次是安全的。
    //
    // 初始化失败要**重试而不是放弃**：板端服务由 systemd 管理，看板可能先于
    // 服务就绪。若这里 return，线程就死了，之后永远不会再有画面。
    while (!isInterruptionRequested()) {
        if (weaknet_init()) break;
        emit fetchFailed(QStringLiteral("无法连接服务端（稍后重试）"));
        msleep(static_cast<unsigned long>(interval_ms_));
    }

    while (!isInterruptionRequested()) {
        weaknet_video_frame f;
        char err[256] = {0};

        const bool call_ok = weaknet_get_video_frame(&f, err, sizeof(err));

        if (!call_ok) {
            // 调用本身失败：连接断了、服务端拒绝、或回复签名不符。
            // 这是真错误，界面上要说清楚，不能静默重试。
            emit fetchFailed(QString::fromUtf8(err));
            ++consecutive_failures;
        } else if (!f.valid) {
            // 无帧是正常形态（摄像头刚启动）。不报错，只告诉界面"暂无画面"。
            emit frameUnavailable();
            consecutive_failures = 0;
        } else {
            FetchedFrame out;
            out.width = f.width;
            out.height = f.height;
            out.stride = f.stride;
            out.uv_offset = f.uv_offset;
            out.fourcc = f.fourcc;
            out.sequence = f.sequence;
            out.timestamp_ns = f.timestamp_ns;
            out.valid = true;

            // mmap → 拷贝到 QByteArray → munmap。
            //
            // 为什么不把 mmap 的指针直接交给 UI 线程：那需要同时保证
            // munmap 发生在 UI 用完**之后**，而 UI 线程的节奏我们控制不了
            // （可能正在重绘、可能被系统挂起）。超时回收会把槽位抢走，
            // 指针就悬空了。一帧 3MB 的拷贝在 A55 上约 2~3ms，
            // 10fps 下完全可以接受，换来的是明确的所有权边界。
            void* p = ::mmap(nullptr, f.size, PROT_READ, MAP_SHARED, f.fd, 0);
            if (p == MAP_FAILED) {
                emit fetchFailed(QStringLiteral("mmap 失败: %1").arg(std::strerror(errno)));
                ++consecutive_failures;
            } else {
                out.nv12 = QByteArray(static_cast<const char*>(p),
                                      static_cast<int>(f.size));
                ::munmap(p, f.size);
                consecutive_failures = 0;
                emit frameReady(out);
            }
        }

        // fd 归我们：不管有没有帧、不管 mmap 成没成，都要关。
        // 漏关就是 fd 泄漏，中枢的槽位也永远不释放。
        if (f.fd >= 0) ::close(f.fd);

        // 归还槽位。注意顺序：**先 close fd 再 release**。
        // 反过来的话，服务端可能在 fd 还开着时就覆写这块内存——
        // 虽然我们此时已经不读它了，但让"仍有读者"的时间窗尽量短总没错。
        if (call_ok && f.valid) {
            char rel_err[128] = {0};
            // 归还失败不单独报错：最常见原因是服务端已因超时回收，
            // 此时下一轮取帧会拿到新的槽位，继续正常工作。
            weaknet_release_video_frame(f.slot, rel_err, sizeof(rel_err));
        }

        int sleep_ms = interval_ms_;
        if (consecutive_failures > 0 && consecutive_failures < kMaxBackoffRetries) {
            sleep_ms = kFailureBackoffMs;
        }
        msleep(static_cast<unsigned long>(sleep_ms));
    }
}

}  // namespace dashboard
}  // namespace weaknet
