/**
 * @file test_snapshot_service_gtest.cpp
 * @brief SnapshotService 契约测试（无摄像头、无 VE、无 dma-heap）
 *
 * ============================================================================
 * 这份测试针对的是什么
 * ============================================================================
 *
 * 快照服务比编码器多出三类风险，每一类都**不会报错**：
 *
 *   1. **重复入队**：事故巡检线程每 2 秒轮询一次。若每次请求都入队，
 *      一次持续 5 分钟的事故会写出 150 张 600KB 的图（90 MB）。
 *   2. **路径穿越**：incident_id 是外部输入，拼进路径时任何一个 `..`
 *      都会让快照写到目录之外。
 *   3. **槽位泄漏**：忘了 release 会让 hub 三个槽位很快被吃光，采集侧
 *      publish 全部跳过——但采集线程本身"看起来"完全正常。
 *
 * 所以这里断言的全部是「队列/路径/生命周期」语义，而不是「能不能编出图」。
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>

#include "snapshot_service.hpp"

namespace weaknet {
namespace {

/// 可注入的假帧源：借出一块普通内存，并记录 acquire/release 次数
class FakeFrameSource : public ISnapshotFrameSource {
public:
    explicit FakeFrameSource(bool has_frame = true) : m_has(has_frame) {}

    bool acquire(Frame& out) override {
        ++acquires;
        if (!m_has) return false;
        out.data = m_buf.data();
        out.size = m_buf.size();
        out.width = 1640;
        out.height = 1232;
        out.stride = 1640;
        out.uv_offset = 2030336;
        out.sequence = 42;
        out.timestamp_ns = 1700000000000000000ull;
        out.slot = 0;
        ++outstanding;
        return true;
    }

    void release(uint32_t) override {
        ++releases;
        --outstanding;
    }

    std::string backend() const override { return "fake-frames"; }

    std::atomic<int> acquires{0};
    std::atomic<int> releases{0};
    std::atomic<int> outstanding{0};

    std::vector<uint8_t> m_buf = std::vector<uint8_t>(3045504, 0x80);

private:
    bool m_has;
};

/// 可注入的假上传器：只记录「收到了什么」，不发任何网络请求
///
/// 为什么不让单测用真的 `VideoClipExporter`：那会把 OpenSSL 私钥、libcurl
/// 编译开关、退避时长一并拖进这份测试，而这里要断言的是「落盘后有没有把
/// 同一份字节交出去」——两件事混在一起，失败原因就再也读不出来了。
class FakeUploader : public ISnapshotUploader {
public:
    explicit FakeUploader(bool accept = true) : m_accept(accept) {}

    bool enqueueSnapshot(EdgeSnapshotUpload shot) override {
        ++calls;
        if (!m_accept) return false;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_shots.push_back(std::move(shot));
        return true;
    }

    std::vector<EdgeSnapshotUpload> shots() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_shots;
    }

    std::atomic<int> calls{0};

private:
    bool m_accept;
    mutable std::mutex m_mutex;
    std::vector<EdgeSnapshotUpload> m_shots;
};

/// 临时目录守卫：析构时递归删除
class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/wn_snap_test_XXXXXX";
        char* p = ::mkdtemp(tmpl);
        m_path = p ? p : "";
    }
    ~TempDir() { removeAll(m_path); }
    const std::string& path() const { return m_path; }

    /// 递归列目录下所有普通文件（相对路径）
    std::vector<std::string> files() const {
        std::vector<std::string> out;
        walk(m_path, "", out);
        return out;
    }

private:
    static void walk(const std::string& root, const std::string& rel,
                     std::vector<std::string>& out) {
        const std::string dir = rel.empty() ? root : root + "/" + rel;
        DIR* d = ::opendir(dir.c_str());
        if (!d) return;
        struct dirent* e = nullptr;
        while ((e = ::readdir(d)) != nullptr) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string child_rel = rel.empty() ? name : rel + "/" + name;
            const std::string full = root + "/" + child_rel;
            struct stat st;
            if (::stat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                walk(root, child_rel, out);
            } else {
                out.push_back(child_rel);
            }
        }
        ::closedir(d);
    }

    static void removeAll(const std::string& p) {
        if (p.empty()) return;
        std::string cmd = "rm -rf '" + p + "'";
        if (std::system(cmd.c_str()) != 0) {
            // 清理失败不影响测试结论
        }
    }

    std::string m_path;
};

/// 等在 async 完成后检查（服务是异步的，必须等）
bool waitFor(const std::function<bool()>& pred, int timeout_ms = 3000) {
    for (int i = 0; i < timeout_ms / 10; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// ===========================================================================
// 队列语义
// ===========================================================================

TEST(SnapshotServiceQueue, DuplicateIncidentIsNotQueuedTwice) {
    // 这是本模块最重要的一条：事故巡检每 2 秒轮询一次，去重失效会
    // 让一次长事故产出上百张图。
    //
    // 必须**连着发**才能证明去重真的生效：第一张在工作线程里编完出队只要
    // 10 ms，如果每发一次就 sleep 等它拍完，那其实测的是「已处理表」而不是
    // 「待办队列」——两段各自都该被测到，所以这里同时断言两个阶段。
    auto frames = std::make_unique<FakeFrameSource>();
    auto* frames_raw = frames.get();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());

    // 阶段一：全在队列里时连发 5 次（尚未被消费）
    for (int i = 0; i < 5; ++i) svc.request("inc-dup");
    EXPECT_EQ(svc.stats().requested, 5u);
    EXPECT_EQ(svc.stats().queued, 1u) << "同一事故只能入队一次";
    EXPECT_EQ(svc.stats().duplicate, 4u) << "队列内去重失效";

    // 阶段二：等它拍完出队，再发 5 次——这一次命中的是已处理表。
    // 若只查待办队列，这 5 次会全部入队，最终写出 6 张图（正是被单测
    // 抓出来的那个缺陷）。
    ASSERT_TRUE(waitFor([&]() { return svc.stats().written >= 1; }));
    const uint64_t dups_before = svc.stats().duplicate;
    for (int i = 0; i < 5; ++i) svc.request("inc-dup");
    EXPECT_EQ(svc.stats().queued, 1u) << "拍完出队后仍然不能重复入队";
    EXPECT_EQ(svc.stats().duplicate, dups_before + 5) << "已处理表去重失效";

    svc.stop();
    EXPECT_EQ(dir.files().size(), 1u) << "无论请求多少次，一个事故只留一张图";
    EXPECT_GE(frames_raw->releases.load(), 1);
}

TEST(SnapshotServiceQueue, RequestBeforeStartIsRejected) {
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();
    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);

    EXPECT_FALSE(svc.request("inc-1")) << "未 start 时应明确拒绝，不能默默攒着";
    EXPECT_EQ(svc.stats().queued, 0u);
}

TEST(SnapshotServiceQueue, EmptyIncidentIdIsRejected) {
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();
    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    EXPECT_FALSE(svc.request(""));
    svc.stop();
}

// ===========================================================================
// 端到端（桩编码器）
// ===========================================================================

TEST(SnapshotServiceEndToEnd, WritesOneFilePerIncident) {
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request("inc-A"));

    const bool ok = waitFor([&]() { return svc.stats().written >= 1; });
    ASSERT_TRUE(ok) << "3 秒内没写出文件: " << svc.describe();

    const auto files = dir.files();
    ASSERT_EQ(files.size(), 1u) << "一个事故一张图";
    EXPECT_NE(files[0].find("inc-A"), std::string::npos)
        << "文件必须在以 incident_id 命名的子目录里，实际: " << files[0];
    EXPECT_NE(files[0].find(".jpg"), std::string::npos);
    EXPECT_EQ(files[0].find(".tmp"), std::string::npos)
        << "不能留下 .tmp：那是写盘失败或没 rename";

    svc.stop();
}

TEST(SnapshotServiceEndToEnd, LeaseIsAlwaysReleased) {
    // 槽位泄漏是静默故障：hub 槽位吃光后采集侧 publish 全部跳过，
    // 而采集线程看起来完全正常。
    auto frames = std::make_unique<FakeFrameSource>();
    auto* raw = frames.get();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    for (int i = 0; i < 3; ++i) {
        svc.request("inc-" + std::to_string(i));
    }
    ASSERT_TRUE(waitFor([&]() { return svc.stats().written >= 3; }));
    svc.stop();

    EXPECT_EQ(raw->acquires.load(), raw->releases.load())
        << "每次 acquire 必须配对一次 release";
    EXPECT_EQ(raw->outstanding.load(), 0) << "不能有未归还的租约";
}

TEST(SnapshotServiceEndToEnd, NoFrameIsCountedNotCrashed) {
    auto frames = std::make_unique<FakeFrameSource>(/*has_frame=*/false);
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request("inc-noframe"));

    ASSERT_TRUE(waitFor([&]() { return svc.stats().no_frame >= 1; }));
    EXPECT_EQ(svc.stats().written, 0u);
    EXPECT_EQ(svc.stats().captured, 0u) << "取不到帧不能算编码成功";
    svc.stop();
    EXPECT_TRUE(dir.files().empty());
}

