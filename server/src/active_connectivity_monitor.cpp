/**
 * @file active_connectivity_monitor.cpp
 * @brief 受控主动连通性探测 — 网络 I/O 实现
 *
 * 实现要点：
 *   1. DNS：自构造 UDP 查询报文直连配置的 resolver。
 *      不用 getaddrinfo —— libc resolver 会发出发出方不等待的附加查询
 *      （AAAA/MX），那正是此前把健康解析器误判为故障的原因。
 *      自构造报文可精确指定 QTYPE 并控制超时，语义明确。
 *
 *   2. TCP：非阻塞 connect + poll，超时可控。
 *
 *   3. TLS/HTTPS：未实现（无 TLS 开发依赖），字段保持 attempted=false，
 *      由 evaluator 表达为 NO_CAPABILITY。绝不手写 OpenSSL ABI 或 shell-out。
 *
 *   4. 依赖截断：DNS 失败则不再尝试该目标的 TCP，避免一次故障污染多个 SLE。
 */

#include "active_connectivity_monitor.hpp"
#include "logger.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <sys/socket.h>
#include <unistd.h>

namespace weaknet_dbus {

namespace {

/// FNV-1a（与 DNS 捕获侧一致，仅用于日志可读性，不参与配对）
std::string qtypeName(uint16_t) { return "A"; }

/**
 * @brief 自构造 DNS A 查询并等待应答。
 *
 * @param resolver_ip   resolver 地址（由系统解析出，见 resolveResolver）
 * @param qname         要查询的域名
 * @param timeout_sec   单次超时
 * @param out_addr      成功时填充解析到的 IPv4 地址（点分十进制）
 * @param out_detail    诊断信息
 */
bool probeDnsA(const std::string& resolver_ip, const std::string& qname,
               uint32_t timeout_sec, std::string* out_addr, std::string* out_detail) {
    // ---- 构造查询报文 ----
    std::vector<uint8_t> pkt;
    pkt.resize(12);
    const uint16_t txid = static_cast<uint16_t>(rand() & 0xFFFF);
    pkt[0] = static_cast<uint8_t>(txid >> 8);
    pkt[1] = static_cast<uint8_t>(txid & 0xFF);
    pkt[2] = 0x01;   // RD=1
    pkt[3] = 0x00;
    pkt[5] = 0x01;   // QDCOUNT=1
    // QNAME（标签长度 + 标签 + 0x00），大小写不敏感
    for (const auto& label : [&] {
             std::vector<std::string> v;
             std::string cur;
             for (char c : qname) {
                 if (c == '.') { if (!cur.empty()) v.push_back(cur); cur.clear(); }
                 else cur.push_back(c);
             }
             if (!cur.empty()) v.push_back(cur);
             return v;
         }()) {
        if (label.size() > 63) { *out_detail = "label_too_long"; return false; }
        pkt.push_back(static_cast<uint8_t>(label.size()));
        pkt.insert(pkt.end(), label.begin(), label.end());
    }
    pkt.push_back(0x00);
    pkt.push_back(0x00); pkt.push_back(0x01);  // QTYPE=A
    pkt.push_back(0x00); pkt.push_back(0x01);  // QCLASS=IN

    // ---- 发送 ----
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { *out_detail = std::string("socket: ") + std::strerror(errno); return false; }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    if (::inet_pton(AF_INET, resolver_ip.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        *out_detail = "bad_resolver_ip";
        return false;
    }
    if (::sendto(fd, pkt.data(), pkt.size(), 0,
                 reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        ::close(fd);
        *out_detail = std::string("sendto: ") + std::strerror(errno);
        return false;
    }

    // ---- 等待应答 ----
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, static_cast<int>(timeout_sec * 1000));
    if (pr <= 0) {
        ::close(fd);
        *out_detail = (pr == 0) ? "dns_timeout" : std::string("poll: ") + std::strerror(errno);
        return false;
    }

    uint8_t resp[1024];
    const ssize_t n = ::recv(fd, resp, sizeof(resp), 0);
    ::close(fd);
    if (n < 12) { *out_detail = "short_response"; return false; }

    const uint16_t rid = static_cast<uint16_t>((resp[0] << 8) | resp[1]);
    if (rid != txid) { *out_detail = "txid_mismatch"; return false; }
    const uint8_t rcode = resp[3] & 0x0F;
    if (rcode != 0) { *out_detail = std::string("rcode=") + std::to_string(rcode); return false; }

    const uint16_t ancount = static_cast<uint16_t>((resp[6] << 8) | resp[7]);
    if (ancount == 0) { *out_detail = "no_answer"; return false; }

    // ---- 遍历全部 answer 记录，查找 A 记录 ----
    //
    // 只检查第一条 answer 是错的：CDN 域名（如 www.baidu.com）的第一条
    // 通常是 CNAME（www.a.shifen.com），真正的 A 记录在其后。
    // 实测该 bug 会让健康解析器被判为 DNS capability 故障。
    auto skipName = [](const uint8_t* b, size_t n, size_t off) -> size_t {
        while (off < n) {
            const uint8_t len = b[off];
            if (len == 0) return off + 1;
            if ((len & 0xC0) == 0xC0) return off + 2;   // 压缩指针
            off += 1 + len;
        }
        return n;
    };

    size_t off = skipName(resp, static_cast<size_t>(n), 12);  // 跳过 QNAME
    if (off + 4 > static_cast<size_t>(n)) { *out_detail = "truncated_question"; return false; }
    off += 4;                                                  // QTYPE + QCLASS

    for (uint16_t i = 0; i < ancount; ++i) {
        if (off + 10 > static_cast<size_t>(n)) break;
        off = skipName(resp, static_cast<size_t>(n), off);
        if (off + 10 > static_cast<size_t>(n)) break;
        const uint16_t rtype = static_cast<uint16_t>((resp[off] << 8) | resp[off + 1]);
        const uint16_t rdlen = static_cast<uint16_t>((resp[off + 8] << 8) | resp[off + 9]);
        off += 10;
        if (off + rdlen > static_cast<size_t>(n)) break;
        if (rtype == 1 && rdlen == 4) {
            char buf[INET_ADDRSTRLEN] = {};
            in_addr a{};
            std::memcpy(&a, resp + off, 4);
            ::inet_ntop(AF_INET, &a, buf, sizeof(buf));
            *out_addr = buf;
            *out_detail = "resolved";
            return true;
        }
        off += rdlen;
    }

    *out_detail = "no_a_record";
    return false;
}

/// 非阻塞 connect + poll，带超时
bool probeTcpConnect(const std::string& ip, uint16_t port, uint32_t timeout_sec,
                     std::string* out_detail) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { *out_detail = std::string("socket: ") + std::strerror(errno); return false; }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        *out_detail = "bad_ip";
        return false;
    }

    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (rc == 0) { ::close(fd); *out_detail = "connected"; return true; }
    if (errno != EINPROGRESS) {
        *out_detail = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int pr = ::poll(&pfd, 1, static_cast<int>(timeout_sec * 1000));
    if (pr <= 0) {
        ::close(fd);
        *out_detail = (pr == 0) ? "connect_timeout" : std::string("poll: ") + std::strerror(errno);
        return false;
    }

    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0 || soerr != 0) {
        *out_detail = std::string("connect_failed: ") + std::strerror(soerr ? soerr : errno);
        ::close(fd);
        return false;
    }
    ::close(fd);
    *out_detail = "connected";
    return true;
}

/**
 * @brief 解析出系统实际使用的 resolver 地址（用于自构造 DNS 查询）。
 *
 * 只读取 /etc/resolv.conf 的 nameserver 行，不发起任何查询 ——
 * 这里解析的是"配置"，不是"域名"。
 */
std::string readConfiguredResolver() {
    FILE* f = ::fopen("/etc/resolv.conf", "r");
    if (!f) return "127.0.0.1";
    char line[256];
    std::string found;
    while (::fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (std::strncmp(p, "nameserver", 10) == 0) {
            p += 10;
            while (*p == ' ' || *p == '\t') ++p;
            char* end = p;
            while (*end && *end != '\n' && *end != ' ' && *end != '\t') ++end;
            *end = '\0';
            found = p;
            break;   // 取第一个 nameserver
        }
    }
    ::fclose(f);
    return found.empty() ? std::string("127.0.0.1") : found;
}

}  // namespace

