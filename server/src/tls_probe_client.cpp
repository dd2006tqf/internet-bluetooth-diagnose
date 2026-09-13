/**
 * @file tls_probe_client.cpp
 * @brief TLS/HTTP 探测客户端实现
 *
 * ## 实现要点
 *
 * 1. **不拥有 fd**：调用方 connect 并负责 close。本文件只把 fd 交给
 *    SSL_set_fd，SSL_free 不会关闭它。
 *
 * 2. **非阻塞 + 显式 deadline**：fd 由调用方设为非阻塞。所有等待都走
 *    poll() 并受同一 deadline 约束，绝不出现无限期阻塞。
 *
 * 3. **一条 HTTP 解析路径**：TLS 与明文共用同一套 request 构造与
 *    response 解析（通过 Transport 抽象），避免两份解析逻辑漂移。
 *    明文路径供 Captive Portal 的受控 oracle 使用。
 *
 * 4. **证书校验分层表达**：证书链/主机名校验失败与握手失败分别记为
 *    cert_verify_failed 与 tls_handshake_failed。两者对"Internet 是否
 *    可用"的含义不同（前者可能是中间人/伪造，后者是协议层不可达），
 *    必须可区分。
 *
 * 5. **http_ok 与状态码解耦**：http_ok 表示"得到了可解析的 HTTP 响应"。
 *    404/500 同样是合法响应 —— 收到它就证明了 HTTPS transport 存在。
 *    把状态码纳入 http_ok 会让 endpoint 的业务语义伪装成网络故障。
 */

#include "tls_probe_client.hpp"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

// OpenSSL 1.1 vs 3.x 兼容：SSL_get_peer_certificate 在 3.0 被弃用为
// SSL_get1_peer_certificate（语义都是"引用计数 +1，调用方负责 free"）。
#ifdef WEAKNET_HAVE_TLS
#ifndef WEAKNET_SSL_GET_PEER_CERT
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#define WEAKNET_SSL_GET_PEER_CERT(s) SSL_get1_peer_certificate(s)
#else
#define WEAKNET_SSL_GET_PEER_CERT(s) SSL_get_peer_certificate(s)
#endif
#endif
#endif

#ifdef WEAKNET_HAVE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace weaknet_dbus {

namespace {

using Clock = std::chrono::steady_clock;

/// 读取上限：响应头 + 正文片段。防止异常响应导致无界内存增长。
constexpr size_t kMaxHeaderBytes = 8192;

double elapsedMs(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now() - t0).count() / 1000.0;
}

int remainingMs(Clock::time_point deadline) {
    const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(
                       deadline - Clock::now()).count();
    return d <= 0 ? 0 : static_cast<int>(d);
}

/// 等待 fd 就绪；返回 >0 就绪，0 超时，<0 错误
int waitFd(int fd, short events, int timeout_ms) {
    pollfd p{};
    p.fd = fd;
    p.events = events;
    int r;
    do {
        r = ::poll(&p, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    return r;
}

/// HTTP 请求与响应共用的一层传输抽象（TLS 或明文）
struct Transport {
    int fd{-1};
#ifdef WEAKNET_HAVE_TLS
    SSL* ssl{nullptr};
#endif

    bool isTls() const {
#ifdef WEAKNET_HAVE_TLS
        return ssl != nullptr;
#else
        return false;
#endif
    }

    /**
     * 写全部字节。
     * @return true 全部写完；false 出错或超时（detail 说明原因）
     */
    bool writeAll(const void* data, size_t len, Clock::time_point deadline,
                  std::string* detail) {
        const char* p = static_cast<const char*>(data);
        size_t sent = 0;
        while (sent < len) {
#ifdef WEAKNET_HAVE_TLS
            if (isTls()) {
                ERR_clear_error();
                const int r = SSL_write(ssl, p + sent, static_cast<int>(len - sent));
                if (r > 0) { sent += static_cast<size_t>(r); continue; }
                const int e = SSL_get_error(ssl, r);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                    const int t = remainingMs(deadline);
                    if (t <= 0) { *detail = "http_send_timeout"; return false; }
                    const int w = waitFd(fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, t);
                    if (w <= 0) { *detail = (w == 0) ? "http_send_timeout" : "send_poll_error"; return false; }
                    continue;
                }
                *detail = "http_send_failed";
                return false;
            }
#endif
            const ssize_t r = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
            if (r > 0) { sent += static_cast<size_t>(r); continue; }
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                const int t = remainingMs(deadline);
                if (t <= 0) { *detail = "http_send_timeout"; return false; }
                if (waitFd(fd, POLLOUT, t) <= 0) { *detail = "http_send_timeout"; return false; }
                continue;
            }
            *detail = std::string("http_send_failed: ") + std::strerror(errno);
            return false;
        }
        return true;
    }

    /**
     * 读一段。
     * @return >0 读到的字节数；0 EOF；-1 需重试或致命错误（detail 说明）
     */
    ssize_t readSome(void* buf, size_t len, Clock::time_point deadline,
                     std::string* detail) {
#ifdef WEAKNET_HAVE_TLS
        if (isTls()) {
            ERR_clear_error();
            const int r = SSL_read(ssl, buf, static_cast<int>(len));
            if (r > 0) return r;
            const int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_ZERO_RETURN) return 0;   // 对端正常关闭
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                const int t = remainingMs(deadline);
                if (t <= 0) { *detail = "http_read_timeout"; return -1; }
                const int w = waitFd(fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, t);
                if (w <= 0) { *detail = (w == 0) ? "http_read_timeout" : "read_poll_error"; return -1; }
                return -1;   // 让调用方重试
            }
            if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) return 0;  // 非干净 EOF
            *detail = "http_read_failed";
            return -1;
        }