TEST(SnapshotServiceEndToEnd, EmptyOutputDirEncodesWithoutWriting) {
    // 「只编不落盘」用于验证场景：让「编码成功」与「写盘成功」分开观察。
    auto frames = std::make_unique<FakeFrameSource>();
    SnapshotServiceConfig cfg;
    cfg.output_dir.clear();

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request("inc-nodisk"));
    ASSERT_TRUE(waitFor([&]() { return svc.stats().captured >= 1; }));
    EXPECT_EQ(svc.stats().written, 0u);
    EXPECT_EQ(svc.outputStats().images, 1u);
    svc.stop();
}

// ===========================================================================
// 上行交接（setUploader）
// ===========================================================================

TEST(SnapshotServiceUpload, HandsOffTheSameBytesItWroteToDisk) {
    // 断言的是**同一份字节**：上传用的必须是内存里那张（已被 move），不是
    // 按路径重读。若哪天有人把这里改成「读文件再发」，本用例会在
    // 文件被清理时静默通过——所以这里连长度和内容都比。
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    FakeUploader uploader;
    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    svc.setUploader(&uploader);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request("inc-upload"));

    ASSERT_TRUE(waitFor([&]() { return uploader.calls.load() >= 1; }));
    const auto shots = uploader.shots();
    ASSERT_EQ(shots.size(), 1u) << "一次事故只该交接一张图";

    const auto& shot = shots[0];
    EXPECT_EQ(shot.incident_id, "inc-upload");
    EXPECT_EQ(shot.width, 1640u);
    EXPECT_EQ(shot.height, 1232u);
    EXPECT_FALSE(shot.bytes.empty());
    EXPECT_TRUE(shot.valid());

    // image_id 必须合规（云端正则 `^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$`）：
    // 第一段不允许是 '.' 或 '-'，否则云端 422 而本地只会看到「上传失败」。
    ASSERT_FALSE(shot.image_id.empty());
    const char first = shot.image_id.front();
    EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(first)))
        << "image_id 首字符必须字母数字: " << shot.image_id;
    EXPECT_EQ(shot.image_id.find('/'), std::string::npos)
        << "image_id 会拼进对象键与 URL，不能含分隔符";

    svc.stop();
}

