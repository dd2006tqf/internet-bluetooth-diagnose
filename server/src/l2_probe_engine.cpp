/**
 * @file l2_probe_engine.cpp
 * @brief 二层回流探测引擎实现（AF_PACKET 收发）
 */

#include "l2_probe_engine.hpp"

// 头文件包含顺序是载荷的，不是风格问题：
//   linux/if.h 与 net/if.h 定义同一批 IFF_* 宏，谁先谁赢。
//   linux/if_packet.h 会连带引入 linux/if.h，所以必须先显式包含 net/if.h
//   （它提供 if_nametoindex / if_indextoname / IF_NAMESIZE）。
//   若反过来，会得到几十条 "IFF_UP conflicts with a previous declaration"。
#include <net/if.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>

#include "logger.hpp"
// ServerContext / WeakNetConfig：applyL2ProbeConfig 与 applyL2ProbeRuntimeConfig
// 需要完整定义（它们读 ctx->cfg.* 与 ctx->l2_probe 的所有权）。
// 这两个函数刻意放在本文件而不是 server.cpp —— 见文件末尾的说明。
#include "server.hpp"

namespace weaknet_dbus {

namespace {

/// 探测帧专用 EtherType：IEEE 802 Local Experimental Ethertype 1。
/// 标准留给实验用途；选它的决定性理由是隔离——所有现有 eBPF 程序都不解析它，
/// 所以探测帧在内核过滤层就与被动证据分离了。
constexpr uint16_t kProbeEtherType = 0x88B5;

/// 帧格式版本。未来改格式时递增，旧账本仍可解析。
constexpr uint8_t kProbeVersion = 1;

/// 帧尾魔数，防止把截断/畸变的帧当成有效回流
constexpr uint16_t kProbeMagic = 0xA55A;

/// payload 长度：ver(1) + run_id(8) + seq(4) + magic(2)
constexpr size_t kPayloadLen = 15;

/**
 * 帧布局（不含 FCS，由硬件追加）：
 *
 *   0      6      12        14   15        23        27      29
 *   +------+------+---------+----+---------+---------+-------+----...
 *   | dst  | src  | 0x88B5  | ver| run_id  |  seq    | magic | (到 29)
 *   +------+------+---------+----+---------+---------+-------+
 */
struct ProbeFrame {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;   // 网络序
    uint8_t version;
    uint64_t run_id;      // 本机序（我们自己解析，不跨主机）
    uint32_t seq;         // 本机序
    uint16_t magic;       // 本机序
} __attribute__((packed));

static_assert(sizeof(ProbeFrame) == 6 + 6 + 2 + 1 + 8 + 4 + 2,
              "探测帧布局必须紧致，不能有编译器填充");

uint64_t monotonicUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/// 墙钟毫秒。ts_start_ms / ts_end_ms 必须同域（都是墙钟）——
/// 混用单调时钟会让落库的时间跨度变成负数或几十年，
/// 而下游按 ts 查询/清理时不会报错，只会静默查不到。
uint64_t wallClockMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool parseMac(const std::string& s, uint8_t out[6]) {
    unsigned b[6];
    char tail;
    if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2],
                    &b[3], &b[4], &b[5], &tail) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (b[i] > 0xFF) return false;
        out[i] = static_cast<uint8_t>(b[i]);
    }
    return true;
}

std::string macToString(const uint8_t m[6]) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1],
                  m[2], m[3], m[4], m[5]);
    return std::string(buf);
}

/// 单播判定：最低位为 0。组播/广播一律拒绝。
bool isUnicast(const uint8_t m[6]) { return (m[0] & 0x01) == 0; }

std::vector<std::string> splitTrim(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            // trim
            size_t b = cur.find_first_not_of(" \t");
            size_t e = cur.find_last_not_of(" \t");
            if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    size_t b = cur.find_first_not_of(" \t");
    size_t e = cur.find_last_not_of(" \t");
    if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
    return out;
}

}  // namespace

L2ProbeEngine::L2ProbeEngine() = default;

L2ProbeEngine::~L2ProbeEngine() { stop(); }

int32_t L2ProbeEngine::resolveIfindex(const std::string& name) {
    // if_nametoindex 在接口不存在时返回 0 并置 errno = ENODEV
    unsigned idx = ::if_nametoindex(name.c_str());
    return idx == 0 ? 0 : static_cast<int32_t>(idx);
}

