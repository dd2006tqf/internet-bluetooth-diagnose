// test_action_catalog_version_gtest.cpp
// 动作目录指纹的跨端一致性回归测试
// Module under test: assurance/action_registry.hpp / action_registry.cpp
//
// 为什么需要它
// ------------
// 这个指纹是跨端比较的：网关自述的值必须与云端
// `edge_action_catalog.catalog_version()` 完全相等，否则 Policy 会把每一个
// 动作都判定为"部署版本漂移"并 fail-closed —— 后果是整片现场无法下发任何
// 调参动作。因此两端必须锁在同一金标上；任何一侧改了序列化规则、白名单或
// 动作目录，下面的断言都会立刻失败，而不是等到真机上才发现全线 403。
//
// 注意：本测试**不重复**云端断言（云端有自己的 pytest）。这里钉的是
// "C++ 侧确实产出与契约一致的那一串"。

#include <gtest/gtest.h>

#include <string>

#include "assurance/action_registry.hpp"
#include "weaknet_config.hpp"  // trialableKeyNames()

namespace {

const weaknet::ActionRegistry& registry() {
    return weaknet::ActionRegistry::defaultInstance();
}

}  // namespace

// 与云端 edge_action_catalog.TOP_LEVEL_CATALOG_VERSION 相同的金标值。
// 改动序列化规则或目录/白名单内容时，两端必须同时更新，否则跨端比较失效。
TEST(ActionCatalogVersion, MatchesCrossEndGoldenVector) {
    EXPECT_EQ(weaknet::actionCatalogVersion(registry()), "bdb093ac5310");
}

TEST(ActionCatalogVersion, IsTwelveHexChars) {
    const std::string version = weaknet::actionCatalogVersion(registry());
    ASSERT_EQ(version.size(), 12u);
    for (char c : version) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
            << "非十六进制字符: " << c;
    }
}

// 规范载荷必须精确等于云端 json.dumps(sort_keys=True, separators=(",",":"))
// 的产物。这里断言结构骨架：字段顺序、无空白、config_keys 升序。
// （云端侧由 pytest 断言同一字符串，两端一致才有意义。）
TEST(ActionCatalogVersion, CanonicalPayloadIsStableAndOrdered) {
    const std::string payload = weaknet::actionCatalogCanonicalPayload(registry());

    // 无空白：任何空格都意味着与云端 separators 约定分叉
    EXPECT_EQ(payload.find(' '), std::string::npos);

    // 顶层键序固定：actions 在前、config_keys 在后
    const size_t actions_at = payload.find("\"actions\"");
    const size_t config_at = payload.find("\"config_keys\"");
    ASSERT_NE(actions_at, std::string::npos);
    ASSERT_NE(config_at, std::string::npos);
    EXPECT_LT(actions_at, config_at);

    // 动作按 action_id 升序（std::map 天然有序，这里钉住不被改成无序容器）
    const size_t check_at = payload.find("\"CHECK_RESOLVER_CONFIG\"");
    const size_t inspect_at = payload.find("\"INSPECT_DEFAULT_GATEWAY\"");
    const size_t probe_at = payload.find("\"PROBE_PUBLIC_RESOLVER\"");
    const size_t restart_at = payload.find("\"RESTART_NETWORK_INTERFACE\"");
    ASSERT_NE(check_at, std::string::npos);
    ASSERT_NE(inspect_at, std::string::npos);
    ASSERT_NE(probe_at, std::string::npos);
    ASSERT_NE(restart_at, std::string::npos);
    EXPECT_LT(check_at, inspect_at);
    EXPECT_LT(inspect_at, probe_at);
    EXPECT_LT(probe_at, restart_at);

    // 空参数动作必须序列化为 "params":[]（不是缺字段、不是 null）
    EXPECT_NE(payload.find("\"CHECK_RESOLVER_CONFIG\":{\"params\":[]}"),
              std::string::npos);

    // param 字段序固定为 allowed/name/required/type
    const size_t allowed_at = payload.find("\"allowed\":[\"223.5.5.5\"");
    ASSERT_NE(allowed_at, std::string::npos);
    EXPECT_LT(allowed_at, payload.find("\"name\":\"resolver\"", allowed_at));
    EXPECT_LT(payload.find("\"name\":\"resolver\"", allowed_at),
              payload.find("\"required\":true", allowed_at));
    EXPECT_LT(payload.find("\"required\":true", allowed_at),
              payload.find("\"type\":\"ipv4_address\"", allowed_at));
}

// 白名单必须整体可枚举且已排序 —— 指纹要覆盖"网关能改哪些键"，
// 顺序若不稳定，两端就会算出不同指纹。
TEST(TrialableKeys, ExposedSortedAndNonEmpty) {
    const auto& keys = weaknet_dbus::trialableKeyNames();
    ASSERT_FALSE(keys.empty());
    for (size_t i = 1; i < keys.size(); ++i) {
        EXPECT_LT(keys[i - 1], keys[i]) << "白名单未按字节序升序（或存在重复）";
    }
    // 与云端 CONFIG_KEYS 的规模对应；数量变化必须两端同步
    EXPECT_EQ(keys.size(), 39u);
}