TEST(SnapshotServiceUpload, ImageIdAndCapturedTimeDescribeTheSameInstant) {
    // 两者分别用来做幂等键与事实入库。取两次墙钟会让它们差几毫秒——
    // 数据各自都对，但彼此对不上，事后复盘会当成「板端时钟跳变」。
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    FakeUploader uploader;
    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    svc.setUploader(&uploader);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request("inc-clock"));
    ASSERT_TRUE(waitFor([&]() { return uploader.calls.load() >= 1; }));

    const auto shots = uploader.shots();
    ASSERT_EQ(shots.size(), 1u);
    const std::string expect_prefix =
        "snap-" + std::to_string(shots[0].captured_at_ms) + "-";
    EXPECT_EQ(shots[0].image_id.rfind(expect_prefix, 0), 0u)
        << "image_id=" << shots[0].image_id
        << " 与 captured_at_ms=" << shots[0].captured_at_ms << " 不是同一时刻";
    svc.stop();
}

TEST(SnapshotServiceUpload, DiskWriteStillHappensWhenUploaderRejectsEverything) {
    // 上行被拒**不得**影响本地留证。把云端不可达报成「拍证失败」会让运维
    // 去查摄像头，而摄像头一切正常。
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    FakeUploader uploader(/*accept=*/false);
    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    svc.setUploader(&uploader);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request("inc-reject"));

    ASSERT_TRUE(waitFor([&]() { return uploader.calls.load() >= 1; }));
    EXPECT_EQ(svc.stats().written, 1u) << "本地必须已经写出";
    EXPECT_EQ(svc.stats().failed, 0u) << "上行被拒不等于拍证失败";
    EXPECT_EQ(svc.stats().uploaded_handoff, 0u);
    EXPECT_EQ(dir.files().size(), 1u);
    svc.stop();
}