std::string L2ProbeEngine::resolveIfname(int32_t ifindex) {
    char buf[IF_NAMESIZE] = {0};
    if (::if_indextoname(static_cast<unsigned>(ifindex), buf) == nullptr) {
        return std::string();
    }
    return std::string(buf);
}

bool L2ProbeEngine::isEthernetDevice(const std::string& name) {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct ifreq ifr {};
    std::snprintf(ifr.ifr_name, IF_NAMESIZE, "%s", name.c_str());
    bool ok = false;
    if (::ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
        // 只接受以太网卡。loopback(veth 不是 loopback) 与隧道设备
        // 没有稳定的二层语义，探它们得到的\"回流\"无法解释。
        ok = (ifr.ifr_hwaddr.sa_family == ARPHRD_ETHER);
    }
    ::close(fd);
    return ok;
}

bool L2ProbeEngine::parseIfaces(const std::string& spec) {
    auto names = splitTrim(spec, ',');
    if (names.empty()) {
        last_error_ = "l2_probe.egress_ifaces 为空：不探测任何接口";
        return false;
    }
    targets_.clear();
    for (const auto& n : names) {
        const int32_t idx = resolveIfindex(n);
        if (idx == 0) {
            last_error_ = "l2_probe.egress_ifaces: 接口不存在 '" + n + "'";
            return false;
        }
        if (!isEthernetDevice(n)) {
            last_error_ = "l2_probe.egress_ifaces: '" + n +
                          "' 不是以太网接口（ARPHRD_ETHER）";
            return false;
        }
        L2ProbeTarget t;
        t.ifname = n;
        t.ifindex = idx;
        t.vlan_id = -1;
        targets_.push_back(t);
    }

    // 单口拒绝启动，而不是"启动但永远没结论"。
    //
    // 判据放在 plan 层（checkL2ProbeEgressEligibility）而不是这里另写一遍：
    // 那条规则是**语义契约**，必须能被不需要 CAP_NET_RAW 的单测穷举，
    // 否则它只会在真机部署时以"归因恒为 NO_LOOP"的形态暴露。
    // 详见 l2_probe_plan.hpp 的注释。
    const std::string ineligible = checkL2ProbeEgressEligibility(targets_);
    if (!ineligible.empty()) {
        last_error_ = ineligible;
        targets_.clear();
        return false;
    }
    return true;
}

bool L2ProbeEngine::parsePeers(const std::string& spec) {
    auto items = splitTrim(spec, ',');
    if (items.empty()) {
        // 与 active_probe.targets 同一纪律：不内置任何默认目标。
        // 没有目标就不知道往哪发，宁可 disabled 也不猜。
        last_error_ =
            "l2_probe.probe_peers 为空：不内置任何默认探测目标，必须显式配置";
        return false;
    }
    peers_.clear();
    for (const auto& s : items) {
        uint8_t mac[6];
        if (!parseMac(s, mac)) {
            last_error_ = "l2_probe.probe_peers: MAC 格式非法 '" + s + "'";
            return false;
        }
        if (!isUnicast(mac)) {
            // 广播/组播探测帧会在客户生产网络里制造额外流量，
            // 而广播风暴恰恰是我们想诊断的故障——绝不能自己制造它。
            last_error_ = "l2_probe.probe_peers: 只接受单播 MAC，拒绝组播/广播 '" +
                          s + "'";
            return false;
        }
        peers_.push_back(macToString(mac));
    }
    return true;
}

bool L2ProbeEngine::applyVlanSpec(const std::string& spec) {
    // 空 = 全部不带 tag（native VLAN）。不自动做 PVID 归一化——
    // 归一化需要读取交换机配置，那超出了\"观测\"的范围。
    if (spec.empty()) return true;

    // 格式 "eth0:100,eth1:200"
    auto items = splitTrim(spec, ',');
    for (const auto& item : items) {
        const size_t colon = item.rfind(':');
        if (colon == std::string::npos) {
            last_error_ = "l2_probe.vlan_ids: 期望 ifname:vlan，得到 '" + item + "'";
            return false;
        }
        const std::string ifname = item.substr(0, colon);
        const std::string vlan_s = item.substr(colon + 1);
        int vlan = -1;
        try {
            vlan = std::stoi(vlan_s);
        } catch (...) {
            last_error_ = "l2_probe.vlan_ids: VLAN 非数字 '" + vlan_s + "'";
            return false;
        }
        if (vlan < 0 || vlan > 4094) {
            last_error_ = "l2_probe.vlan_ids: VLAN 超出 0..4094 '" + vlan_s + "'";
            return false;
        }
        bool matched = false;
        for (auto& t : targets_) {
            if (t.ifname == ifname) {
                t.vlan_id = vlan;
                matched = true;
            }
        }
        if (!matched) {
            last_error_ = "l2_probe.vlan_ids: 接口 '" + ifname +
                          "' 不在 egress_ifaces 中";
            return false;
        }
    }
    return true;
}

