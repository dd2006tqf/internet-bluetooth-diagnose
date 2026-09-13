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
#include "tls_probe_client.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

/**
 * @brief 非阻塞 connect + poll，带超时。
 *
 * @param out_fd 成功时返回**仍处连接状态**的 fd，由调用方负责 close。
 *               保留而非立即 close 是因为 TLS 阶段要在同一个连接上
 *               继续握手；重连会引入一次额外的握手 RTT 与状态差异。
 */
bool probeTcpConnectFd(const std::string& ip, uint16_t port, uint32_t timeout_sec,
                       int* out_fd, std::string* out_detail) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { *out_detail = std::string("socket: ") + std::strerror(errno); return false; }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    // TLS 阶段复用同一 fd，禁用 Nagle 以免小请求被延迟合并
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        *out_detail = "bad_ip";
        return false;
    }

    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (rc == 0) { *out_fd = fd; *out_detail = "connected"; return true; }
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
    *out_fd = fd;
    *out_detail = "connected";
    return true;
}

/// 仅验证建连能力（Portal 探测只需要一个已连接 fd，用不到 TLS）
bool probeTcpConnect(const std::string& ip, uint16_t port, uint32_t timeout_sec,
                     std::string* out_detail) {
    int fd = -1;
    std::string detail;
    const bool ok = probeTcpConnectFd(ip, port, timeout_sec, &fd, &detail);
    if (fd >= 0) ::close(fd);
    *out_detail = detail;
    return ok;
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
        r.failure_domain = t.failure_domain.empty() ? t.hostname : t.failure_domain;
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

        // ---- TCP 阶段（保留 fd 供 TLS 复用）----
        r.tcp.attempted = true;
        const auto tcp_t0 = std::chrono::steady_clock::now();
        std::string tcp_detail;
        int fd = -1;
        const bool tcp_ok = probeTcpConnectFd(addr, t.tcp_port, cfg.timeout_sec,
                                              &fd, &tcp_detail);
        r.tcp.latency_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - tcp_t0).count() / 1000.0;
        r.tcp.success = tcp_ok;
        r.tcp.detail = tcp_detail;

        if (!tcp_ok) {
            // 依赖截断：TCP 未成功则 TLS/HTTP 记为 blocked_by_tcp，
            // 而不是各自产生一次失败
            r.tls.attempted = false;
            r.tls.detail = "blocked_by_tcp";
            r.http.attempted = false;
            r.http.detail = "blocked_by_tcp";
            if (fd >= 0) ::close(fd);
            results.push_back(std::move(r));
            continue;
        }

        // ---- TLS + HTTP 阶段（HTTPS capability）----
        if (cfg.https_enabled) {
            TlsProbeClient::Config tc;
            tc.deadline_ms = cfg.timeout_sec * 1000;
            tc.use_tls = true;
            tc.path = "/";
            const auto tr = TlsProbeClient::run(fd, t.hostname, tc);

            r.tls.attempted = tr.tls_attempted;
            r.tls.success = tr.tls_ok;
            r.tls.latency_ms = tr.tls_ms;
            r.tls.detail = tr.tls_detail;
            r.tls_info.cert_verified = tr.cert_verified;
            r.tls_info.version = tr.tls_version;
            r.tls_info.cipher = tr.cipher;
            r.tls_info.peer_subject = tr.peer_subject;
            r.tls_info.issuer = tr.issuer;

            r.http.attempted = tr.http_attempted;
            // http.success 表示"得到可解析的 HTTP 响应"，与状态码无关。
            // 404/500 同样是合法响应，证明 transport 存在。
            r.http.success = tr.http_ok;
            r.http.latency_ms = tr.http_ms;
            r.http.detail = tr.http_detail;
            r.http_info.status_code = tr.status_code;
            r.http_info.location = tr.location;
            r.http_info.server_header = tr.server_header;
            r.http_info.content_type = tr.content_type;
        } else {
            r.tls.attempted = false;
            r.tls.detail = "https_probe_disabled";
            r.http.attempted = false;
            r.http.detail = "https_probe_disabled";
        }

        if (fd >= 0) ::close(fd);
        results.push_back(std::move(r));
    }

    // ---- Captive Portal oracle（独立一轮，明文 HTTP）----
    std::vector<weaknet::ProbeTargetResult> portal_results;
    if (cfg.portal.enabled && !cfg.portal.targets.empty()) {
        portal_results = runPortalRound(cfg, resolver);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_results_ = results;
        last_portal_results_ = portal_results;
    }
    return results;
}