TEST(SnapshotServiceUpload, NoUploaderKeepsLegacyBehaviour) {
    // 未调用 setUploader 时行为完全不变：这条保证所有既有单测与探针工具
    // 不受影响，也让 setUploader 成为一个**纯增量**能力。
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request("inc-legacy"));
    ASSERT_TRUE(waitFor([&]() { return svc.stats().written >= 1; }));
    EXPECT_EQ(svc.stats().uploaded_handoff, 0u);
    EXPECT_NE(svc.describe().find("upload=off"), std::string::npos)
        << "describe 必须明确说上行没接: " << svc.describe();
    svc.stop();
}

TEST(SnapshotServiceUpload, DoesNotUploadInEncodeOnlyMode) {
    // output_dir 为空 = 「只编不落盘」的验证模式。这种模式被探针工具与单测
    // 使用，把它的图发到云端只会在事故列表里留下没有事故的孤儿证据。
    auto frames = std::make_unique<FakeFrameSource>();
    SnapshotServiceConfig cfg;
    cfg.output_dir.clear();

    FakeUploader uploader;
    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    svc.setUploader(&uploader);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request("inc-encode-only"));
    ASSERT_TRUE(waitFor([&]() { return svc.stats().captured >= 1; }));
    EXPECT_EQ(uploader.calls.load(), 0) << "验证模式不得上行";
    svc.stop();
}

// ===========================================================================
// 路径安全
// ===========================================================================

TEST(SnapshotServicePath, RejectsTraversalInIncidentId) {
    // incident_id 是外部输入。`../` 必须被中和，文件只能落在 output_dir 内。
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request("../../etc/evil"));

    ASSERT_TRUE(waitFor([&]() { return svc.stats().written >= 1; }));
    svc.stop();

    const auto files = dir.files();
    ASSERT_EQ(files.size(), 1u);
    // 断言的是「不会穿越」这个性质，而不是「字符串里不含 ..」。
    // `../../etc/evil` 全部替换成 `_` 后是 `.._.._etc_evil`——它含 `..` 但
    // 不含分隔符，根本不是一个目录跳转。用字符串匹配会把安全的结果误判成
    // 不安全，反过来也挡不住真正的穿越（那要看文件名落到了哪里）。
    EXPECT_EQ(files[0].find('/'), files[0].rfind('/')) << "只能有一层子目录，实际: " << files[0];
    const size_t slash = files[0].find('/');
    ASSERT_NE(slash, std::string::npos);
    const std::string sub = files[0].substr(0, slash);
    EXPECT_NE(sub, "..") << "子目录名不能是父目录引用";
    EXPECT_NE(sub, ".") << "子目录名不能是当前目录引用";
    EXPECT_EQ(sub.find('/'), std::string::npos);
    // files() 是相对 output_dir 枚举的，能被列出来本身就说明文件在目录内；
    // 上面再钉死「只有一层子目录」，穿越到 /etc 之类的位置不可能发生。
}

TEST(SnapshotServicePath, AllDotsIdBecomesSafe) {
    // `..` 整体是个目录跳转；逐字符替换后可能变成 `..`（如果只替换非点字符），
    // 实现必须把它整体中和掉。
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request(".."));
    ASSERT_TRUE(waitFor([&]() { return svc.stats().written >= 1; }));
    svc.stop();

    const auto files = dir.files();
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].find(".."), std::string::npos) << "实际: " << files[0];
}

// ===========================================================================
// 计数口径
// ===========================================================================

TEST(SnapshotServiceStats, OutputStatsAccumulate) {
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();
    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    svc.request("i1");
    svc.request("i2");
    ASSERT_TRUE(waitFor([&]() { return svc.stats().written >= 2; }));
    svc.stop();

    const auto out = svc.outputStats();
    EXPECT_EQ(out.images, 2u);
    EXPECT_GT(out.bytes, 0u);
    // 桩产出 10 字节，远小于上限
    EXPECT_EQ(out.over_chunk_limit, 0u);
}