bool L2ProbeEngine::configure(const std::string& egress_ifaces,
                              const std::string& probe_peers,
                              const std::string& vlan_ids,
                              uint32_t max_frames_per_round,
                              uint32_t rate_limit_pps, uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_.clear();
    targets_.clear();
    peers_.clear();

    if (!parseIfaces(egress_ifaces)) return false;
    if (!parsePeers(probe_peers)) return false;
    if (!applyVlanSpec(vlan_ids)) return false;

    // ---- 硬钳制：配置无法突破安全上界 ----
    //
    // 这两条不是"建议"。rate_limit_pps 曾被认为是纯性能参数，直到意识到
    // 配置写错一个零就能把 4000 pps 的可疑流量打进客户网络——那是我们
    // 正要诊断的故障形态。所以上限写在代码里，不写在配置里。
    const uint32_t clamped_rate = std::clamp(rate_limit_pps, 1u, kMaxRatePps);
    const uint32_t clamped_frames =
        std::clamp(max_frames_per_round, 1u, kMaxFramesPerRound);
    const uint32_t clamped_timeout = std::clamp(timeout_ms, 100u, 30000u);

    // 参数与目标集合一起在锁内提交：本函数开头已持 mutex_，
    // runRound() 也在开头持锁做整体快照，因此不会出现"新目标 + 旧速率"。
    // **不要**在这里再套一层 lock_guard —— std::mutex 不可重入，
    // 内层构造会立刻自死锁。
    rate_limit_pps_ = clamped_rate;
    max_frames_per_round_ = clamped_frames;
    timeout_ms_ = clamped_timeout;

    if (rate_limit_pps != clamped_rate) {
        LOG_WARNING(LogModule::NETWORK, "l2_probe.rate_limit_pps 被钳制到硬上限 "
                                         << kMaxRatePps << "（配置值 " << rate_limit_pps
                                         << "）");
    }
    if (max_frames_per_round != clamped_frames) {
        LOG_WARNING(LogModule::NETWORK, "l2_probe.max_frames_per_round 被钳制到硬上限 "
                                         << kMaxFramesPerRound << "（配置值 "
                                         << max_frames_per_round << "）");
    }

    LOG_INFO(LogModule::NETWORK,
             "l2_probe 配置就绪: rate=" << clamped_rate << "pps max_frames="
                                        << clamped_frames << " timeout_ms="
                                        << clamped_timeout);
    return true;
}

bool L2ProbeEngine::updateLimits(uint32_t max_frames_per_round, uint32_t rate_limit_pps,
                                 uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (targets_.empty()) {
        // 没有目标 = configure 没成功过。在线改参数无意义，让调用方走重建路径。
        return false;
    }
    rate_limit_pps_ = std::clamp(rate_limit_pps, 1u, kMaxRatePps);
    max_frames_per_round_ = std::clamp(max_frames_per_round, 1u, kMaxFramesPerRound);
    timeout_ms_ = std::clamp(timeout_ms, 100u, 30000u);
    LOG_INFO(LogModule::NETWORK,
             "l2_probe 参数在线更新: rate=" << rate_limit_pps_ << "pps max_frames="
                                            << max_frames_per_round_
                                            << " timeout_ms=" << timeout_ms_);
    return true;
}

std::vector<std::string> L2ProbeEngine::egressIfaces() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(targets_.size());
    for (const auto& t : targets_) out.push_back(t.ifname);
    return out;
}