/**
 * @brief Captive Portal oracle 探测（明文 HTTP）
 *
 * 与 HTTPS 探测分开的原因：门户拦截通常发生在明文 HTTP 层，
 * 且 oracle 端点与 capability 目标不是同一批（前者必须响应已知）。
 *
 * 判定信号只在**受控端点**上产生：
 *   - 302/301 且 Location 指向非预期主机 → REDIRECTED（门户典型行为）
 *   - 状态码正常但正文不含预期子串     → CONTENT_MISMATCH
 *   - 其余                             → NONE
 *
 * 顶层 DNS 失败时不计入：底层不通时不得产生门户语义。
 */
std::vector<weaknet::ProbeTargetResult>
ActiveConnectivityMonitor::runPortalRound(const ActiveProbeConfig& cfg,
                                          const std::string& resolver) {
    std::vector<weaknet::ProbeTargetResult> out;
    const auto now_ns = [] {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    };

    for (const auto& t : cfg.portal.targets) {
        weaknet::ProbeTargetResult r;
        r.target_id = t.id;
        r.hostname = t.hostname;
        r.tcp_port = t.tcp_port;
        r.failure_domain = t.failure_domain.empty() ? t.hostname : t.failure_domain;
        r.timestamp_ns = now_ns();

        std::string addr, detail;
        if (!probeDnsA(resolver, t.hostname, cfg.timeout_sec, &addr, &detail)) {
            // 底层 DNS 不通：不产生门户信号（交给底层 SLE 解释）
            r.http_info.portal_signal = weaknet::PortalSignal::NOT_PROBED;
            out.push_back(std::move(r));
            continue;
        }

        int fd = -1;
        std::string tcp_detail;
        if (!probeTcpConnectFd(addr, t.tcp_port, cfg.timeout_sec, &fd, &tcp_detail)) {
            r.http_info.portal_signal = weaknet::PortalSignal::NOT_PROBED;
            out.push_back(std::move(r));
            continue;
        }

        TlsProbeClient::Config tc;
        tc.deadline_ms = cfg.timeout_sec * 1000;
        tc.use_tls = false;                 // 门户 oracle 走明文
        // 路径与预期正文支持按目标覆盖（不同厂商端点本就不一致）
        const std::string path = !t.http_path.empty()
            ? t.http_path
            : (cfg.portal.path.empty() ? std::string("/") : cfg.portal.path);
        // expect_body 的三态语义：必须能表达"该目标不比对正文"（如
        // generate_204 的正确判据是状态码 204，正文为空）。
        //   显式声明 → 用声明值
        //   显式留空（字段存在但为空）→ 不比对正文，由状态码判定
        //   字段不存在（无第 6 段）→ 回落全局值
        // 不能简单地"空即回落"：那会让 generate_204 被套上文本预期，
        // 从而永远判 CONTENT_MISMATCH（真机实测过的错误行为）。
        const std::string expect_body = t.expect_body_specified
            ? t.expect_body
            : cfg.portal.expect_body;
        tc.path = path;
        const auto tr = TlsProbeClient::run(fd, t.hostname, tc);
        ::close(fd);

        r.http.attempted = tr.http_attempted;
        r.http.success = tr.http_ok;
        r.http.latency_ms = tr.http_ms;
        r.http.detail = tr.http_detail;
        r.http_info.status_code = tr.status_code;
        r.http_info.location = tr.location;
        r.http_info.server_header = tr.server_header;
        r.http_info.content_type = tr.content_type;

        if (!tr.http_ok) {
            // 拿不到响应：无法断定是否被门户拦截，不产生信号
            r.http_info.portal_signal = weaknet::PortalSignal::NOT_PROBED;
            out.push_back(std::move(r));
            continue;
        }

        // ---- 一致信号判定 ----
        // 重定向到非预期主机
        const bool is_redirect = (tr.status_code == 301 || tr.status_code == 302
                                  || tr.status_code == 303 || tr.status_code == 307
                                  || tr.status_code == 308);
        if (is_redirect && !tr.location.empty()) {
            const bool points_elsewhere = tr.location.find(t.hostname) == std::string::npos
                                          && tr.location.rfind("/", 0) != 0;
            r.http_info.portal_signal = points_elsewhere
                ? weaknet::PortalSignal::REDIRECTED
                : weaknet::PortalSignal::NONE;
        } else if (!expect_body.empty()) {
            // 正文指纹比对（大小写敏感，见 ActiveProbeTargetConfig::expect_body）
            const bool matched = tr.body_snippet.find(expect_body) != std::string::npos;
            r.http_info.body_matches_expected = matched;
            if (matched) {
                r.http_info.portal_signal = weaknet::PortalSignal::NONE;
            } else if (tr.status_code >= 200 && tr.status_code < 400) {
                // 状态码正常但内容不是预期 —— 内容被替换
                r.http_info.portal_signal = weaknet::PortalSignal::CONTENT_MISMATCH;
            } else {
                // 5xx 等：endpoint 自身故障，不是门户信号
                r.http_info.portal_signal = weaknet::PortalSignal::NOT_PROBED;
            }
        } else if (t.expect_body_specified) {
            // 显式声明"不比对正文"，例如 generate_204：
            // 正确判据是**状态码本身**（204 = 未被拦截）。
            // 此时任何非 2xx/3xx 都是异常；被门户拦截通常会得到
            // 200 + 门户页面（被上游重定向分支捕获）或非 204。
            if (tr.status_code == 204 || tr.status_code == 200) {
                r.http_info.portal_signal = weaknet::PortalSignal::NONE;
            } else if (tr.status_code >= 200 && tr.status_code < 400) {
                r.http_info.portal_signal = weaknet::PortalSignal::CONTENT_MISMATCH;
            } else {
                r.http_info.portal_signal = weaknet::PortalSignal::NOT_PROBED;
            }
        } else {
            // 未配置任何预期（字段缺失）：只有明确的重定向才算信号，否则不推测
            r.http_info.portal_signal = weaknet::PortalSignal::NONE;
        }

        if (r.http_info.portal_signal == weaknet::PortalSignal::REDIRECTED
            || r.http_info.portal_signal == weaknet::PortalSignal::CONTENT_MISMATCH) {
            r.portal_detected = true;
        }

        LOG_INFO(LogModule::NETWORK, "Portal oracle: target=" << t.id
                 << " host=" << t.hostname
                 << " http_ok=" << (tr.http_ok ? "true" : "false")
                 << " status=" << tr.status_code
                 << " detail=" << tr.http_detail
                 << " signal=" << static_cast<int>(r.http_info.portal_signal)
                 << " body_len=" << tr.body_snippet.size());

        out.push_back(std::move(r));
    }
    return out;
}

std::vector<weaknet::ProbeTargetResult> ActiveConnectivityMonitor::portalResults() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_portal_results_;
}

bool ActiveConnectivityMonitor::tlsAvailable() {
    return TlsProbeClient::available();
}

std::vector<weaknet::ProbeTargetResult> ActiveConnectivityMonitor::results() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_results_;
}

}  // namespace weaknet_dbus