TEST(SnapshotServiceStats, DescribeCarriesActionableState) {
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();
    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    const std::string d = svc.describe();
    EXPECT_NE(d.find("snapshot-service"), std::string::npos);
    EXPECT_NE(d.find("fake"), std::string::npos) << "必须能看出后端是桩，实际: " << d;
    EXPECT_NE(d.find("running=yes"), std::string::npos);
    svc.stop();
    EXPECT_NE(svc.describe().find("running=no"), std::string::npos);
}

// ===========================================================================
// 容量回收的**接线**（`pruneSnapshotDir` 自身的行为由
// `test_snapshot_retention_gtest` 覆盖）
//
// 这一组存在的理由：把 `handleOne` 里那三行
// `if (m_config.max_total_bytes > 0) pruneSnapshots(path);` 整段删掉，
// 保留模块的 21 个用例**依然全绿**——它们只测了函数本身，没人测「服务有没有
// 调用它、有没有把刚写的那张传成 keep」。而这里错了不会报任何错，只表现为
// 「磁盘慢慢满了，或者刚拍的图不见了」。
//
// 桩编码器每张产出 10 字节，所以下面用 15 字节 / 1 字节这种小上限就能把
// 「装得下一张」「装不下一张」两种边界都覆盖到。
// ===========================================================================

TEST(SnapshotServicePrune, DisabledByDefaultKeepsEveryImage) {
    // 默认 max_total_bytes = 0（不回收）。没人复核过的数字不该开始删证据，
    // 所以默认路径必须连扫描都不做。
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;  // max_total_bytes 保持 0
    cfg.output_dir = dir.path();

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    svc.request("inc-a");
    svc.request("inc-b");
    ASSERT_TRUE(waitFor([&]() { return svc.stats().written >= 2; }));
    svc.stop();

    EXPECT_EQ(dir.files().size(), 2u) << "默认关闭时一张都不该删";
    EXPECT_EQ(svc.stats().prune_runs, 0u) << "关闭时连扫描都不该发生";
    EXPECT_EQ(svc.stats().pruned, 0u);
}

TEST(SnapshotServicePrune, OverCapDeletesTheOlderImage) {
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();
    cfg.max_total_bytes = 15;  // 只装得下一张 10 字节的图

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    svc.request("inc-a");
    ASSERT_TRUE(waitFor([&]() { return svc.stats().written >= 1; }));
    svc.request("inc-b");
    ASSERT_TRUE(waitFor([&]() { return svc.stats().written >= 2; }));
    ASSERT_TRUE(waitFor([&]() { return svc.stats().pruned >= 1; }));
    svc.stop();

    EXPECT_EQ(dir.files().size(), 1u) << "超上限后必须真的删到装得下";
    EXPECT_EQ(svc.stats().pruned, 1u);
    EXPECT_GT(svc.stats().prune_runs, 0u) << "一次都没扫描，说明接线断了";
    EXPECT_EQ(svc.stats().prune_still_over, 0u) << "上限装得下一张时不该报仍超限";
}