bool L2ProbeEngine::start() {
    if (isUsable() && socket_fd_ >= 0) return true;  // 已启动

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (targets_.empty() || peers_.empty()) {
            if (last_error_.empty()) {
                last_error_ = "未 configure 或配置无效，不能启动";
            }
            LOG_ERROR(LogModule::NETWORK,
                      "l2_probe 启动被拒绝: " << last_error_);
            return false;
        }
    }

    // SOCK_RAW 而非 SOCK_DGRAM：需要完整以太头才能拿到 dst MAC，
    // 从而排除\"被自己 IP 栈吐回来\"的假阳性。
    int fd = ::socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC,
                      htons(kProbeEtherType));
    if (fd < 0) {
        last_error_ = std::string("socket(AF_PACKET) 失败: ") + std::strerror(errno) +
                      "（需要 CAP_NET_RAW）";
        LOG_ERROR(LogModule::NETWORK, "l2_probe " << last_error_);
        return false;
    }

    // 只收这个 EtherType 的帧。内核会按协议过滤，因此被动流量
    // 根本不会到达本 socket；同时我们也收不到客户网络里的任何业务报文。
    struct sockaddr_ll sll {};
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(kProbeEtherType);
    sll.sll_ifindex = 0;  // 0 = 所有接口（我们要看\"从哪个口回来\"）
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&sll), sizeof(sll)) < 0) {
        last_error_ = std::string("bind(AF_PACKET) 失败: ") + std::strerror(errno);
        LOG_ERROR(LogModule::NETWORK, "l2_probe " << last_error_);
        ::close(fd);
        return false;
    }

    // 接收缓冲：单轮最多 64 帧 × 64 字节，256KB 绰绰有余，
    // 且能吸收内核在窗口内的突发。
    int rcvbuf = 256 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    socket_fd_ = fd;
    LOG_INFO(LogModule::NETWORK, "l2_probe AF_PACKET socket 已打开 (EtherType 0x88B5)");
    return true;
}

void L2ProbeEngine::stop() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
        LOG_INFO(LogModule::NETWORK, "l2_probe AF_PACKET socket 已关闭");
    }
}

