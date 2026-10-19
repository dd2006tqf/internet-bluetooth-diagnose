/**
 * @file update_status.hpp
 * @brief OTA Phase 1 本地升级：版本比较 + manifest 校验（纯函数，无副作用）
 *
 * 设计约束：
 *   - 本头文件只放纯逻辑（版本解析/比较、manifest 行解析），不做任何 IO，
 *     以便 gtest 无需真机环境即可覆盖。
 *   - 涉及文件系统（读 current 软链接、读 manifest、算 sha256）的运行时
 *     查询由 DbusService::handleGetUpdateStatus 在 .cpp 侧组装，本头不管。
 *   - manifest 行格式与 sha256sum 文本输出兼容："<64位hex>  <相对路径>"。
 *     相对路径不得为绝对路径、不得含 ".."，否则解析拒绝（防路径穿越）。
 */

#pragma once

#include <string>

namespace weaknet {
namespace update {

/**
 * @brief 校验版本字符串是否为严格 X.Y.Z（每段为非负整数，无前导符号）
 * @param v 版本字符串，如 "1.0.0"
 * @return true 合法；false 非法（含空串、段数不对、非数字段）
 */
bool IsValidVersion(const std::string& v);

/**
 * @brief 数字分段比较两个版本（非字典序：1.10.0 > 1.2.0）
 * @param a, b 版本字符串；非法输入按 0.0.0 对待
 * @return -1(a<b) / 0(相等) / +1(a>b)
 */
int CompareVersions(const std::string& a, const std::string& b);

/**
 * @brief 升级策略：是否允许从 current 切到 target
 * @param current 当前版本；空串表示首次安装（无 current 软链接），恒允许
 * @param target  目标版本
 * @return true 允许（首次/同版幂等/升级）；false 拒绝（降级/非法目标）
 */
bool IsUpgradeAllowed(const std::string& current, const std::string& target);

/**
 * @brief 解析一行 manifest（"<sha256hex>  <relpath>"）
 * @param line 单行文本（不含换行）
 * @param sha_out [out] 64 位小写 hex
 * @param path_out [out] 相对路径
 * @return true 解析成功；false 拒绝（空行/注释/sha 非法/路径绝对或穿越）
 */
bool ParseManifestLine(const std::string& line, std::string* sha_out,
                       std::string* path_out);

/**
 * @brief 读 base_dir/current 软链接，提取 "releases/<ver>" 中的版本
 * @param base_dir 网关根目录（如 /home/radxa/weaknet）
 * @return 版本字符串；无链接/格式不合/非法版本时返回 ""（调用方视为 unmanaged）
 *
 * 只读，不触碰 data/ 下任何状态文件。
 */
std::string ReadCurrentVersion(const std::string& base_dir);

/**
 * @brief 读 release 目录里的 VERSION 文件（版本单一来源）
 * @param release_dir 如 base_dir/releases/1.2.3
 * @return 版本字符串（去空白）；文件缺失或内容非法时返回 ""
 */
std::string ReadReleaseVersion(const std::string& release_dir);

/**
 * @brief 完整性校验报告
 */
struct IntegrityReport {
    enum Capability { OK, NO_CAPABILITY };
    Capability capability = NO_CAPABILITY;  ///< 构建未带 OpenSSL 时为 NO_CAPABILITY
    int checked = 0;   ///< 清单中实际校验的条目数
    int failed = 0;    ///< 哈希不匹配或文件缺失的条目数
};

/**
 * @brief 按 release 目录内 manifest.sha256 校验全部文件
 * @param release_dir release 目录（含 manifest.sha256）
 * @return 见 IntegrityReport；无清单时 checked==0（未定标，不判失败）
 *
 * 只读，不修改任何文件。manifest 行复用 ParseManifestLine 的安全约束。
 */
IntegrityReport VerifyReleaseIntegrity(const std::string& release_dir);

/**
 * @brief 组装 GetUpdateStatus 查询 JSON（只读）
 * @param base_dir 网关根目录
 * @return 如 {"installed_version":"1.0.0","layout":"releases",
 *             "pending_staged_version":"","current_committed":true}；
 *         未迁移的扁平布局返回 installed_version "unmanaged", layout "flat"。
 */
std::string StatusJson(const std::string& base_dir);

}  // namespace update
}  // namespace weaknet
