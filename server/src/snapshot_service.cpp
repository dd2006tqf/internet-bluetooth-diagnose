/**
 * @file snapshot_service.cpp
 * @brief SnapshotService 实现：按需取帧 → 编码 → 原子落盘
 *
 * 关键设计见头注释。这里只补充三个实现层面的取舍：
 *
 * 1. **落盘用 tmp + rename**：rename 同文件系统内原子。断电/被杀留下的是
 *    一个 .tmp 文件而不是半张 JPEG。半张 JPEG 更危险，因为它看起来像真图。
 *
 * 2. **单线程消费**：所有 capture() 都在 m_thread 里。编码器不线程安全，
 *    这条约束靠「只有一个消费者」来保证，而不是加锁——加锁会让事故线程
 *    在编码期间被阻塞 10ms 以上。
 *
 * 3. **失败不重试**：一张快照拍不出来，重试往往还是拍不出来（帧源没帧 /
 *    VE 被占）。把失败计数暴露出去，让人从日志看到，比在后台无声重试好。
 */

#include "snapshot_service.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logger.hpp"
#include "video_frame_hub.hpp"

namespace weaknet {

void snapshotPruneAttempts(std::map<std::string, SnapshotAttempt>& attempts,
                           const std::string& current, size_t cap) {
    if (attempts.size() <= cap) return;

    // 先清「没拍成」的：它们已经试满次数，留着对行为没有影响。
    for (auto it = attempts.begin(); it != attempts.end();) {
        if (!it->second.done && it->first != current) {
            it = attempts.erase(it);
        } else {
            ++it;
        }
    }
    if (attempts.size() <= cap) return;

    // 剩下的都是「拍成了」的。整表清空后只把当前这条放回去——ID 由
    // 「时间戳 + 随机码」构成，不会复用，最坏后果是多拍一张真图。
    const int tries = attempts.count(current) ? attempts[current].tries : 0;
    attempts.clear();
    if (!current.empty()) {
        attempts[current] = SnapshotAttempt{tries, false};
    }
}

namespace {

/// 递归建目录（mkdir -p）。已存在视为成功。
///
/// 为什么不用 `std::filesystem::create_directories`：本项目的 ARM64 目标是
/// Bullseye / GCC 10，`std::filesystem` 需要额外链接 `-lstdc++fs`，而现有
/// 构建脚本没有它。自己写二十行比给所有目标加链接参数更省事。
bool makeDirs(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    cur.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        const bool last = (i + 1 == path.size());
        if (path[i] == '/' || last) {
            if (cur == "/" || cur.empty()) continue;
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

/// 墙钟毫秒。快照文件名用它——单调时钟跨重启会归零，不适合做文件名。
int64_t wallMs() {
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 +
           static_cast<int64_t>(ts.tv_nsec) / 1000000;
}

/**
 * @brief 原子写文件
 *
 * 先写 `<path>.tmp`，fsync，再 rename。顺序不能变：先 rename 后 fsync 的话，
 * 断电可能留下一个「名字对了但内容是空的」文件。
 */
bool writeFileAtomic(const std::string& path, const void* data, size_t size,
                     std::string* err) {
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        *err = "fopen(" + tmp + "): " + std::strerror(errno);
        return false;
    }
    const size_t wrote = std::fwrite(data, 1, size, f);
    if (wrote != size) {
        *err = "short write: " + std::to_string(wrote) + "/" +
               std::to_string(size);
        std::fclose(f);
        ::unlink(tmp.c_str());
        return false;
    }
    // 必须 fflush + fsync：fclose 只保证「内核缓冲」，不保证落盘。
    std::fflush(f);
    ::fsync(::fileno(f));
    std::fclose(f);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        *err = "rename: " + std::string(std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

/// 把 incident_id 变成安全的目录名
///
/// 事故 ID 来自关联器，理论上可控；但它是**外部输入**拼进路径，任何一个
/// `..` 或 `/` 都会让快照写到目录之外。这里只放行 `[A-Za-z0-9._-]`，
/// 其余替换成 `_`——宁可文件名不好看，也不能有路径穿越。
///
/// **连续两个及以上的点一律折叠成 `_`**：单个点（`a.b`、`.hidden`）是正常
/// 文件名，而 `..` 是父目录引用。只按字符白名单过滤会留下 `..`，虽然它前面
/// 没有 `/` 时不会真的穿越，但那要靠「上游一定不会拼出分隔符」这条脆弱假设
/// 来兜底。折叠掉这层假设，结果里永远不可能出现 `..`。
std::string sanitizeComponent(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const char c = in[i];
        if (c == '.') {
            size_t run = 0;
            while (i + run < in.size() && in[i + run] == '.') ++run;
            out.push_back(run >= 2 ? '_' : '.');
            i += run;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
        ++i;
    }
    // `.` 是当前目录引用，会让快照直接落进 output_dir 根部而不是子目录。
    // 空名字同样没有意义，统一换成占位符。
    if (out.empty() || out == ".") out = "_";
    return out;
}

/// 同一事故最多尝试几次。
///
/// 失败后**允许重试**，因为最常见的失败是「事故刚触发时帧源还没发布」——
/// 2 秒后重试一次通常就成了。但重试必须有界：摄像头/VE 真坏了的时候，
/// 无限重试等于每 2 秒对坏硬件做一次压力测试，还会让失败计数失去意义。
constexpr int kMaxAttempts = 3;

/// 记忆表（m_attempts）的条目上限。
///
/// 它只是去重与重试计数用的历史，不需要永久保留。触顶时优先清掉**没拍成**
/// 的记录（它们已经试满次数，留着也没有行为影响），若清完仍然超限就整体清空。
///
/// 整体清空是安全的：事故 ID 由「时间戳 + 随机码」构成，同一个 ID 不会在事后
/// 再次出现；万一真的重复，最坏后果是多拍一张，而那张图仍然是对的。
constexpr size_t kMaxRemembered = 512;

/// 记一次尝试并在触顶时裁剪
void noteAttempt(std::map<std::string, SnapshotAttempt>& attempts,
                 const std::string& incident) {
    ++attempts[incident].tries;
    snapshotPruneAttempts(attempts, incident, kMaxRemembered);
}

}  // namespace

/**
 * @brief `VideoFrameHub` 的适配器
 *
 * 把 hub 的「dma-buf fd + slot 号」翻译成「一块可读内存」。
 *
 * ============================================================================
 * 为什么必须 mmap 而不是直接读 fd
 * ============================================================================
 * 拿到的是 dma-buf（或 memfd）句柄，不是内存指针。编码器需要 CPU 能读的
 * 虚拟地址，所以必须 mmap。
 *
 * ============================================================================
 * 为什么 mmap 放在 acquire() 里、munmap 放在 release() 里
 * ============================================================================
 * 不能只 close(fd) 就完事：映射本身持有对底层内存的引用，fd 关掉后映射
 * 依然有效。如果 close 了 fd 却不 munmap，每次拍照都会泄漏一块 3MB 的
 * 地址空间——因为只在事故时触发，很难被发现。
 *
 * 所以 release() 必须做三件事，缺一不可：
 *   - munmap        → 否则地址空间泄漏
 *   - close(fd)     → 否则 dma-buf 引用泄漏
 *   - hub->release(slot) → 否则三个槽位很快全被吃掉，采集侧 publish 全跳过
 */
class HubFrameSource : public ISnapshotFrameSource {
public:
    explicit HubFrameSource(VideoFrameHub* hub) : m_hub(hub) {}

    ~HubFrameSource() override { unmap(); }

    bool acquire(Frame& out) override {
        if (!m_hub) return false;
        if (m_mapped != nullptr) {
            // 上一个租约没释放就再次 acquire：说明调用方漏了 release。
            // 强制清掉而不是叠加，否则后面会 munmap 错地址。
            unmap();
        }

        VideoFrameLease lease;
        if (!m_hub->acquire(lease)) return false;

        void* p = ::mmap(nullptr, lease.size, PROT_READ, MAP_SHARED, lease.fd, 0);
        if (p == MAP_FAILED) {
            // 映射失败也要归还：否则这个 slot 永远回不来。
            ::close(lease.fd);
            m_hub->release(lease.slot);
            return false;
        }

        m_mapped = p;
        m_size = lease.size;
        m_fd = lease.fd;
        m_slot = lease.slot;

        out.data = p;
        out.size = lease.size;
        out.width = lease.width;
        out.height = lease.height;
        out.stride = lease.stride;
        out.uv_offset = lease.uv_offset;
        out.sequence = lease.sequence;
        out.timestamp_ns = lease.timestamp_ns;
        out.slot = lease.slot;
        return true;
    }

    void release(uint32_t /*slot*/) override {
        // slot 参数在 hub 适配里不必用：我们自己记着 m_slot。用自己记的
        // 那份可以避免调用方传错 slot 造成的连锁后果。
        unmap();
    }

    std::string backend() const override {
        if (!m_hub) return "hub:none";
        // hub 在收到第一帧之前后端名恒为 `none`（分配方式要等 configure()
        // 按真实几何才定）。此时如实说「还没配」而不是「none」——后者在
        // 运维眼里等于「后端坏了」，而真实情况通常只是「还没收到帧」。
        if (!m_hub->configured()) return "hub:unconfigured";
        return "hub:" + m_hub->backend();
    }

private:
    void unmap() {
        if (m_mapped != nullptr) {
            ::munmap(m_mapped, m_size);
            m_mapped = nullptr;
        }
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        if (m_hub && m_slot != UINT32_MAX) {
            m_hub->release(m_slot);
            m_slot = UINT32_MAX;
        }
        m_size = 0;
    }

    VideoFrameHub* m_hub{nullptr};
    void* m_mapped{nullptr};
    size_t m_size{0};
    int m_fd{-1};
    uint32_t m_slot{UINT32_MAX};
};

std::unique_ptr<ISnapshotFrameSource> createHubFrameSource(VideoFrameHub* hub) {
    return std::make_unique<HubFrameSource>(hub);
}

SnapshotService::SnapshotService(std::unique_ptr<ISnapshotFrameSource> frames,
                                 std::unique_ptr<ISnapshotEncoder> encoder,
                                 SnapshotServiceConfig config)
    : m_frames(std::move(frames)),
      m_encoder(std::move(encoder)),
      m_config(std::move(config)) {}

SnapshotService::~SnapshotService() { stop(); }

bool SnapshotService::start() {
    if (m_running) return true;
    if (!m_frames || !m_encoder) {
        m_last_error = "missing frame source or encoder";
        return false;
    }
    m_stop.store(false, std::memory_order_release);
    m_running = true;
    m_thread = std::thread([this]() { run(); });
    return true;
}

void SnapshotService::stop() {
    if (!m_running) return;
    m_stop.store(true, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

bool SnapshotService::request(const std::string& incident_id) {
    if (incident_id.empty()) return false;

    std::lock_guard<std::mutex> lk(m_mutex);
    ++m_stats.requested;

    if (!m_running) {
        ++m_stats.dropped_full;   // 未启动：与「丢掉了」同一类后果
        return false;
    }

    // 同一事故已拍过（或正在待办）就不重复入队。
    //
    // 这条不是优化而是**正确性**：事故巡检线程每 2 秒轮询一次，如果每次都
    // 入队，一次持续 5 分钟的事故会产生 150 张 600KB 的图（90 MB）。而
    // 「事故现场」一张就够了。
    //
    // **必须同时查已处理表**：编码只要 10 ms，而轮询间隔 2 s——只查待办
    // 队列的话，第二次轮询时上一张早就拍完出队了，去重形同不存在。（这条
    // 曾被单测抓出来：只查队列时连续 5 次请求仍然写出了 5 张图。）
    for (const auto& id : m_pending) {
        if (id == incident_id) {
            ++m_stats.duplicate;
            return false;
        }
    }
    {
        auto it = m_attempts.find(incident_id);
        // 两种情况都挡住：
        //   - done      → 已经拍到了，一个事故一张图
        //   - tries 到顶 → 试够次数还是不行，别再把坏硬件当压力测试靶子
        if (it != m_attempts.end() &&
            (it->second.done || it->second.tries >= kMaxAttempts)) {
            ++m_stats.duplicate;
            return false;
        }
    }

    if (m_pending.size() >= m_config.max_pending) {
        // 满了丢**最旧的**：新事故比旧事故更值得留证。
        m_pending.pop_front();
        ++m_stats.dropped_full;
    }
    m_pending.push_back(incident_id);
    ++m_stats.queued;
    return true;
}

void SnapshotService::run() {
    while (!m_stop.load(std::memory_order_acquire)) {
        std::string incident;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (m_pending.empty()) {
                incident.clear();
            } else {
                incident = m_pending.front();
                m_pending.pop_front();
                // 出队即记一次尝试：这样「正在拍」与「拍完/拍砸」都有记录，
                // 重复请求在出队之后依旧挡得住（编码 10 ms 远快于轮询 2 s，
                // 只查待办队列的话第二次轮询就拦不住了）。
                noteAttempt(m_attempts, incident);
            }
        }
        if (incident.empty()) {
            // 没有待办就睡一会儿。20 ms 足够让「事故来了」到「照片拍好」
            // 的延迟保持在一次 PING 的量级，又不会空转吃 CPU。
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // 失败不删尝试记录：正是那条记录让重试在 kMaxAttempts 之后停下来。
        // 无帧重试一次通常就成了（事故刚触发时帧源可能还没发布），但硬件
        // 真坏的时候必须能停，否则等于每 2 秒敲一次坏摄像头。
        handleOne(incident);
    }
}

bool SnapshotService::handleOne(const std::string& incident_id) {
    // ---- 1. 取一帧 ----
    ISnapshotFrameSource::Frame f;
    if (!m_frames->acquire(f)) {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_stats.no_frame;
        m_last_error = "no frame available in hub";
        return false;
    }
    // 无论后面哪一步失败都必须归还租约，否则槽位会被永久占用（hub 的
    // 槽位数有限）。
    struct LeaseGuard {
        ISnapshotFrameSource* src;
        uint32_t slot;
        ~LeaseGuard() { src->release(slot); }
    } guard{m_frames.get(), f.slot};

    // ---- 2. 编码器首次使用时按真实几何 configure ----
    //
    // 必须用**帧带来的**几何而非配置常量：分辨率、stride、uv_offset 都由
    // 驱动决定。用 width*height 推 uv_offset 会落进对齐空洞，整张图偏色
    // 而且不报错（本板实测空洞 9856 字节）。
    SnapshotEncoderConfig econf;
    econf.width = f.width;
    econf.height = f.height;
    econf.stride = f.stride;
    econf.uv_offset = f.uv_offset;
    econf.frame_bytes = static_cast<uint32_t>(f.size);
    // quality 留 0 = 用编码器默认值（70）。理由见 snapshot_encoder.hpp
    // 头注释：85 在 1640x1232 下会超过云端单分片上限。
    if (!m_encoder->configure(econf)) {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_stats.failed;
        m_last_error = "encoder configure failed: " + m_encoder->describe();
        return false;
    }

    // ---- 3. 编码 ----
    SnapshotFrameView view;
    view.data = f.data;
    view.size = f.size;
    view.sequence = f.sequence;
    view.timestamp_ns = f.timestamp_ns;

    SnapshotImage img;
    if (!m_encoder->capture(view, img)) {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_stats.failed;
        m_last_error = "capture failed: " + m_encoder->describe();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_stats.captured;
        ++m_out.images;
        m_out.bytes += img.bytes.size();
        if (!img.fitsSingleChunk()) ++m_out.over_chunk_limit;
        // 「只编不落盘」模式里，编出来就算达成目的——否则同一次事故会在
        // 轮询下被反复编码，白白占用 VE。
        if (m_config.output_dir.empty()) m_attempts[incident_id].done = true;
    }

    // ---- 4. 落盘 ----
    //
    // output_dir 为空表示「只编不落盘」。这是给验证场景用的：它让
    // 「编码成功」与「写盘成功」可以分开观察，而不是混在一个计数器里。
    //
    // 这条早退也**跳过了第 5 步的上行**，这是刻意的：该模式只被探针工具
    // 与单测使用（它们自己构造 SnapshotService，从不调 setUploader），把一张
    // 诊断用的图发到云端只会在事故列表里留下没有事故的孤儿证据。
    if (m_config.output_dir.empty()) return true;

    const std::string dir =
        m_config.output_dir + "/" + sanitizeComponent(incident_id);
    if (!makeDirs(dir)) {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_stats.failed;
        m_last_error = "mkdir failed: " + dir + " (" + std::strerror(errno) + ")";
        return false;
    }

    const std::string name = std::to_string(wallMs()) + "_" +
                             std::to_string(f.sequence) + ".jpg";
    const std::string path = dir + "/" + name;

    std::string err;
    if (!writeFileAtomic(path, img.bytes.data(), img.bytes.size(), &err)) {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_stats.failed;
        m_last_error = "write failed: " + path + ": " + err;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_stats.written;
        // 拍到并写成功 → 这个事故到此为止，后续轮询全部去重。
        m_attempts[incident_id].done = true;
    }
    LOG_INFO(weaknet_dbus::LogModule::VIDEO,
             "快照已留存 incident=" << incident_id << " path=" << path
             << " bytes=" << img.bytes.size()
             << " " << img.width << "x" << img.height
             << " seq=" << img.sequence << " encode_ms=" << img.encode_ms
             << (img.fitsSingleChunk() ? "" : "（超单分片上限）"));

    // ---- 5. 上行 ----
    //
    // 顺序是**先落盘、后上传**，不可颠倒：
    //   - 落盘是本机事实，不依赖网络，永远不会因为云端不可用而丢；
    //   - 若先上传后落盘，一次成功的上传会让 `attempts.done` 提前成立，
    //     于是写盘失败的那张图再也不会重拍。
    //
    // 上传用的是**内存里这份 img.bytes**，不是按 `path` 重读：
    // `path` 所在目录将来可能被清理策略扫掉，重读会在最需要证据的时候
    // 静默失败，而失败点离事故已经过去很久。
    //
    // 上传失败**不改**本函数的返回值：本地已经留住了证据，把云端不可达
    // 报成「拍证失败」会让运维去查摄像头，而摄像头一切正常。上传侧的
    // 成败由 uploader 自己的计数器与日志表述。
    if (m_uploader != nullptr && img.fitsSingleChunk()) {
        // 墙钟只采样**一次**。两次调用会得到两个不同的毫秒值，于是
        // `image_id` 里的时刻与 `captured_at_ms` 里的时刻不一致——它们描述
        // 的是同一张图，不一致就是自相矛盾，而云端会把 captured_at_ms 当
        // 事实入库、把 image_id 当幂等键，两者都对但彼此对不上。
        const uint64_t captured_ms = static_cast<uint64_t>(wallMs());
        EdgeSnapshotUpload shot;
        shot.image_id = makeImageId(captured_ms, f.sequence);
        shot.incident_id = incident_id;
        shot.width = img.width;
        shot.height = img.height;
        shot.captured_at_ms = captured_ms;
        shot.bytes = std::move(img.bytes);
        if (m_uploader->enqueueSnapshot(std::move(shot))) {
            std::lock_guard<std::mutex> lk(m_mutex);
            ++m_stats.uploaded_handoff;
        }
    } else if (m_uploader != nullptr) {
        // 超过单张上限：契约层会 422，而 `fitsSingleChunk` 的注释正是为此
        // 存在。这里不尝试分片——为一张图引入分片只是把两个失败点换成一个，
        // 而真实场景的图（118~128 KB）离上限有 5 倍余量。
        LOG_WARNING(weaknet_dbus::LogModule::VIDEO,
                    "静帧超过单张上限，未上行 incident=" << incident_id
                    << " bytes=" << img.bytes.size());
    }

    return true;
}

std::string SnapshotService::makeImageId(uint64_t wall_ms, uint64_t sequence) const {
    // 形态：`snap-<wall_ms>-<sequence>`，与云端 `image_id` 正则
    // `^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$` 相容。
    //
    // 为什么用 sequence 而不是随机码：sequence 是 hub 给的**帧序号**，
    // 同一毫秒内连拍两张时它必然不同；而随机码在同一毫秒内仍有碰撞可能，
    // 一旦撞上，云端会把第二张判成 duplicate 从而永久少一张证据。
    //
    // 为什么前缀用 `snap-`：它不是 clip_id，混进同一段命名空间会让
    // 「按前缀枚举证据」时两种对象彼此冒充。
    return "snap-" + std::to_string(wall_ms) + "-" + std::to_string(sequence);
}

SnapshotService::Stats SnapshotService::stats() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_stats;
}

SnapshotService::OutputStats SnapshotService::outputStats() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_out;
}

std::string SnapshotService::describe() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::ostringstream os;
    os << "snapshot-service";
    if (m_encoder) {
        os << " encoder=" << m_encoder->backend();
    }
    if (m_frames) {
        os << " frames=" << m_frames->backend();
    }
    os << " running=" << (m_running ? "yes" : "no")
       << " pending=" << m_pending.size()
       << " requested=" << m_stats.requested
       << " captured=" << m_stats.captured
       << " written=" << m_stats.written
       << " failed=" << m_stats.failed
       << " no_frame=" << m_stats.no_frame;
    // 上行**未接**时明确说出来。少了这一行，运维看到「written 一直在涨」
    // 会以为证据已经到云端了，而实际上它只在本机盘上。
    os << " upload=" << (m_uploader != nullptr ? "on" : "off");
    if (m_uploader != nullptr) os << " handed_off=" << m_stats.uploaded_handoff;
    if (!m_config.output_dir.empty()) {
        os << " dir=" << m_config.output_dir;
    }
    if (!m_last_error.empty()) os << " | last_error: " << m_last_error;
    return os.str();
}

}  // namespace weaknet
