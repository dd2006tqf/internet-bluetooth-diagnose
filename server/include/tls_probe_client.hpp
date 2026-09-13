#pragma once

/**
 * @file tls_probe_client.hpp
 * @brief TLS/HTTP 探测客户端 — 受控主动探测的加密层
 *
 * ## 职责边界（硬约束）
 *
 *   调用方（ActiveConnectivityMonitor）持有 socket 生命周期与超时策略，
 *   本类只接收**已连接的 fd**，在其上完成 TLS 握手与 HTTP 请求。
 *   本类不做任何 SLE 判定，也不构造 probe 目标。
 *
 *   这样切分的理由：connect 逻辑已存在于 monitor 中，重复实现会产生
 *   两套超时语义；而 TLS/HTTP 是本类唯一触及 OpenSSL 的地方，
 *   便于把 OpenSSL 的链接范围限制在 PRIVATE。
 *
 * ## 为什么同时支持明文 HTTP
 *
 *   Captive Portal 的判定 oracle 是"受控 connectivity-check 端点是否
 *   被重定向/替换内容"。门户拦截通常发生在明文 HTTP 层，
 *   因此同一个客户端需要能在 use_tls=false 下工作，
 *   复用同一条响应解析路径而不是再写一个 HTTP 解析器。
 *
 * ## 语义边界（与评价体系一致）
 *
 *   收到**任意合法 HTTP 状态码**即证明 HTTPS transport 可用。
 *   404/500 是 endpoint 自身的业务语义，不得据此判定 Internet 故障。
 *   因此 http_ok 的含义是"得到了可解析的 HTTP 响应"，而不是"状态码是 2xx"。
 *   是否构成负面证据由 evaluator 依据状态码分类决定。
 *
 * ## 无 TLS 依赖时的行为
 *
 *   整个实现由 WEAKNET_HAVE_TLS 包围。未定义时本类仍可编译，
 *   但 run() 返回 tls_attempted=false，由 evaluator 表达为
 *   NO_CAPABILITY（reason=no_tls_probe_capability），绝不伪造成功。
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace weaknet_dbus {

/**
 * @brief TLS/HTTP 探测客户端
 */
class TlsProbeClient {
public:
    struct Config {
        /// 整个 TLS+HTTP 过程的截止时间（毫秒）
        uint32_t deadline_ms{3000};
        /// true = 先 TLS 握手再发 HTTP；false = 明文 HTTP（Portal oracle 用）
        bool use_tls{true};
        /// HTTP 请求路径
        std::string path{"/"};
        /// Host: 头。空则使用 sni_hostname
        std::string host_header;
        /// 额外 CA bundle 文件；空则使用系统 trust store
        std::string ca_file;
        /// 响应体最多读取的字节数（Portal 指纹用）
        size_t body_limit{1024};
        /// 是否校验证书链与主机名（仅用于诊断场景关闭）
        bool verify_peer{true};
    };

    struct Result {
        // ---- TLS 阶段 ----
        bool tls_attempted{false};
        bool tls_ok{false};
        std::string tls_detail;      ///< connected / tls_handshake_failed / cert_verify_failed 等
        std::string tls_version;     ///< 如 TLSv1.3
        std::string cipher;
        std::string peer_subject;    ///< 叶证书 subject
        std::string issuer;          ///< 签发 CA
        bool cert_verified{false};
        double tls_ms{0.0};

        // ---- HTTP 阶段 ----
        bool http_attempted{false};
        bool http_ok{false};         ///< 得到可解析的 HTTP 响应（与状态码无关）
        int status_code{0};
        std::string location;        ///< Location 响应头（Portal 重定向判定）
        std::string server_header;
        std::string content_type;
        std::string body_snippet;    ///< 截断到 body_limit
        size_t body_bytes{0};
        double http_ms{0.0};
        std::string http_detail;
    };

    /**
     * @brief 在已连接的 socket 上执行 TLS（可选）与 HTTP 探测
     *
     * @param fd            已 connect 成功的 fd（调用方负责 close）
     * @param sni_hostname  用于 SNI 与主机名校验的服务名（通常是配置的 hostname）
     * @param cfg           超时与请求参数
     *
     * 阻塞语义：fd 应为非阻塞。本方法内部按 deadline 自旋等待，
     * 绝不会无限期阻塞。
     */
    static Result run(int fd, const std::string& sni_hostname, const Config& cfg);

    /// 运行期是否具备 TLS 能力（编译期决定，供 evaluator 与启动日志使用）
    static bool available();
};

}  // namespace weaknet_dbus