#endif
        const ssize_t r = ::recv(fd, buf, len, 0);
        if (r >= 0) return r;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const int t = remainingMs(deadline);
            if (t <= 0) { *detail = "http_read_timeout"; return -1; }
            if (waitFd(fd, POLLIN, t) <= 0) { *detail = "http_read_timeout"; return -1; }
            return -1;   // 让调用方重试
        }
        // 其余 errno 是致命错误（ECONNRESET / ENOTCONN / EBADF …）。
        // 必须设置 detail —— 否则调用方无法区分"需重试"与"已失败"，
        // 会无限重试同一个坏 fd。这条曾被本客户端的单元测试捕获。
        *detail = std::string("http_read_failed: ") + std::strerror(errno);
        return -1;
    }
};

#ifdef WEAKNET_HAVE_TLS

/// 进程内一次性 OpenSSL 初始化（1.1.1 需要；3.x 下是 no-op）
void ensureOpenSslInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    });
}

/**
 * @brief 在已连接 fd 上完成 TLS 握手并校验证书
 *
 * 校验分两层：
 *   1. 链校验 —— SSL_CTX_set_default_verify_paths + SSL_VERIFY_PEER
 *   2. 主机名校验 —— SSL_set1_host（同时校验 CN 与 SAN，dNSName）
 *
 * 失败时区分 cert_verify_failed 与 tls_handshake_failed：
 * 前者是证书层面问题（自签/过期/域名不符/中间人），后者是协议层不可达。
 */
bool doHandshake(SSL* ssl, int fd, Clock::time_point deadline, bool verify,
                 std::string* detail) {
    for (;;) {
        ERR_clear_error();
        const int r = SSL_connect(ssl);
        if (r == 1) {
            if (verify && SSL_get_verify_result(ssl) != X509_V_OK) {
                *detail = "cert_verify_failed";
                return false;
            }
            *detail = "connected";
            return true;
        }
        const int e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            const int t = remainingMs(deadline);
            if (t <= 0) { *detail = "tls_timeout"; return false; }
            const int w = waitFd(fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, t);
            if (w <= 0) { *detail = (w == 0) ? "tls_timeout" : "tls_poll_error"; return false; }
            continue;
        }
        // 握手已中断：若校验结果非 OK，则根因是证书
        if (verify && SSL_get_verify_result(ssl) != X509_V_OK) {
            *detail = "cert_verify_failed";
            return false;
        }
        if (e == SSL_ERROR_SSL) {
            const unsigned long err = ERR_peek_last_error();
            char ebuf[256] = {};
            ERR_error_string_n(err, ebuf, sizeof(ebuf));
            *detail = std::string("tls_handshake_failed: ") + ebuf;
        } else {
            *detail = "tls_handshake_failed";
        }
        return false;
    }
}

#endif  // WEAKNET_HAVE_TLS

