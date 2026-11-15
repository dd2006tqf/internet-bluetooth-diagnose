/**
 * @file board_capture_pixel_sink_check.cpp
 * @brief 一次性探针：分辨「DQBUF 不出帧」与「pixel_sink 没接到」两条路径
 *
 * ============================================================================
 * 它解决什么问题
 * ============================================================================
 *
 * 板端生产进程出现了自相矛盾的现象：
 *
 *   - `video qoe attribution: APP_LAYER_SUSPECT frames_bad=yes` 每 10 秒稳定
 *     输出 —— 说明 `frame_source->snapshotAndReset()` 返回了 `valid()==true`
 *     且帧数不为零；
 *   - 而 `video frame hub configured` 在**全时段 journal 里 0 次** —— 那行
 *     日志在 `pixel_sink` 回调的首次执行路径里。
 *
 * 两者只能同时成立于一种情况：**帧从 V4L2 出得来，但没有被交给 pixel_sink**。
 * 也可能是日志级别把它滤掉了（`log_level: info`，而这行确实是 INFO）。
 *
 * 猜这两种可能没有意义，因为后续所有方案都取决于结论：
 *
 *   - 若 sink 没被调用 → 问题在本仓库的像素分发逻辑，要改代码；
 *   - 若 sink 被调用、只是日志没进 journal → 分发正常，问题在日志/副作用，
 *     视频链路其实是通的，方案完全不同。
 *
 * 所以本探针**不猜**：它同时统计两条路径，在同一进程里比对。
 *
 * ============================================================================
 * 为什么必须复制一份产品代码跑，而不是直接读生产进程
 * ============================================================================
 *
 * 生产进程不暴露这两个计数（hub 的 skipped 计数没有日志出口，帧源也没有
 * 「sink 被调用过几次」这个字段）。而 /dev/video0 被生产进程独占，探针无法
 * 同时打开——所以必须**先停生产服务再跑**，这是本探针唯一的侵入性。
 *
 * 换来的是一次性拿到确定事实：DQBUF 计数、sink 调用计数、首帧几何、
 * 以及每帧之间的间隔。四条数据放在一起，上面两种可能立刻二选一。
 *
 * ============================================================================
 * 用法
 * ============================================================================
 *
 *   # 必须先停生产服务：/dev/video0 是独占的
 *   sudo systemctl stop weaknet-server
 *   sudo ./board_capture_pixel_sink_check --seconds 10
 *   sudo systemctl start weaknet-server
 *
 * 退出码：0 = 两条路径都有数据（分发正常）；1 = 帧出得来但 sink 没被调用，
 *         或采集侧一帧都没出（两者都判 1，但**结论文字不同**——前者是分发
 *         故障、后者是采集故障，改错地方等于白改）；2 = 帧源从未采样 / start 失败。
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <getopt.h>

#include "v4l2_frame_source.hpp"

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop.store(true); }

}  // namespace

int main(int argc, char** argv) {
    uint32_t seconds = 10;

    static const option kLongOpts[] = {
        {"seconds", required_argument, nullptr, 's'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int ch = 0;
    while ((ch = getopt_long(argc, argv, "s:h", kLongOpts, nullptr)) != -1) {
        switch (ch) {
            case 's': seconds = static_cast<uint32_t>(std::atoi(optarg)); break;
            case 'h':
                std::printf("用法: %s [--seconds N]\n", argv[0]);
                return 0;
            default:
                return 2;
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::printf("=== 像素分发路径核查 ===\n");

    // 与生产完全相同的配置：默认构造 = 1640x1232。
    // **不**在这里改几何——探针要回答的是「生产配置下会怎样」。
    weaknet::V4l2CaptureConfig cfg;
    weaknet::V4l2FrameSource src(cfg);

    // 两条路径各一个计数器。原子量是为了让编译器和读者都清楚：
    // 这两个数字是被另一个线程写的。
    std::atomic<uint64_t> sink_calls{0};
    std::atomic<uint64_t> sink_bytes{0};
    std::atomic<uint64_t> sink_first_ts{0};
    std::atomic<uint64_t> sink_last_ts{0};
    std::atomic<uint32_t> sink_width{0};
    std::atomic<uint32_t> sink_height{0};
    std::atomic<uint32_t> sink_stride{0};
    std::atomic<uint32_t> sink_uv_offset{0};
    std::atomic<uint64_t> sink_first_seq{0};

    src.setPixelSink([&](const weaknet::V4l2FrameSource::PixelFrame& pf) {
        // 首帧几何只记一次：它是后续 hub->configure 的输入，若 stride 或
        // uv_offset 与文档不符，画面会偏色且不报错。
        if (sink_calls.load(std::memory_order_relaxed) == 0) {
            sink_width.store(pf.width, std::memory_order_relaxed);
            sink_height.store(pf.height, std::memory_order_relaxed);
            sink_stride.store(pf.stride, std::memory_order_relaxed);
            sink_uv_offset.store(pf.uv_offset, std::memory_order_relaxed);
            sink_first_ts.store(pf.timestamp_ns, std::memory_order_relaxed);
            sink_first_seq.store(pf.sequence, std::memory_order_relaxed);
        }
        sink_last_ts.store(pf.timestamp_ns, std::memory_order_relaxed);
        sink_bytes.fetch_add(pf.size, std::memory_order_relaxed);
        sink_calls.fetch_add(1, std::memory_order_relaxed);
    });

    if (!src.start()) {
        std::printf("!! frame_source->start() 失败: %s\n", src.lastError().c_str());
        return 2;
    }
    std::printf("[src] start OK: %s\n", src.describe().c_str());

    // 等到点或收到信号。这里刻意不用 snapshotAndReset 的 window 来计时：
    // 探针要的是「墙钟 10 秒内发生了多少事」，与生产归因节奏无关。
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 关键的一步：把帧源自己的账取出来，与 sink 的账并排比对。
    const weaknet::FrameStats st = src.snapshotAndReset();
    src.stop();

    const uint64_t calls = sink_calls.load();
    std::printf("\n=== 结论 ===\n");
    std::printf("[frames] valid=%s window_ms=%u received=%llu expected=%llu "
                "corrupt=%llu seq_gaps=%u max_gap_ms=%u p95_gap_ms=%u "
                "dequeue_p95_ms=%u bytes=%llu\n",
                st.valid() ? "yes" : "no", st.window_ms,
                static_cast<unsigned long long>(st.frames_received),
                static_cast<unsigned long long>(st.frames_expected),
                static_cast<unsigned long long>(st.frames_corrupt),
                st.seq_gap_count, st.max_gap_ms, st.p95_gap_ms,
                st.p95_dequeue_latency_ms,
                static_cast<unsigned long long>(st.bytes_received));
    std::printf("[sink]   calls=%llu bytes=%llu\n",
                static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(sink_bytes.load()));

    if (calls > 0) {
        const uint64_t first_ts = sink_first_ts.load();
        const uint64_t last_ts = sink_last_ts.load();
        std::printf("[sink]   几何=%ux%u stride=%u uv_offset=%u 首帧seq=%llu\n",
                    sink_width.load(), sink_height.load(), sink_stride.load(),
                    sink_uv_offset.load(),
                    static_cast<unsigned long long>(sink_first_seq.load()));
        if (last_ts > first_ts && calls > 1) {
            const double span_s = static_cast<double>(last_ts - first_ts) / 1e9;
            std::printf("[sink]   时间轴跨度=%.3fs 实测帧率=%.2f fps\n", span_s,
                        static_cast<double>(calls - 1) / span_s);
        }
    }

    if (!st.valid() && calls == 0) {
        std::printf("\n-> 帧源根本没采样过：start() 之后一帧都没走完采集周期。\n");
        return 2;
    }
    // `valid()` 的含义是「采样过」，不是「采到了帧」——它只判断 sampled_at
    // 是否被置过。所以 valid=true 而 received=0 是完全可能的：采集跑了一轮，
    // 但一帧都没拿到。这种情形下 sink 当然也是 0 次调用，若直接落到下面那条
    // 判定，就会把「采集侧不出帧」误报成「像素分发有问题」——把上游故障归给
    // 下游，是最容易让人改错代码的一类误判。必须先按 received 分流。
    if (st.valid() && st.frames_received == 0 && calls == 0) {
        std::printf("\n-> **采集侧根本没出帧**：DQBUF 一帧都没成功，"
                    "pixel_sink 自然收不到。问题在采集，不在分发。\n");
        return 1;
    }
    if (calls == 0) {
        std::printf("\n-> **帧出得来但 pixel_sink 一次都没被调用**：问题在像素分发。\n");
        return 1;
    }
    if (!st.valid()) {
        std::printf("\n-> sink 有数据但帧源统计无效：统计口径与分发路径不一致。\n");
        return 1;
    }

    std::printf("\n-> 两条路径都有数据：采集与分发正常。\n");
    return 0;
}