L2ProbeRoundRecord L2ProbeEngine::runRound(uint64_t now_ms) {
    L2ProbeRoundRecord rec;
    rec.ts_start_ms = now_ms;

    // round_id 用墙钟 + 随机后缀：同一毫秒内两次调用也不碰撞，
    // 跨重启同样不碰撞（与 site_incident 的 incident_id 同一手法）。
    {
        static std::atomic<uint64_t> counter{0};
        std::random_device rd;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "l2r-%llu-%04x%04x",
                      static_cast<unsigned long long>(now_ms),
                      static_cast<unsigned>(rd() & 0xFFFF),
                      static_cast<unsigned>(counter.fetch_add(1) & 0xFFFF));
        rec.round_id = buf;
    }

    if (!isUsable()) {
        rec.degraded_reason = "not_started";
        rec.ts_end_ms = now_ms;
        LOG_WARNING(LogModule::NETWORK, "l2_probe runRound 被跳过：引擎不可用");
        return rec;
    }

    std::vector<L2ProbeTarget> targets;
    std::vector<std::string> peers;
    std::vector<std::string> peers_snapshot;
    uint32_t max_frames = 0;
    uint32_t rate_pps = 1;
    uint32_t timeout_ms = 2000;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets = targets_;
        peers = peers_;
        peers_snapshot = peers_;
        max_frames = max_frames_per_round_;
        rate_pps = rate_limit_pps_;
        timeout_ms = timeout_ms_;
    }

    // ---- 计划本轮 ----
    weaknet::L2ProbeTokenBucket bucket(rate_pps);
    weaknet::L2ProbeRoundPlan plan =
        planL2ProbeRound(targets, max_frames, &bucket, now_ms);
    rec.covered = plan.covered;
    rec.degraded_reason = plan.degraded_reason;

    if (plan.frames.empty()) {
        rec.ts_end_ms = wallClockMs();
        if (rec.degraded_reason.empty()) rec.degraded_reason = "no_frames_planned";
        LOG_WARNING(LogModule::NETWORK,
                 "l2_probe 本轮零帧可发: " << rec.degraded_reason);
        return rec;
    }

    // ---- 发送 ----
    //
    // 每次发送前重读本机出口 MAC：接口可能在轮中途被重配（这正是我们要观测的
    // 现场变化之一），缓存一份启动时的 MAC 会让账本撒谎。
    std::vector<L2ProbeRoundRecord::FrameResult> pending;
    pending.reserve(plan.frames.size());

    for (const auto& fp : plan.frames) {
        L2ProbeRoundRecord::FrameResult fr;
        fr.seq = fp.seq;
        fr.egress_ifname = fp.target.ifname;
        fr.egress_ifindex = fp.target.ifindex;
        fr.vlan_id = fp.target.vlan_id;

        uint8_t src_mac[6] = {0};
        {
            int cfd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (cfd >= 0) {
                struct ifreq ifr {};
                std::snprintf(ifr.ifr_name, IF_NAMESIZE, "%s",
                              fp.target.ifname.c_str());
                if (::ioctl(cfd, SIOCGIFHWADDR, &ifr) == 0) {
                    std::memcpy(src_mac, ifr.ifr_hwaddr.sa_data, 6);
                }
                ::close(cfd);
            }
        }

        if (src_mac[0] == 0 && src_mac[1] == 0 && src_mac[2] == 0 &&
            src_mac[3] == 0 && src_mac[4] == 0 && src_mac[5] == 0) {
            rec.degraded_reason = "iface_down";
            fr.sent = false;
            pending.push_back(fr);
            continue;
        }

        // dst MAC 取第一个 peer。多 peer 时的分配策略：按 seq 轮转，
        // 保证同一 peer 不会被连续打，也不会漏掉任何一个。
        uint8_t dst_mac[6];
        const std::string& peer = peers[fp.seq % peers.size()];
        if (!parseMac(peer, dst_mac)) {
            rec.degraded_reason = "peer_parse_failed";
            fr.sent = false;
            pending.push_back(fr);
            continue;
        }

        ProbeFrame frame{};
        std::memcpy(frame.dst, dst_mac, 6);
        std::memcpy(frame.src, src_mac, 6);
        frame.ethertype = htons(kProbeEtherType);
        frame.version = kProbeVersion;
        // run_id 用 round_id 的哈希：同轮所有帧相同，跨轮不同。
        // 这样收到老帧（上一轮迟到）时能识别并丢弃。
        std::hash<std::string> hasher;
        frame.run_id = static_cast<uint64_t>(hasher(rec.round_id));
        frame.seq = fp.seq;
        frame.magic = kProbeMagic;

        struct sockaddr_ll addr {};
        addr.sll_family = AF_PACKET;
        addr.sll_protocol = htons(kProbeEtherType);
        addr.sll_ifindex = fp.target.ifindex;
        addr.sll_halen = ETH_ALEN;
        std::memcpy(addr.sll_addr, dst_mac, 6);

        const ssize_t n = ::sendto(socket_fd_, &frame, sizeof(frame), 0,
                                   reinterpret_cast<struct sockaddr*>(&addr),
                                   sizeof(addr));
        if (n == static_cast<ssize_t>(sizeof(frame))) {
            fr.sent = true;
            fr.sent_ms = monotonicUs() / 1000;
        } else {
            // 单帧失败不中断整轮，但整轮标记降级：部分证据不能当完整证据用。
            rec.degraded_reason = "send_failed";
        }
        pending.push_back(fr);
    }

    // ---- 收包 ----
    //
    // 窗口固定为 timeout_ms，用 poll 等待；收到帧立即按 seq 配对，
    // 不等窗口结束才处理（STM 收敛期的迟到帧也要收）。
    auto matchIngress = [&](int32_t ingress_ifindex,
                            const std::string& ingress_name) -> bool {
        // 有帧从非出口接口回来 → 跨口回流这一事实成立
        bool multi = false;
        for (auto& fr : pending) {
            if (!fr.sent) continue;
            if (fr.egress_ifindex != ingress_ifindex) {
                multi = true;
                break;
            }
        }
        (void)ingress_name;
        return multi;
    };

    const uint64_t recv_start_us = monotonicUs();
    const uint64_t deadline_us =
        recv_start_us + static_cast<uint64_t>(timeout_ms) * 1000ULL;

    while (true) {
        const uint64_t now_us = monotonicUs();
        if (now_us >= deadline_us) break;
        const int wait_ms =
            static_cast<int>(std::min<uint64_t>((deadline_us - now_us) / 1000, 50));
        struct pollfd pfd {};
        pfd.fd = socket_fd_;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            rec.degraded_reason = "recv_error";
            break;
        }
        if (pr == 0) continue;  // 超时片，回到循环检查 deadline

        // 一次把可用帧全部读出来：只读一帧会让后续帧在缓冲里排队，
        // 而每帧都要再等一个 poll 周期。
        while (true) {
            uint8_t buf[128];
            struct sockaddr_ll from {};
            socklen_t fromlen = sizeof(from);
            const ssize_t n = ::recvfrom(socket_fd_, buf, sizeof(buf), MSG_DONTWAIT,
                                         reinterpret_cast<struct sockaddr*>(&from),
                                         &fromlen);
            if (n <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                rec.degraded_reason = "recv_error";
                break;
            }
            if (n < static_cast<ssize_t>(sizeof(ProbeFrame))) continue;

            ProbeFrame got;
            std::memcpy(&got, buf, sizeof(got));
            if (got.magic != kProbeMagic) continue;
            if (got.version != kProbeVersion) continue;
            if (ntohs(got.ethertype) != kProbeEtherType) continue;

            std::hash<std::string> hasher;
            if (got.run_id != static_cast<uint64_t>(hasher(rec.round_id))) {
                continue;  // 迟到帧，属于别的轮次
            }

            // PACKET_OUTGOING 标记的是我们自己发出的帧被\"回显\"，
            // 不是真正的入口帧。必须排除，否则每轮都会自我匹配。
            if (from.sll_pkttype == PACKET_OUTGOING) continue;

            for (auto& fr : pending) {
                if (fr.seq == got.seq && fr.sent && fr.ingress_ifindex < 0) {
                    fr.ingress_ifindex = from.sll_ifindex;
                    fr.ingress_ifname = resolveIfname(from.sll_ifindex);
                    fr.recv_ms = monotonicUs() / 1000;
                    fr.rtt_us = static_cast<int64_t>(
                        (fr.recv_ms - fr.sent_ms) * 1000ULL);
                    if (matchIngress(from.sll_ifindex, fr.ingress_ifname)) {
                        rec.has_multi_return = true;
                        const std::string pair = fr.egress_ifname + "->" +
                                                 (fr.ingress_ifname.empty()
                                                      ? std::to_string(from.sll_ifindex)
                                                      : fr.ingress_ifname);
                        if (std::find(rec.return_pairs.begin(),
                                      rec.return_pairs.end(),
                                      pair) == rec.return_pairs.end()) {
                            rec.return_pairs.push_back(pair);
                        }
                    }
                    break;
                }
            }
        }
    }

    rec.frames = std::move(pending);
    rec.ts_end_ms = wallClockMs();

    // ---- 假阳性排除 ----
    //
    // 从"帧从另一个口回来"到"存在二层通路"之间，有一个与二层无关的原因
    // 会造成同样的观测结果：**目标 MAC 就是本机某个口**。此时帧被本地
    // 协议栈收下，回来的路径是"自己发给自己"，不是二层回流。
    //
    // 只有这一道。**ip_forward 不在其列** —— 这里曾经把它当作整轮降级的
    // 判据，在隔离 netns 里实测发现那是错的，理由有两条：
    //
    //   1. 判据与机制不匹配。ip_forward 决定的是**IP 包**是否被转发；
    //      我们的探测帧是自定义 EtherType 0x88B5，IP 栈根本不解析它，
    //      更不会转发它。用 IP 转发开关去否决二层观测，是拿一个
    //      不相干的开关去否决一份不相干证据。
    //   2. 它会让功能在真实部署里永久失效。开发板作为网关，ip_forward 恒为 1，
    //      于是每一轮都被判降级 → 归因恒为 NONE → 装了等于没装。
    //      这种"看起来在工作、实际永远没有结论"的形态比功能缺失更难发现。
    //
    // 真正需要排除的转发路径是"帧被桥转发到另一个口"。那不是需要排除的
    // 假阳性 —— 桥转发正是**二层通路存在的证据**，正是我们要观测的东西。
    {
        for (const auto& fr : rec.frames) {
            if (!fr.sent) continue;
            int cfd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (cfd < 0) continue;
            struct ifreq ifr {};
            std::snprintf(ifr.ifr_name, IF_NAMESIZE, "%s", fr.egress_ifname.c_str());
            if (::ioctl(cfd, SIOCGIFHWADDR, &ifr) == 0) {
                for (const auto& peer : peers_snapshot) {
                    uint8_t pm[6];
                    if (parseMac(peer, pm) &&
                        std::memcmp(pm, ifr.ifr_hwaddr.sa_data, 6) == 0) {
                        rec.degraded_reason = "self_egress_loop";
                        break;
                    }
                }
            }
            ::close(cfd);
            if (!rec.degraded_reason.empty()) break;
        }
    }

    // ---- 逐帧重算 multi_return（收包窗口里可能有多帧）----
    // 上面的 matchIngress 只检查"是否存在非本口帧"，这里做最终汇总，
    // 避免依赖收包顺序。
    rec.has_multi_return = false;
    rec.return_pairs.clear();
    for (const auto& fr : rec.frames) {
        if (!fr.sent || fr.ingress_ifindex < 0) continue;
        if (fr.ingress_ifindex != fr.egress_ifindex) {
            rec.has_multi_return = true;
            const std::string pair =
                fr.egress_ifname + "->" +
                (fr.ingress_ifname.empty() ? std::to_string(fr.ingress_ifindex)
                                           : fr.ingress_ifname);
            if (std::find(rec.return_pairs.begin(), rec.return_pairs.end(), pair) ==
                rec.return_pairs.end()) {
                rec.return_pairs.push_back(pair);
            }
        }
    }

    rec.covered = rec.covered && rec.degraded_reason.empty();

    // 关于「帧从原口回来」的语义澄清：
    // 同口回流（ingress_ifindex == egress_ifindex）**不是**异常，不计入
    // has_multi_return，也不是自环。它的含义是"目标就在这个口后面"，
    // 是这个口存在二层对端的正常证据。真正的自环已在上面按
    // dst MAC == 本机口 MAC 排除（degraded_reason = self_egress_loop）。
    //
    // 这里不再有任何与 ip_forward 相关的判断，理由见上方假阳性排除段。

    LOG_INFO(LogModule::NETWORK,
             "l2_probe round " << rec.round_id << ": planned=" << rec.frames.size()
                               << " sent="
                               << std::count_if(rec.frames.begin(), rec.frames.end(),
                                                [](const auto& f) { return f.sent; })
                               << " recv="
                               << std::count_if(rec.frames.begin(), rec.frames.end(),
                                                [](const auto& f) {
                                                    return f.ingress_ifindex >= 0;
                                                })
                               << " multi_return="
                               << (rec.has_multi_return ? "yes" : "no")
                               << " covered=" << (rec.covered ? "yes" : "no")
                               << " degraded=" << rec.degraded_reason);

    return rec;
}