/// case-insensitive 取头部值（不含前导空格）
std::string findHeader(const std::string& headers, const std::string& name) {
    std::string lower_headers = headers;
    for (char& c : lower_headers) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    std::string needle = name;
    for (char& c : needle) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    needle += ':';

    size_t pos = 0;
    while (pos < lower_headers.size()) {
        const size_t eol = lower_headers.find("\r\n", pos);
        if (eol == std::string::npos) break;
        if (lower_headers.compare(pos, needle.size(), needle) == 0) {
            size_t v = pos + needle.size();
            while (v < eol && (headers[v] == ' ' || headers[v] == '\t')) ++v;
            return headers.substr(v, eol - v);
        }
        pos = eol + 2;
    }
    return std::string();
}

}  // namespace

bool TlsProbeClient::available() {
#ifdef WEAKNET_HAVE_TLS
    return true;
#else
    return false;
#endif
}

TlsProbeClient::Result TlsProbeClient::run(int fd, const std::string& sni_hostname,
                                           const Config& cfg) {
    Result out;
    const auto deadline = Clock::now() + std::chrono::milliseconds(cfg.deadline_ms);

    Transport tr;
    tr.fd = fd;

#ifdef WEAKNET_HAVE_TLS
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    // RAII 清理：任何提前返回路径都不泄漏 OpenSSL 对象
    struct Guard {
        SSL_CTX** ctx;
        SSL** ssl;
        ~Guard() {
            if (*ssl) SSL_free(*ssl);
            if (*ctx) SSL_CTX_free(*ctx);
        }
    } guard{&ctx, &ssl};
#endif

    // ---- TLS 阶段 ----
    if (cfg.use_tls) {
#ifdef WEAKNET_HAVE_TLS
        ensureOpenSslInit();
        out.tls_attempted = true;

        const auto t0 = Clock::now();
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            out.tls_detail = "ssl_ctx_new_failed";
            out.tls_ms = elapsedMs(t0);
            return out;
        }
        // 最低 TLS 1.2：1.0/1.1 已被主流服务端淘汰，且探测不应降级到不安全协议
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

        if (cfg.verify_peer) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            if (!cfg.ca_file.empty()) {
                if (SSL_CTX_load_verify_locations(ctx, cfg.ca_file.c_str(), nullptr) != 1) {
                    out.tls_detail = "ca_file_load_failed";
                    out.tls_ms = elapsedMs(t0);
                    return out;
                }
            } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
                // 系统 trust store 不可用：这是本机配置问题，不是目标故障。
                // 如实报告，由 evaluator 决定语义。
                out.tls_detail = "no_system_trust_store";
                out.tls_ms = elapsedMs(t0);
                return out;
            }
        } else {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        }

        ssl = SSL_new(ctx);
        if (!ssl) {
            out.tls_detail = "ssl_new_failed";
            out.tls_ms = elapsedMs(t0);
            return out;
        }
        if (SSL_set_fd(ssl, fd) != 1) {
            out.tls_detail = "ssl_set_fd_failed";
            out.tls_ms = elapsedMs(t0);
            return out;
        }
        // SNI：虚拟主机与 CDN 必须靠它选中正确证书
        if (!sni_hostname.empty()) {
            SSL_set_tlsext_host_name(ssl, sni_hostname.c_str());
        }
        // 主机名校验：同时覆盖 CN 与 SAN 的 dNSName
        if (cfg.verify_peer && !sni_hostname.empty()) {
            X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
            X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            if (X509_VERIFY_PARAM_set1_host(param, sni_hostname.c_str(), 0) != 1) {
                out.tls_detail = "host_verify_param_failed";
                out.tls_ms = elapsedMs(t0);
                return out;
            }
        }

        std::string hs_detail;
        out.tls_ok = doHandshake(ssl, fd, deadline, cfg.verify_peer, &hs_detail);
        out.tls_detail = hs_detail;
        out.tls_ms = elapsedMs(t0);

        if (!out.tls_ok) {
            return out;   // 握手失败：不进入 HTTP 阶段（依赖截断由 evaluator 表达）
        }

        tr.ssl = ssl;

        out.cert_verified = (SSL_get_verify_result(ssl) == X509_V_OK);
        if (const char* v = SSL_get_version(ssl)) out.tls_version = v;
        if (const SSL_CIPHER* c = SSL_get_current_cipher(ssl)) {
            if (const char* n = SSL_CIPHER_get_name(c)) out.cipher = n;
        }
        if (X509* cert = WEAKNET_SSL_GET_PEER_CERT(ssl)) {
            char subj[256] = {};
            char iss[256] = {};
            if (X509_NAME_oneline(X509_get_subject_name(cert), subj, sizeof(subj)))
                out.peer_subject = subj;
            if (X509_NAME_oneline(X509_get_issuer_name(cert), iss, sizeof(iss)))
                out.issuer = iss;
            X509_free(cert);
        }