void ActiveConnectivityMonitor::configure(const ActiveProbeConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_ = cfg;
}

std::string ActiveConnectivityMonitor::describeConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << "enabled=" << (cfg_.enabled ? "true" : "false")
        << " targets=" << cfg_.targets.size()
        << " interval=" << cfg_.interval_sec << "s"
        << " timeout=" << cfg_.timeout_sec << "s";
    return oss.str();
}

std::vector<weaknet::ProbeTargetResult> ActiveConnectivityMonitor::runProbeRound() {
    ActiveProbeConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = cfg_;
        ++round_counter_;
    }

    std::vector<weaknet::ProbeTargetResult> results;
    if (!cfg.enabled || cfg.targets.empty())
        return results;

    const std::string resolver = readConfiguredResolver();
    const auto now_ns = [] {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    };

    for (const auto& t : cfg.targets) {
        weaknet::ProbeTargetResult r;
        r.target_id = t.id;
        r.hostname = t.hostname;
        r.tcp_port = t.tcp_port;
        r.timestamp_ns = now_ns();

        // ---- DNS 阶段 ----
        r.dns.attempted = true;
        std::string addr, detail;
        const auto dns_t0 = std::chrono::steady_clock::now();
        const bool dns_ok = probeDnsA(resolver, t.hostname, cfg.timeout_sec, &addr, &detail);
        r.dns.latency_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - dns_t0).count() / 1000.0;
        r.dns.success = dns_ok;
        r.dns.detail = dns_ok ? ("resolved=" + addr) : detail;

        // 依赖截断：DNS 未成功时**不尝试** TCP。
        // 否则一次 DNS 故障会在四个维度上各产生一次"失败"，污染根因。
        if (!dns_ok) {
            r.tcp.attempted = false;
            r.tcp.detail = "blocked_by_dns";
            r.tls.attempted = false;
            r.tls.detail = "blocked_by_dns";
            r.http.attempted = false;
            r.http.detail = "blocked_by_dns";
            results.push_back(std::move(r));
            continue;
        }

        // ---- TCP 阶段 ----
        r.tcp.attempted = true;
        const auto tcp_t0 = std::chrono::steady_clock::now();
        std::string tcp_detail;
        const bool tcp_ok = probeTcpConnect(addr, t.tcp_port, cfg.timeout_sec, &tcp_detail);
        r.tcp.latency_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - tcp_t0).count() / 1000.0;
        r.tcp.success = tcp_ok;
        r.tcp.detail = tcp_detail;

        // ---- TLS / HTTP：本版本未实现（无 TLS 开发依赖）----
        // 明确保持 attempted=false，由 evaluator 表达为 NO_CAPABILITY。
        // 绝不因 TCP 成功就推断 HTTPS 可用。
        r.tls.attempted = false;
        r.tls.detail = tcp_ok ? "no_tls_probe_capability" : "blocked_by_tcp";
        r.http.attempted = false;
        r.http.detail = tcp_ok ? "no_tls_probe_capability" : "blocked_by_tcp";

        results.push_back(std::move(r));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_results_ = results;
    }
    return results;
}

std::vector<weaknet::ProbeTargetResult> ActiveConnectivityMonitor::results() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_results_;
}

}  // namespace weaknet_dbus
