/**
 * @file action_registry.cpp
 * @brief 动作目录内容指纹（跨端部署版本漂移检测）。
 *
 * 为什么需要它
 * ------------
 * `action_policy` 早已实现「云端审阅目录 != 网关宣称目录 → allowed=False」的
 * fail-closed 语义，但真实运行中**永不触发**：云端从不设置该值，网关也从不上报。
 * 本文件提供网关侧的"我声称自己能执行什么"指纹，让这条防线具备输入源
 * （见 docs/网关动作目录版本契约.md）。
 *
 * 指纹必须跨端可比，因此规范 JSON 的每一个字节都被钉死：
 *   - 顶层键序：actions 在前、config_keys 在后；
 *   - actions 按 action_id 升序（std::map 天然有序）；
 *   - 每个动作仅含 params；param 字段序固定为 allowed/name/required/type；
 *   - config_keys 按字节序升序；
 *   - 分隔符无空白（"," 与 ":"）。
 *
 * 该形态等价于云端 `json.dumps(payload, sort_keys=True, separators=(",", ":"))`。
 * 规范载荷只含 ASCII（action_id / 参数名 / 类型 / 白名单键 / IP 字面量），
 * 刻意**不含** description 与 executable——它们已是中文或绝对路径，纳入会让
 * 两端转义规则（Python ensure_ascii 与手工 UTF-8 透传）产生分歧。
 *
 * 金标：`bdb093ac5310`（两端必须一致；见契约文档 §2.1）。
 */

#include "assurance/action_registry.hpp"

#include <openssl/evp.h>

#include <iomanip>
#include <sstream>

#include "utils/json_escape.hpp"
#include "weaknet_config.hpp"

namespace weaknet {

namespace {

/// 规范 JSON 里的一个字符串字面量（含外层引号）。
std::string jsonString(const std::string& raw) {
    return "\"" + weaknet_utils::escapeJsonString(raw) + "\"";
}

void appendParams(std::ostringstream& out, const std::vector<ActionParamSpec>& params) {
    out << "\"params\":[";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) out << ",";
        const auto& p = params[i];
        // 字段序：allowed, name, required, type（对应 Python sort_keys）
        out << "{\"allowed\":[";
        for (size_t j = 0; j < p.allowed_values.size(); ++j) {
            if (j > 0) out << ",";
            out << jsonString(p.allowed_values[j]);
        }
        out << "]," << "\"name\":" << jsonString(p.name)
            << ",\"required\":" << (p.required ? "true" : "false")
            << ",\"type\":" << jsonString(p.type) << "}";
    }
    out << "]";
}

/// 与云端 `edge_action_catalog._canonical_payload()` 逐字节等价的规范载荷。
std::string canonicalPayload(const ActionRegistry& registry) {
    std::ostringstream out;
    out << "{\"actions\":{";
    bool first_action = true;
    for (const auto& [action_id, def] : registry.actions()) {
        if (!first_action) out << ",";
        first_action = false;
        out << jsonString(action_id) << ":{" ;
        appendParams(out, def.param_specs);
        out << "}";
    }
    out << "},\"config_keys\":[";
    const auto& keys = weaknet_dbus::trialableKeyNames();
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) out << ",";
        out << jsonString(keys[i]);
    }
    out << "]}";
    return out.str();
}

std::string sha256Hex12(const std::string& payload) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) return {};
    std::string hex;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, payload.data(), payload.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, digest, &len) == 1) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        // 输出完整 64 字符十六进制后截前 12 —— 与云端 `hexdigest()[:12]` 等价。
        // 注意：是 12 个**字符**（6 字节），不是 12 字节（那会得到 24 字符）。
        for (unsigned int i = 0; i < len; ++i) {
            oss << std::setw(2) << static_cast<unsigned int>(digest[i]);
        }
        hex = oss.str().substr(0, 12);
    }
    EVP_MD_CTX_free(ctx);
    return hex;
}

}  // namespace

std::string actionCatalogVersion(const ActionRegistry& registry) {
    return sha256Hex12(canonicalPayload(registry));
}

std::string actionCatalogCanonicalPayload(const ActionRegistry& registry) {
    return canonicalPayload(registry);
}

}  // namespace weaknet
