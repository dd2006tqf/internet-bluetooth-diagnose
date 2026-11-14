/**
 * @file video_frame_hub.cpp
 * @brief 帧中枢实现：dma-buf/memfd 槽位 + 限速发布 + 引用计数租约
 *
 * 设计要点见头文件。这里只补充三处实现层面的决策：
 *
 * 1. **租约是引用计数而不是独占锁**。面板和编码器可以同时持同一帧——它们
 *    都只读，写保护由 `refs > 0` 拦住即可。做成独占会让两个消费者互相抢，
 *    谁先拿到谁把另一个饿死。
 *
 * 2. **memcpy 在锁外**，且拷贝期间该槽位标记为 `writing`，acquire 不会交出
 *    半写状态。锁内只做指针与计数，临界区在纳秒级。
 *
 * 3. **租约超时按 slot 记录起始时刻**，不是全局一个时间戳。全局戳会让
 *    「刚拿到的新租约」被「很久以前那个进程的旧租约」的时长时间误杀。
 */

#include "video_frame_hub.hpp"

#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace weaknet {
namespace {

/// 从 dma-heap 分配一块可共享内存。失败返回 -1
int allocFromDmaHeap(size_t size) {
    const int hfd = ::open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (hfd < 0) return -1;
    dma_heap_allocation_data data{};
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    data.heap_flags = 0;
    const int rc = ::ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &data);
    ::close(hfd);
    if (rc < 0) return -1;
    return static_cast<int>(data.fd);
}

