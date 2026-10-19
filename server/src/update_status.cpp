/**
 * @file update_status.cpp
 * @brief OTA Phase 1 本地升级：版本比较 + manifest 校验（纯函数实现）
 *
 * 实现要点：
 *   - 版本分段用 strtol 逐段解析，段数必须恰为 3，每段只允许纯数字；
 *     拒绝前导 +/- 与空段，避免 strtol 的宽松语义漏网。
 *   - CompareVersions 对非法输入按 0.0.0 对待：调用方（升级策略）另行用
 *     IsValidVersion 把关，这里不返回错误码，保持比较全序。
 *   - ParseManifestLine 兼容 sha256sum 文本格式（sha + 空白 + 路径），
 *     但路径白名单收紧：非空、非绝对、逐段不为 ".."，防解包路径穿越。
 *   - 文件系统查询（ReadCurrentVersion / StatusJson）与纯解析分离，
 *     前者只读 current 软链接与 manifest，不触碰 data/ 状态文件。
 */

#include "update_status.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "utils/json_escape.hpp"

namespace weaknet {
namespace update {
namespace {

// 解析 "X.Y.Z" 为三段 long；成功返回 true。out 至少 3 元。
bool ParseVersionParts(const std::string& v, long out[3]) {
    if (v.empty()) return false;
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        if (pos >= v.size()) return false;
        size_t dot = v.find('.', pos);
        std::string part = (dot == std::string::npos) ? v.substr(pos)
                                                      : v.substr(pos, dot - pos);
        if (part.empty()) return false;
        for (char c : part) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        char* end = nullptr;
        long val = std::strtol(part.c_str(), &end, 10);
        if (end == part.c_str() || *end != '\0') return false;
        out[i] = val;
        if (dot == std::string::npos) {
            // 第三段后必须结束；提前结束（段数不足）拒绝
            if (i != 2) return false;
            pos = v.size();
        } else {
            // 第三段内不应再有点（拒绝 "1.0.0.0"）
            if (i == 2) return false;
            pos = dot + 1;
        }
    }
    return pos == v.size();
}

bool IsHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

// 相对路径安全检查：非空、非绝对、逐段不为 ".."（含 "a/../b" 这类中间穿越）。
bool IsSafeRelPath(const std::string& p) {
    if (p.empty() || p[0] == '/') return false;
    size_t pos = 0;
    while (pos <= p.size()) {
        size_t slash = p.find('/', pos);
        std::string seg = (slash == std::string::npos) ? p.substr(pos)
                                                       : p.substr(pos, slash - pos);
        if (seg == "..") return false;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

// 读 current 软链接目标，提取版本段 "releases/<ver>"；无链接或格式不合返回 ""。
std::string ReadlinkVersion(const std::string& link_path) {
    char buf[1024];
    const ssize_t n = ::readlink(link_path.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string target(buf);
    // 接受 "releases/1.2.3" 或绝对路径 "/…/releases/1.2.3"；取最后两段
    const std::string marker = "releases/";
    const size_t at = target.rfind(marker);
    if (at == std::string::npos) return "";
    std::string ver = target.substr(at + marker.size());
    // 剥掉可能的路径分隔符（目标目录名应为纯版本）
    if (ver.find('/') != std::string::npos) return "";
    return IsValidVersion(ver) ? ver : "";
}

// 读 manifest.json 第一行的 "version":"x.y.z"（极简提取，manifest 为发布时生成，
// 无第三方 JSON 依赖；找不到返回空）。
std::string ManifestVersion(const std::string& manifest_path) {
    std::ifstream in(manifest_path);
    if (!in) return "";
    std::string line;
    std::getline(in, line);
    const std::string key = "\"version\":\"";
    const size_t at = line.find(key);
    if (at == std::string::npos) return "";
    const size_t vs = at + key.size();
    const size_t ve = line.find('"', vs);
    if (ve == std::string::npos) return "";
    return line.substr(vs, ve - vs);
}

}  // namespace

bool IsValidVersion(const std::string& v) {
    long parts[3] = {0, 0, 0};
    return ParseVersionParts(v, parts);
}

int CompareVersions(const std::string& a, const std::string& b) {
    long pa[3] = {0, 0, 0};
    long pb[3] = {0, 0, 0};
    ParseVersionParts(a, pa);  // 非法按 0.0.0 对待
    ParseVersionParts(b, pb);
    for (int i = 0; i < 3; ++i) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return 1;
    }
    return 0;
}

bool IsUpgradeAllowed(const std::string& current, const std::string& target) {
    if (!IsValidVersion(target)) return false;
    if (current.empty()) return true;  // 首次安装
    if (!IsValidVersion(current)) return false;
    return CompareVersions(target, current) >= 0;  // 同版幂等 + 升级
}

bool ParseManifestLine(const std::string& line, std::string* sha_out,
                       std::string* path_out) {
    if (!sha_out || !path_out) return false;
    if (line.empty() || line[0] == '#') return false;
    // sha256sum 格式：sha + 空白 + 路径（单空格或双空格都接受）
    const size_t sp = line.find(' ');
    if (sp == std::string::npos) return false;
    const std::string sha = line.substr(0, sp);
    size_t ps = sp;
    while (ps < line.size() && line[ps] == ' ') ++ps;
    const std::string path = line.substr(ps);
    if (!IsHex64(sha)) return false;
    if (!IsSafeRelPath(path)) return false;
    *sha_out = sha;
    *path_out = path;
    return true;
}

std::string ReadCurrentVersion(const std::string& base_dir) {
    return ReadlinkVersion(base_dir + "/current");
}

// 去首尾空白（含换行）
std::string Trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() &&
           (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    size_t e = s.size();
    while (e > b &&
           (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
            s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

std::string ReadReleaseVersion(const std::string& release_dir) {
    std::ifstream in(release_dir + "/VERSION");
    if (!in) return "";
    std::string line;
    std::getline(in, line);
    // 实现侧不依赖测试库：这里手工断言内容合法，测试验证此函数
    const std::string ver = Trim(line);
    return IsValidVersion(ver) ? ver : "";
}

#ifdef WEAKNET_HAVE_TLS
#include <openssl/evp.h>

// 流式 sha256 文件：返回小写 hex；失败返回 ""。
std::string FileSha256Hex(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    std::string out;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1) {
        char buf[8192];
        while (in) {
            in.read(buf, sizeof(buf));
            const std::streamsize n = in.gcount();
            if (n > 0 && EVP_DigestUpdate(ctx, buf, static_cast<size_t>(n)) != 1)
                break;
        }
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        if (in.eof() && EVP_DigestFinal_ex(ctx, digest, &len) == 1) {
            static const char* kHex = "0123456789abcdef";
            out.reserve(len * 2);
            for (unsigned int i = 0; i < len; ++i) {
                out.push_back(kHex[(digest[i] >> 4) & 0xF]);
                out.push_back(kHex[digest[i] & 0xF]);
            }
        }
    }
    EVP_MD_CTX_free(ctx);
    return out;
}
#endif  // WEAKNET_HAVE_TLS

IntegrityReport VerifyReleaseIntegrity(const std::string& release_dir) {
    IntegrityReport rep;
#ifndef WEAKNET_HAVE_TLS
    return rep;  // 无 OpenSSL 构建：退化为未定标，checked==0，不误报 failed
#else
    rep.capability = IntegrityReport::OK;
    std::ifstream man(release_dir + "/manifest.sha256");
    if (!man) return rep;  // 无清单：未定标
    std::string line;
    while (std::getline(man, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string want, rel;
        if (!ParseManifestLine(line, &want, &rel)) continue;  // 空行/注释跳过
        ++rep.checked;
        const std::string got = FileSha256Hex(release_dir + "/" + rel);
        // 大小写无关比较（sha256sum 输出小写，实现输出小写；容忍大写清单）
        std::string wl = want, gl = got;
        for (char& c : wl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : gl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (gl.empty() || gl != wl) ++rep.failed;
    }
    return rep;
#endif
}

std::string StatusJson(const std::string& base_dir) {
    const std::string cur = ReadCurrentVersion(base_dir);
    const std::string pending = ManifestVersion(base_dir + "/staging/manifest.json");
    std::ostringstream json;
    json << "{";
    json << "\"installed_version\":\""
         << weaknet_utils::escapeJsonString(cur.empty() ? "unmanaged" : cur)
         << "\",";
    json << "\"layout\":\""
         << (cur.empty() ? "flat" : "releases") << "\",";
    json << "\"pending_staged_version\":\""
         << weaknet_utils::escapeJsonString(pending) << "\",";
    // committed 标记：current 指向的 release 内存在 .committed 文件即视为已转正
    bool committed = false;
    if (!cur.empty()) {
        struct stat st;
        committed = (::stat((base_dir + "/releases/" + cur + "/.committed").c_str(),
                            &st) == 0);
    }
    json << "\"current_committed\":" << (committed ? "true" : "false");
    json << "}";
    return json.str();
}

}  // namespace update
}  // namespace weaknet