#else
        out.tls_attempted = false;
        out.tls_detail = "no_tls_probe_capability";
        return out;
#endif
    }

    // ---- HTTP 阶段 ----
    out.http_attempted = true;
    const auto ht0 = Clock::now();

    std::ostringstream req;
    const std::string host = cfg.host_header.empty() ? sni_hostname : cfg.host_header;
    req << "GET " << (cfg.path.empty() ? "/" : cfg.path) << " HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "User-Agent: weaknet-probe/1.0\r\n"
        << "Accept: */*\r\n"
        << "Connection: close\r\n\r\n";
    const std::string request = req.str();

    std::string wdetail;
    if (!tr.writeAll(request.data(), request.size(), deadline, &wdetail)) {
        out.http_detail = wdetail;
        out.http_ms = elapsedMs(ht0);
        return out;
    }

    // 读取：先攒够响应头，再按 body_limit 收正文
    std::string raw;
    raw.reserve(kMaxHeaderBytes + cfg.body_limit);
    char buf[4096];
    size_t header_end = std::string::npos;
    const size_t read_cap = kMaxHeaderBytes + cfg.body_limit;

    while (raw.size() < read_cap) {
        std::string rdetail;
        const ssize_t n = tr.readSome(buf, sizeof(buf), deadline, &rdetail);
        if (n > 0) {
            raw.append(buf, static_cast<size_t>(n));
            if (header_end == std::string::npos) {
                header_end = raw.find("\r\n\r\n");
            }
            // 头部已完整且正文已达上限即停
            if (header_end != std::string::npos
                && raw.size() - (header_end + 4) >= cfg.body_limit) {
                break;
            }
            continue;
        }
        if (n == 0) break;                       // EOF：正常（Connection: close）
        if (rdetail == "http_read_timeout") {
            // 已经拿到完整头部时，超时不算失败：正文不完整不影响 transport 判定
            if (header_end != std::string::npos) break;
            out.http_detail = "http_read_timeout";
            out.http_ms = elapsedMs(ht0);
            return out;
        }
        // 重试路径（poll 醒了但还不可读）继续循环；致命错误则退出
        if (rdetail == "read_poll_error" || rdetail == "http_read_failed") {
            if (header_end != std::string::npos) break;
            out.http_detail = rdetail;
            out.http_ms = elapsedMs(ht0);
            return out;
        }
    }

    // ---- 解析 ----
    if (raw.empty()) {
        out.http_detail = "empty_response";
        out.http_ms = elapsedMs(ht0);
        return out;
    }

    const size_t status_eol = raw.find("\r\n");
    const std::string status_line = raw.substr(0, status_eol == std::string::npos ? raw.size() : status_eol);
    // "HTTP/1.1 200 OK"
    const size_t sp = status_line.find(' ');
    if (status_line.compare(0, 5, "HTTP/") != 0 || sp == std::string::npos || sp + 4 > status_line.size()) {
        out.http_detail = "malformed_status_line";
        out.http_ms = elapsedMs(ht0);
        return out;
    }
    out.status_code = std::atoi(status_line.c_str() + sp + 1);
    if (out.status_code < 100 || out.status_code > 599) {
        out.http_detail = "malformed_status_line";
        out.http_ms = elapsedMs(ht0);
        return out;
    }

    const std::string headers =
        (header_end == std::string::npos) ? raw : raw.substr(0, header_end + 2);
    out.location = findHeader(headers, "Location");
    out.server_header = findHeader(headers, "Server");
    out.content_type = findHeader(headers, "Content-Type");
    if (const std::string cl = findHeader(headers, "Content-Length"); !cl.empty()) {
        out.body_bytes = static_cast<size_t>(std::strtoul(cl.c_str(), nullptr, 10));
    }

    if (header_end != std::string::npos) {
        const size_t body_start = header_end + 4;
        const size_t avail = raw.size() > body_start ? raw.size() - body_start : 0;
        out.body_snippet = raw.substr(body_start, avail < cfg.body_limit ? avail : cfg.body_limit);
    }
    if (out.body_bytes == 0) out.body_bytes = out.body_snippet.size();

    // 得到可解析的 HTTP 响应 —— 与状态码无关。
    // 404/500 同样是合法响应，证明 transport 存在。
    out.http_ok = true;
    out.http_detail = "http_response_received";
    out.http_ms = elapsedMs(ht0);
    return out;
}

}  // namespace weaknet_dbus