/// 回落方案：匿名共享内存。跨进程 mmap 可行，但**不是 dma-buf**
int allocFromMemfd(size_t size) {
    const int fd = ::memfd_create("weaknet-frame", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

struct VideoFrameHub::Impl {
    VideoFrameHubConfig cfg;

    /// 一块槽位内存。
    ///
    /// 用 `shared_ptr` 持有不是为了共享所有权给消费者（消费者拿的是 dup 出来
    /// 的 fd，自己有 mmap），而是为了**让 `configure()` 在 `publish()` 正于
    /// 锁外 memcpy 时回收不掉这块内存**。
    ///
    /// 否则会这样炸：publish 拿到 dst → 放锁 → configure 抢到锁 → munmap →
    /// publish 的 memcpy 往已 unmap 的地址写 → SIGSEGV，或者更糟，那块地址
    /// 已被别的线程重新 mmap，于是写坏了别人的数据。
    ///
    /// 计数器（generation）只能挡住「提交时写错新位图」，挡不住这个——
    /// 这是两件事，缺一不可。
    struct Mapping {
        int fd{-1};
        void* map{MAP_FAILED};
        size_t size{0};

        ~Mapping() {
            if (map != nullptr && map != MAP_FAILED && size > 0) {
                ::munmap(map, size);
            }
            if (fd >= 0) ::close(fd);
        }
    };

    struct Slot {
        std::shared_ptr<Mapping> mem;
        /// 引用计数：>0 表示至少有一个消费者正在读，不得覆写
        uint32_t refs{0};
        /// 正在 memcpy（不可 acquire）
        bool writing{false};
        /// 已写入过至少一帧（区分「空槽」与「有内容但格式变了」）
        bool published{false};
        /// 本槽第一个未归还租约的取走时刻。refs 归零时失效。
        ///
        /// 必须**按槽**记：全局一个时间戳会让「A 槽的老租约超时」把
        /// 「B 槽刚取走的新租约」一起当成过期回收掉。
        std::chrono::steady_clock::time_point lease_start;
    };
    std::vector<Slot> slots;

    // ---- 几何 ----
    uint32_t width{0};
    uint32_t height{0};
    uint32_t stride{0};
    uint32_t fourcc{0};
    size_t size{0};
    /// UV 平面相对帧起始的字节偏移（0 = 等于 stride*height）
    ///
    /// 存的是**事实值**，原样来自生产者。中枢不做任何推导：它不知道
    /// 驱动按什么规则对齐平面，按 stride*height 猜会落进对齐空洞。
    uint32_t uv_offset{0};
    bool configured{false};

    /// 槽位代号：每次 configure() 重建槽位就 +1
    ///
    /// 为什么需要它：`publish()` 在**锁外**做 memcpy。若这期间有人调
    /// `configure()` 把 slots 清空并 munmap，那个 `dst` 就是悬空指针，
    /// memcpy 会写进已归还的地址（最坏的形态是写坏别人的堆）。
    /// 只用「提交时检查 slot >= slots.size()」不够——尺寸可能恰好没变。
    /// 提交时比对代号，不一致就把本次发布作废。
    uint64_t generation{0};

    // ---- 交付状态 ----
    uint32_t latest{UINT32_MAX};   ///< 最新可交付的槽位号
    uint64_t latest_seq{0};
    uint64_t latest_ts{0};

    // ---- 限速 ----
    std::chrono::steady_clock::time_point last_publish;
    bool has_published{false};

    // ---- 统计与锁 ----
    mutable std::mutex mtx;
    Stats st;
    std::string backend_name{"none"};

    void releaseSlotsLocked() {
        // 这里**不** munmap：Mapping 的析构函数负责。若此刻有 publish 正持着
        // 一个 shared_ptr 在锁外 memcpy，那块内存会活到它拷完为止。
        slots.clear();
    }

    /// 回收超时租约（调用方持锁）
    ///
    /// 消费者崩溃没 release 时，那个 slot 会被永久占住。若不回收，
    /// `skipped_busy` 会一路涨到「再也不发布」——面板永远停在最后一帧，
    /// 且没人知道为什么。宁可回收并记一条 `reclaimed`，让这件事可观测。
    void reclaimExpiredLocked(std::chrono::steady_clock::time_point now) {
        if (cfg.lease_timeout_ms == 0) return;
        const auto timeout = std::chrono::milliseconds(cfg.lease_timeout_ms);
        for (auto& s : slots) {
            if (s.refs == 0) continue;
            if (now - s.lease_start <= timeout) continue;
            s.refs = 0;
            s.lease_start = {};
            ++st.reclaimed;
            // 该槽若正是 latest，保持 latest 不变：里面可能还是有效画面，
            // 下一个消费者仍能取到（只是可能不够新）。清掉反而让面板黑屏。
        }
    }
};

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

VideoFrameHub::VideoFrameHub(VideoFrameHubConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg = config;
}

VideoFrameHub::~VideoFrameHub() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->releaseSlotsLocked();
}

// ---------------------------------------------------------------------------
// configure
// ---------------------------------------------------------------------------

bool VideoFrameHub::configure(uint32_t width, uint32_t height, uint32_t stride,
                              uint32_t fourcc, size_t size,
                              uint32_t uv_offset) {
    Impl& s = *impl_;

    if (width == 0 || height == 0 || size == 0) return false;
    // stride 缺省时按 width 兜底：拿不到 stride 的驱动是存在的，此时按紧凑
    // 排布假设，至少能出图（可能斜切，但不至于全黑）。
    if (stride == 0) stride = width;

    const uint32_t want = s.cfg.slot_count ? s.cfg.slot_count : 3;

    std::lock_guard<std::mutex> lk(s.mtx);

    // 几何没变就复用，别重建——重建会让在途租约指向已释放的内存
    if (s.configured && s.width == width && s.height == height &&
        s.stride == stride && s.fourcc == fourcc && s.size == size &&
        s.uv_offset == uv_offset && s.slots.size() == want) {
        return true;
    }

    s.releaseSlotsLocked();
    s.configured = false;
    s.latest = UINT32_MAX;
    // 代号推进必须在**释放之后、任何人拿到新指针之前**。此刻若有 publish
    // 正在锁外 memcpy，它持有的旧代号已经对不上，提交时会被作废。
    ++s.generation;
    s.width = width;
    s.height = height;
    s.stride = stride;
    s.fourcc = fourcc;
    s.size = size;
    s.uv_offset = uv_offset;

    // 先试 dma-heap，再回落 memfd。整批用同一种：混用会让「某个槽能交给
    // 编码器、另一个不能」变成一条只在特定帧上出现的诡异故障。
    std::vector<int> fds;
    fds.reserve(want);
    const char* backend = nullptr;

    if (s.cfg.use_dma_heap) {
        for (uint32_t i = 0; i < want; ++i) {
            const int fd = allocFromDmaHeap(size);
            if (fd < 0) break;
            fds.push_back(fd);
        }
        if (fds.size() == want) {
            backend = "dma-heap";
        } else {
            for (int fd : fds) ::close(fd);
            fds.clear();
        }
    }

    if (fds.empty()) {
        for (uint32_t i = 0; i < want; ++i) {
            const int fd = allocFromMemfd(size);
            if (fd < 0) break;
            fds.push_back(fd);
        }
        if (fds.size() == want) backend = "memfd";
    }

    if (fds.size() != want) {
        for (int fd : fds) ::close(fd);
        s.backend_name = "none";
        return false;
    }

    s.slots.resize(want);
    for (uint32_t i = 0; i < want; ++i) {
        void* m = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fds[i], 0);
        if (m == MAP_FAILED) {
            // 回滚：已建好的随 shared_ptr 一起收掉（含刚失败这个的 fd）
            ::close(fds[i]);
            s.slots.clear();
            s.backend_name = "none";
            return false;
        }
        auto mem = std::make_shared<Impl::Mapping>();
        mem->fd = fds[i];
        mem->map = m;
        mem->size = size;
        s.slots[i].mem = std::move(mem);
    }

    s.backend_name = backend;
    s.configured = true;
    return true;
}