// ======================================================================
// 配置接线：与 active_connectivity_monitor.cpp 的 applyActiveProbeConfig 同构，
// 刻意放在 server_lib 内（而不是 server.cpp）——dbus_service.cpp 的
// SetMonitorParam 要调用它，而 dbus_service.cpp 会进测试目标。
// 放在 server.cpp 会导致所有链接 dbus_service.cpp 的测试目标
// 出现 undefined reference。
// ======================================================================

/**
 * @brief 按当前配置重建二层探测引擎（l2_probe）
 *
 * 为什么是"重建"而不是"改字段"：出口网卡集合、对端 MAC、VLAN 列表决定
 * AF_PACKET socket 的语义（哪些 ifindex 参与、帧长是否带 802.1Q tag）。
 * 在线逐个改会跑出"一半新目标一半旧目标"的轮次，那一轮的回流证据无法解释。
 * 重建是唯一能让"配置变更"保持原子性的方式。
 *
 * 配置无效时保持 nullptr（引擎不存在 = disabled），不半启用：
 * configure() 返回 false 时日志已写明原因（出口不存在 / 对端非单播 / 空列表）。
 */
void applyL2ProbeConfig(ServerContext* ctx) {
    if (!ctx) return;

    // 先拆掉旧引擎（含 socket）。
    //
    // **调用约束**：探测线程可能正在 runRound() 里持着引擎的裸指针，
    // 因此本函数只允许在两种时机调用：
    //   (a) 启动阶段，线程尚未创建（server.cpp 的初始化块）；
    //   (b) 关闭阶段，线程已 join（收尾序列）。
    // 运行期的参数变更走 applyL2ProbeRuntimeConfig，它绝不 reset 引擎。
    if (ctx->l2_probe) {
        ctx->l2_probe->stop();
        ctx->l2_probe.reset();
    }

    if (!ctx->cfg.l2_probe.enabled.load()) {
        LOG_INFO(LogModule::NETWORK, "l2_probe: disabled by config");
        return;
    }

    auto engine = std::make_unique<L2ProbeEngine>();
    const bool ok = engine->configure(
        ctx->cfg.l2_probe.egress_ifaces.get(),
        ctx->cfg.l2_probe.probe_peers.get(),
        ctx->cfg.l2_probe.vlan_ids.get(),
        ctx->cfg.l2_probe.max_frames_per_round.load(),
        ctx->cfg.l2_probe.rate_limit_pps.load(),
        ctx->cfg.l2_probe.timeout_ms.load());

    if (!ok) {
        // 配置被拒 → 保持 disabled。这里**不**回写 cfg.enabled：
        // 配置是部署方的意图，引擎不可用是运行时状态。把意图改成 false
        // 会让"为什么我的配置没生效"永远查不到（配置自己变成了 false）。
        LOG_WARNING(LogModule::NETWORK,
                    "l2_probe: configure rejected (" << engine->lastError()
                                                     << "); probe stays disabled");
        return;
    }

    if (!engine->start()) {
        LOG_WARNING(LogModule::NETWORK,
                    "l2_probe: AF_PACKET socket unavailable (need CAP_NET_RAW); "
                    "probe stays disabled");
        return;
    }

    // 记录目标，便于现场核对"到底在探哪些口"
    {
        std::ostringstream oss;
        for (const auto& name : engine->egressIfaces()) {
            if (oss.tellp() > 0) oss << ",";
            oss << name;
        }
        LOG_INFO(LogModule::NETWORK,
                 "l2_probe: engine ready, egress=[" << oss.str() << "]"
                                                    << " interval_ms="
                                                    << ctx->cfg.l2_probe.interval_ms.load()
                                                    << " timeout_ms="
                                                    << ctx->cfg.l2_probe.timeout_ms.load());
    }

    ctx->l2_probe = std::move(engine);
}