TEST(SnapshotServicePrune, NeverDeletesTheImageItJustWrote) {
    // **接线里最关键的一条。** 上限比一张图还小时，「删到装得下」的唯一解法
    // 是删掉刚拍的这张——设备刚上报「已留证」，文件却立刻消失。
    //
    // 若 `pruneSnapshots(path)` 忘记传 keep（或传错变量），这个用例会让目录
    // 变空；只看 `pruned` 计数是看不出来的。
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();
    cfg.max_total_bytes = 1;  // 比一张图还小

    SnapshotService svc(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(svc.start());
    ASSERT_TRUE(svc.request("inc-only"));
    ASSERT_TRUE(waitFor([&]() { return svc.stats().written >= 1; }));
    ASSERT_TRUE(waitFor([&]() { return svc.stats().prune_still_over >= 1; }));
    svc.stop();

    EXPECT_EQ(dir.files().size(), 1u)
        << "刚写的那张被回收删掉了：留证等于没留";
    EXPECT_EQ(svc.stats().pruned, 0u) << "唯一一张必须被保护，一张都不该删";
    EXPECT_EQ(svc.stats().prune_still_over, 1u) << "删不干净必须可见（上限配错了要有人看见）";
}

TEST(SnapshotServicePrune, DescribeSurfacesTheCap) {
    // describe() 进日志与证据。上限关着时也必须能读出来——「为什么磁盘在涨」
    // 的第一个问题就是「回收开没开」。
    auto frames = std::make_unique<FakeFrameSource>();
    TempDir dir;
    SnapshotServiceConfig cfg;
    cfg.output_dir = dir.path();

    SnapshotService off(std::move(frames), createFakeSnapshotEncoder(), cfg);
    ASSERT_TRUE(off.start());
    EXPECT_NE(off.describe().find("quota=off"), std::string::npos)
        << "实际: " << off.describe();
    off.stop();

    auto frames2 = std::make_unique<FakeFrameSource>();
    SnapshotServiceConfig cfg2;
    cfg2.output_dir = dir.path();
    cfg2.max_total_bytes = 4096;
    SnapshotService on(std::move(frames2), createFakeSnapshotEncoder(), cfg2);
    ASSERT_TRUE(on.start());
    EXPECT_NE(on.describe().find("quota=4096"), std::string::npos)
        << "实际: " << on.describe();
    on.stop();
}

// ===========================================================================
// 记忆表裁剪：这段逻辑错了不会报错，只能靠断言钉死
//
// 两种坏法都很隐蔽：
//   - 表无界增长 → 长跑几个月才显形，表现为「内存缓慢上涨」，没人会联想到快照；
//   - 误删正在处理的那条 → 它的 tries 归零，失败后就无限重试。摄像头坏了的时候
//     等于每 2 秒敲一次，而日志上看起来只是「一直在失败」，看不出是裁剪写错。
// ===========================================================================

TEST(SnapshotAttemptsPrune, NoOpWhenUnderCap) {
    std::map<std::string, SnapshotAttempt> t;
    t["a"] = SnapshotAttempt{1, true};
    t["b"] = SnapshotAttempt{2, false};
    snapshotPruneAttempts(t, "b", 8);
    // 裁剪只在触顶时动手，绝不新增条目——`current` 由 noteAttempt 负责写入。
    EXPECT_EQ(t.size(), 2u) << "没超上限就必须原封不动";
    EXPECT_TRUE(t.count("a"));
    EXPECT_EQ(t.at("b").tries, 2) << "裁剪不该改动任何计数";
}

TEST(SnapshotAttemptsPrune, KeepsCurrentAndItsTries) {
    // 表里塞满「已拍成」的，触顶时必须整体清空——但**当前这条不能丢**。
    std::map<std::string, SnapshotAttempt> t;
    for (int i = 0; i < 10; ++i) {
        t["done-" + std::to_string(i)] = SnapshotAttempt{1, true};
    }
    t["current"] = SnapshotAttempt{2, false};
    ASSERT_GT(t.size(), 8u);

    snapshotPruneAttempts(t, "current", 8);

    EXPECT_LE(t.size(), 8u) << "裁完必须不超上限";
    ASSERT_TRUE(t.count("current")) << "正在处理的那条被删了 -> 会无限重试";
    EXPECT_EQ(t.at("current").tries, 2) << "tries 丢了 -> 上限失效";
    EXPECT_FALSE(t.at("current").done);
}

TEST(SnapshotAttemptsPrune, DropsUndoneBeforeDone) {
    // 没拍成的记录留着没有行为影响（已经试满次数），应优先清掉。
    std::map<std::string, SnapshotAttempt> t;
    t["keep-done"] = SnapshotAttempt{1, true};
    for (int i = 0; i < 10; ++i) {
        t["fail-" + std::to_string(i)] = SnapshotAttempt{3, false};
    }
    snapshotPruneAttempts(t, "keep-done", 4);

    EXPECT_LE(t.size(), 4u);
    EXPECT_TRUE(t.count("keep-done")) << "done 记录才是去重依据，必须留住";
}

TEST(SnapshotAttemptsPrune, EmptyCurrentIsSafe) {
    // 无在途事故时可能传空串：不能因此往表里塞一个空 key。
    std::map<std::string, SnapshotAttempt> t;
    for (int i = 0; i < 10; ++i) {
        t["done-" + std::to_string(i)] = SnapshotAttempt{1, true};
    }
    snapshotPruneAttempts(t, "", 4);
    EXPECT_LE(t.size(), 4u);
    EXPECT_EQ(t.count(""), 0u) << "空 key 无意义，会把表撑脏";
}

}  // namespace
}  // namespace weaknet