// ---------------------------------------------------------------------------
// publish
// ---------------------------------------------------------------------------

bool VideoFrameHub::publish(const void* src, size_t src_size,
                            uint64_t sequence, uint64_t timestamp_ns) {
    Impl& s = *impl_;
    if (src == nullptr || src_size == 0) return false;

    const auto now = std::chrono::steady_clock::now();

    // 先做限速与选槽（持锁），再在锁外 memcpy，最后回锁里提交。
    //
    // 分两段持锁的原因很直接：3 MB 的 memcpy 在这块 A55 上约 1ms 量级，
    // 若放在锁里，面板的 acquire() 会被采集线程每 100ms 卡住 1ms——那是
    // 可感知的抖动来源，且完全没有必要。
    uint32_t slot = UINT32_MAX;
    size_t copy_size = 0;
    uint64_t gen = 0;
    // 在锁外拷贝期间**持有一份共享所有权**。这是 `Mapping` 用 shared_ptr 的
    // 全部理由：configure() 可能在我们放锁后清空 slots，但只要这里握着引用，
    // 析构就会推迟到 memcpy 结束，`dst` 始终有效。
    std::shared_ptr<Impl::Mapping> held;

    {
        std::lock_guard<std::mutex> lk(s.mtx);

        if (!s.configured) {
            ++s.st.skipped_unconfigured;
            return false;
        }

        // 限速：消费者不需要满帧率，白拷 3MB×18fps 纯属浪费
        if (s.cfg.min_publish_interval_ms > 0 && s.has_published) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - s.last_publish);
            if (elapsed.count() < static_cast<long long>(s.cfg.min_publish_interval_ms)) {
                ++s.st.skipped_rate;
                return false;
            }
        }

        s.reclaimExpiredLocked(now);

        // 选一个既没被租用、也没在写的槽
        //
        // 从 latest 之后轮转而不是总选 0 号：始终选 0 号会让 acquire 与
        // publish 抢同一块，另外两个槽形同虚设。
        for (size_t i = 0; i < s.slots.size(); ++i) {
            const size_t idx = (static_cast<size_t>(s.latest == UINT32_MAX ? 0 : s.latest) + 1 + i) %
                               s.slots.size();
            if (s.slots[idx].refs == 0 && !s.slots[idx].writing) {
                slot = static_cast<uint32_t>(idx);
                break;
            }
        }

        if (slot == UINT32_MAX) {
            // 所有槽都在被读。**不覆写**：覆写会让消费者看到撕裂画面，
            // 而且这种撕裂无法与「解码错误」区分。跳过是可观测的。
            ++s.st.skipped_busy;
            return false;
        }

        held = s.slots[slot].mem;
        if (!held) {
            ++s.st.skipped_unconfigured;
            return false;
        }
        s.slots[slot].writing = true;
        copy_size = std::min(src_size, s.size);
        gen = s.generation;
        if (src_size < s.size) ++s.st.truncated;
    }

    // ---- 锁外拷贝 ----
    std::memcpy(held->map, src, copy_size);

    // ---- 提交 ----
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (gen != s.generation) {
            // configure() 在拷贝期间被调用：槽位已整体重建。内存本身是安全的
            // （shared_ptr 撑住了），但这次写入落在了**已经不属于中枢**的旧
            // 缓冲里，交付出去就是旧几何配新数据。作废，让调用方下一帧重来。
            // 同样不能去碰 s.slots[slot]——那是新位图里的另一个槽。
            ++s.st.skipped_unconfigured;
            return false;
        }
        s.slots[slot].writing = false;
        s.slots[slot].published = true;
        s.latest = slot;
        s.latest_seq = sequence;
        s.latest_ts = timestamp_ns;
        s.last_publish = now;
        s.has_published = true;
        ++s.st.published;
    }
    return true;
}