/**
 * @brief 运行期参数变更（绝不销毁引擎）
 *
 * 与 applyL2ProbeConfig 的分工是本模块最容易出错的地方，必须说清楚：
 *
 *   applyL2ProbeConfig       → 只在**线程未运行**时调用，会 stop+reset 引擎；
 *   applyL2ProbeRuntimeConfig→ 只在**线程运行中**调用，绝不碰引擎的所有权。
 *
 * 为什么运行期不能走重建：探测线程在 `runRound()` 里阻塞最长 timeout_ms，
 * 期间持有 `ctx->l2_probe` 解引用得到的指针。主线程若 reset 这个 unique_ptr，
 * 探测线程手里的指针立刻变成悬垂 —— 下一帧 sendto 就是 UAF，且由于
 * runRound 在锁内、崩溃点在内核调用里，栈回溯会指向完全无关的地方。
 *
 * 目标集合（egress_ifaces / probe_peers / vlan_ids）无法在线改：
 * 它们决定 socket 要覆盖哪些 ifindex、帧是否带 802.1Q tag。改了必须重启
 * 服务 —— 这不是能力缺失，而是"一半新目标一半旧目标"的轮次无法解释。
 * 配置层（weaknet_config.cpp）已对这三个键在 run-time set 路径上拒收，
 * 所以这里只需要处理三个限流参数的在线更新。
 */