// ---------------------------------------------------------------------------
// acquire / release
// ---------------------------------------------------------------------------

bool VideoFrameHub::acquire(VideoFrameLease& out) {
    Impl& s = *impl_;
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lk(s.mtx);

    if (!s.configured) return false;
    s.reclaimExpiredLocked(now);

    if (s.latest == UINT32_MAX || s.latest >= s.slots.size()) return false;

    Impl::Slot& slot = s.slots[s.latest];
    if (!slot.published || slot.writing || !slot.mem) return false;

    // dup 一份 fd 给调用方：调用方 close 自己的那份不影响中枢持有的那份。
    // 直接把中枢的 fd 交出去的话，调用方一 close 中枢就废了。
    const int fd = ::fcntl(slot.mem->fd, F_DUPFD_CLOEXEC, 3);
    if (fd < 0) return false;

    if (slot.refs == 0) slot.lease_start = now;
    ++slot.refs;
    ++s.st.acquired;

    out.fd = fd;
    out.width = s.width;
    out.height = s.height;
    out.stride = s.stride;
    out.uv_offset = s.uv_offset;
    out.fourcc = s.fourcc;
    out.sequence = s.latest_seq;
    out.timestamp_ns = s.latest_ts;
    out.size = s.size;
    out.slot = s.latest;
    return true;
}

void VideoFrameHub::release(uint32_t slot) {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mtx);
    if (slot >= s.slots.size()) return;          // 幂等：未知 slot 直接忽略
    Impl::Slot& sl = s.slots[slot];
    if (sl.refs == 0) return;                    // 重复归还
    --sl.refs;
    if (sl.refs == 0) sl.lease_start = {};
    ++s.st.released;
}

// ---------------------------------------------------------------------------
// 观测
// ---------------------------------------------------------------------------

std::string VideoFrameHub::backend() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->backend_name;
}

std::string VideoFrameHub::describe() const {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> lk(s.mtx);

    std::ostringstream os;
    os << "hub:" << s.backend_name;
    if (s.configured) {
        os << "@" << s.width << "x" << s.height << "/stride=" << s.stride
           << "/uvoff=" << s.uv_offset
           << "/size=" << s.size;
    } else {
        os << "(unconfigured)";
    }
    os << " pub=" << s.st.published
       << " skip(rate=" << s.st.skipped_rate
       << ",busy=" << s.st.skipped_busy
       << ",unconf=" << s.st.skipped_unconfigured << ")"
       << " acq=" << s.st.acquired
       << " rel=" << s.st.released
       << " reclaim=" << s.st.reclaimed;
    if (s.st.truncated > 0) os << " truncated=" << s.st.truncated;
    return os.str();
}

VideoFrameHub::Stats VideoFrameHub::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->st;
}

}  // namespace weaknet