void applyL2ProbeRuntimeConfig(ServerContext* ctx) {
    if (!ctx) return;

    const bool want = ctx->cfg.l2_probe.enabled.load();
    if (!want) {
        // 运行期关闭：置停止标志，让线程在下一轮之前退出循环并 break。
        // 不 reset 引擎 —— 线程可能正持有它。线程退出后由收尾序列
        // （或下一次启动）负责销毁。这是"不半启用"的另一面：
        // 关闭也不是立即生效，而是"本轮跑完就不再跑"。
        ctx->l2_probe_stop.store(true);
        LOG_INFO(LogModule::NETWORK, "l2_probe: runtime disable requested");
        return;
    }

    if (!ctx->l2_probe) {
        // 运行期从 disabled 切到 enabled：没有引擎可热更新。
        // 不在这里造引擎 —— start_l2_probe_thread 已经不在跑了
        // （线程只在启动阶段创建），凭空造一个没人调 runRound 的引擎
        // 只会产生一个永远没有数据的 isUsable()==true 对象。
        // 正确做法是记一条日志，让部署方重启服务。
        LOG_WARNING(LogModule::NETWORK,
                    "l2_probe: runtime enable needs a service restart "
                    "(thread is only created at startup)");
        return;
    }

    if (!ctx->l2_probe->updateLimits(
            ctx->cfg.l2_probe.max_frames_per_round.load(),
            ctx->cfg.l2_probe.rate_limit_pps.load(),
            ctx->cfg.l2_probe.timeout_ms.load())) {
        LOG_WARNING(LogModule::NETWORK,
                    "l2_probe: updateLimits rejected (engine not configured)");
    }
}

}  // namespace weaknet_dbus
