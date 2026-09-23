// test_weaknet_config_gtest.cpp
// YAML 子集配置解析器单元测试
// Module under test: weaknet_config.hpp / weaknet_config.cpp

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "weaknet_config.hpp"

using namespace weaknet_dbus;

namespace {

// 写一个临时配置文件，测试结束自动清理
class ConfigFile {
public:
    explicit ConfigFile(const std::string& content) {
        path_ = "./test_cfg_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)) + ".yaml";
        std::ofstream f(path_);
        f << content;
    }
    ~ConfigFile() { std::error_code ec; std::filesystem::remove(path_, ec); }
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

// WeakNetConfig 含 mutex/atomic，不可拷贝移动，用出参填充
bool parse(const std::string& content, WeakNetConfig* out, std::string* err = nullptr) {
    ConfigFile file(content);
    std::string error;
    bool ok = loadWeakNetConfig(file.path(), out, &error);
    if (err) *err = error;
    return ok;
}

}  // namespace

// 缺省文件 → 全默认值，返回 true
TEST(WeakNetConfigTest, MissingFileKeepsDefaults) {
    WeakNetConfig cfg;
    std::string error;
    bool ok = loadWeakNetConfig("./no_such_config_file_xyz.yaml", &cfg, &error);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.rtt.target.get(), "223.5.5.5");
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 10000u);
    EXPECT_EQ(cfg.dns.bpf_obj.get(), "build/dns_monitor.bpf.o");
}

// 合法完整文件 → 各字段正确落位
TEST(WeakNetConfigTest, ValidFileParsesAllSections) {
    WeakNetConfig cfg;
    bool ok = parse(
        "server:\n"
        "  data_dir: /var/lib/weaknet\n"
        "  log_level: debug\n"
        "monitors:\n"
        "  rtt:\n"
        "    enabled: false\n"
        "    target: 8.8.8.8\n"
        "    interval: 5s\n"
        "    timeout: 500ms\n"
        "    window: 50\n"
        "  dns:\n"
        "    bpf_obj: /usr/lib/weaknet/dns_monitor.bpf.o\n",
        &cfg);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.data_dir.get(), "/var/lib/weaknet");
    EXPECT_EQ(cfg.log_level.get(), "debug");
    EXPECT_FALSE(cfg.rtt.enabled.load());
    EXPECT_EQ(cfg.rtt.target.get(), "8.8.8.8");
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 5000u);
    EXPECT_EQ(cfg.rtt.timeout_ms.load(), 500u);
    // jitter 已合并进 rtt：滑动窗口归 rtt 所有
    EXPECT_EQ(cfg.rtt.window_size.load(), 50u);
    EXPECT_EQ(cfg.dns.bpf_obj.get(), "/usr/lib/weaknet/dns_monitor.bpf.o");
    // 未覆盖字段保持默认
    EXPECT_EQ(cfg.traffic.interval_ms.load(), 10000u);
    EXPECT_EQ(cfg.quality.enabled.load(), true);
}

// 新命名空间键（interval_ms/timeout_ms/window_size）同样可解析
TEST(WeakNetConfigTest, NewNsKeysParseAlias) {
    WeakNetConfig cfg;
    bool ok = parse(
        "monitors:\n"
        "  rtt:\n"
        "    interval_ms: 7000\n"
        "    timeout_ms: 900\n"
        "    window_size: 20\n",
        &cfg);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 7000u);
    EXPECT_EQ(cfg.rtt.timeout_ms.load(), 900u);
    EXPECT_EQ(cfg.rtt.window_size.load(), 20u);
}

// setMonitorParam 新旧命名空间均可设置
TEST(WeakNetConfigTest, SetMonitorParamNsAlias) {
    WeakNetConfig cfg;
    std::string err;
    EXPECT_TRUE(setMonitorParam(&cfg, "rtt.interval_ms", "5s", &err));
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 5000u);
    EXPECT_TRUE(setMonitorParam(&cfg, "rtt.interval", "3s", &err));
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 3000u);
    EXPECT_TRUE(setMonitorParam(&cfg, "rtt.window_size", "40", &err));
    EXPECT_EQ(cfg.rtt.window_size.load(), 40u);
}

// 时长后缀解析：裸整数=ms、s、m
TEST(WeakNetConfigTest, DurationSuffixParsing) {
    WeakNetConfig cfg;
    bool ok = parse(
        "monitors:\n"
        "  rtt:\n"
        "    interval: 700\n"
        "  traffic:\n"
        "    interval: 2m\n",
        &cfg);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 700u);
    EXPECT_EQ(cfg.traffic.interval_ms.load(), 120000u);
}

// 注释（整行 + 行内）
TEST(WeakNetConfigTest, CommentsAreIgnored) {
    WeakNetConfig cfg;
    bool ok = parse(
        "# 顶层注释\n"
        "server:\n"
        "  data_dir: /tmp/x  # 行内注释\n"
        "monitors:\n"
        "  rtt:\n"
        "    target: 1.1.1.1\n",
        &cfg);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.data_dir.get(), "/tmp/x");
    EXPECT_EQ(cfg.rtt.target.get(), "1.1.1.1");
}

// 缺个别字段 → 该字段回落默认
TEST(WeakNetConfigTest, MissingFieldFallsBackToDefault) {
    WeakNetConfig cfg;
    bool ok = parse(
        "monitors:\n"
        "  rtt:\n"
        "    target: 114.114.114.114\n",
        &cfg);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.rtt.target.get(), "114.114.114.114");
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 10000u);  // 默认保留
}

// 未知 monitor / 未知字段 / 顶层裸键 / 值类型错误 → 报错
TEST(WeakNetConfigTest, UnknownMonitorRejected) {
    WeakNetConfig cfg;
    std::string err;
    bool ok = parse("monitors:\n  frobnicate:\n    enabled: true\n", &cfg, &err);
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("frobnicate"), std::string::npos);
}

TEST(WeakNetConfigTest, UnknownFieldRejected) {
    WeakNetConfig cfg;
    std::string err;
    bool ok = parse("monitors:\n  rtt:\n    magic: 42\n", &cfg, &err);
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("magic"), std::string::npos);
}

TEST(WeakNetConfigTest, TopLevelBareKeyRejected) {
    WeakNetConfig cfg;
    std::string err;
    bool ok = parse("something: 1\n", &cfg, &err);
    EXPECT_FALSE(ok);
}

TEST(WeakNetConfigTest, InvalidBoolRejected) {
    WeakNetConfig cfg;
    std::string err;
    bool ok = parse("monitors:\n  rtt:\n    enabled: maybe\n", &cfg, &err);
    EXPECT_FALSE(ok);
}

TEST(WeakNetConfigTest, InvalidDurationRejected) {
    WeakNetConfig cfg;
    std::string err;
    bool ok = parse("monitors:\n  rtt:\n    interval: fast\n", &cfg, &err);
    EXPECT_FALSE(ok);
}

// 错误必须带行号
TEST(WeakNetConfigTest, ErrorCarriesLineNumber) {
    WeakNetConfig cfg;
    std::string err;
    bool ok = parse("monitors:\n  rtt:\n    target: 1.1.1.1\n    magic: 42\n", &cfg, &err);
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("line 4"), std::string::npos);
}

// 辅助函数
TEST(WeakNetConfigTest, HelperFunctions) {
    std::string mon, field;
    EXPECT_TRUE(splitMonitorKey("rtt.interval", &mon, &field));
    EXPECT_EQ(mon, "rtt");
    EXPECT_EQ(field, "interval");

    EXPECT_FALSE(splitMonitorKey("no-dot", &mon, &field));
    EXPECT_FALSE(splitMonitorKey(".leading", &mon, &field));
    EXPECT_FALSE(splitMonitorKey("trailing.", &mon, &field));

    EXPECT_TRUE(isEnabledKey("rtt.enabled"));
    EXPECT_TRUE(isEnabledKey("dns.enabled"));
    EXPECT_FALSE(isEnabledKey("rtt.interval"));
    EXPECT_FALSE(isEnabledKey("enabled"));
}

// 空文件 → 全默认
TEST(WeakNetConfigTest, EmptyFileKeepsDefaults) {
    WeakNetConfig cfg;
    bool ok = parse("# 只有注释\n\n", &cfg);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 10000u);
}

// ---- setMonitorParam ----

TEST(WeakNetConfigTest, SetMonitorParamValidRange) {
    WeakNetConfig cfg;
    std::string err;
    EXPECT_TRUE(setMonitorParam(&cfg, "rtt.interval", "5s", &err));
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 5000u);
    EXPECT_TRUE(setMonitorParam(&cfg, "rtt.target", "1.1.1.1", &err));
    EXPECT_EQ(cfg.rtt.target.get(), "1.1.1.1");
    EXPECT_TRUE(setMonitorParam(&cfg, "rtt.enabled", "false", &err));
    EXPECT_FALSE(cfg.rtt.enabled.load());
    EXPECT_TRUE(setMonitorParam(&cfg, "rtt.window", "100", &err));
    EXPECT_EQ(cfg.rtt.window_size.load(), 100u);
    EXPECT_TRUE(setMonitorParam(&cfg, "dns.bpf_obj", "/lib/dns.bpf.o", &err));
    EXPECT_EQ(cfg.dns.bpf_obj.get(), "/lib/dns.bpf.o");
}

TEST(WeakNetConfigTest, SetMonitorParamRejectsBad) {
    WeakNetConfig cfg;
    std::string err;

    // 非白名单字段
    EXPECT_FALSE(setMonitorParam(&cfg, "rtt.magic", "1", &err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 10000u);  // 旧值保留

    // 无效 IPv4
    EXPECT_FALSE(setMonitorParam(&cfg, "rtt.target", "999.1.1.1", &err));

    // 区间过小
    EXPECT_FALSE(setMonitorParam(&cfg, "rtt.interval", "50ms", &err));
    EXPECT_FALSE(setMonitorParam(&cfg, "rtt.window", "1", &err));

    // 非 enabled 的 bool 字段不接受 random
    EXPECT_FALSE(setMonitorParam(&cfg, "rtt.enabled", "maybe", &err));
}

// ---- serializeMonitorJson ----

TEST(WeakNetConfigTest, SerializeMonitorJsonSingle) {
    WeakNetConfig cfg;
    std::string err;
    std::string json = serializeMonitorJson(cfg, "rtt", &err);
    EXPECT_TRUE(err.empty());
    // 必须包含 enabled/target/interval_ms/timeout_ms
    EXPECT_NE(json.find("\"enabled\":true"), std::string::npos);
    EXPECT_NE(json.find("\"target\":\"223.5.5.5\""), std::string::npos);
    EXPECT_NE(json.find("\"interval_ms\":10000"), std::string::npos);
    EXPECT_NE(json.find("\"timeout_ms\":800"), std::string::npos);
}

TEST(WeakNetConfigTest, SerializeMonitorJsonUnknown) {
    WeakNetConfig cfg;
    std::string err;
    std::string json = serializeMonitorJson(cfg, "frobnicate", &err);
    EXPECT_TRUE(json.empty());
    EXPECT_FALSE(err.empty());
}

// ============================================================================
// 引号剥离回归（真机缺陷，2026-09-13）
//
// 缺陷：解析器不剥离值两端引号，于是
//   portal_path: "/success.txt"
// 的实际值是 `"/success.txt"`（含字面引号），使 HTTP 请求路径畸形。
// 真机表现：detectportal.firefox.com 返回 404、captive.apple.com 返回 400，
// Portal oracle 永远判"内容不匹配"，能力无法建立。
//
// 为什么长期未暴露：现有用例的配置值都没加引号，而书写者按 YAML 习惯
// 加引号是极自然的行为，且失败是静默的（解析成功、语义错误）。
// ============================================================================

TEST(WeakNetConfigTest, StripsDoubleQuotedStringValue) {
    ConfigFile f(
        "monitors:\n"
        "  active_probe:\n"
        "    enabled: true\n"
        "    portal_path: \"/success.txt\"\n");
    WeakNetConfig cfg;
    std::string err;
    ASSERT_TRUE(loadWeakNetConfig(f.path(), &cfg, &err)) << err;

    EXPECT_EQ(cfg.active_probe.portal_path.get(), "/success.txt")
        << "双引号必须被剥离，否则请求路径会带字面引号";
}

TEST(WeakNetConfigTest, StripsSingleQuotedStringValue) {
    ConfigFile f(
        "monitors:\n"
        "  active_probe:\n"
        "    enabled: true\n"
        "    portal_path: '/success.txt'\n");
    WeakNetConfig cfg;
    std::string err;
    ASSERT_TRUE(loadWeakNetConfig(f.path(), &cfg, &err)) << err;

    EXPECT_EQ(cfg.active_probe.portal_path.get(), "/success.txt");
}

// 完整复刻真机配置中的 oracle 段，锁定端到端解析结果
TEST(WeakNetConfigTest, ParsesQuotedPortalOracleConfig) {
    ConfigFile f(
        "monitors:\n"
        "  active_probe:\n"
        "    enabled: true\n"
        "    interval_ms: 30000\n"
        "    timeout_ms: 3000\n"
        "    targets: \"cf|one.one.one.one|443|cloudflare,iana|example.com|443|iana\"\n"
        "    https_enabled: true\n"
        "    portal_check_enabled: true\n"
        "    portal_targets: \"fx|detectportal.firefox.com|80|mozilla,ap|captive.apple.com|80|apple\"\n"
        "    portal_path: \"/success.txt\"\n"
        "    portal_expect_body: \"success\"\n");
    WeakNetConfig cfg;
    std::string err;
    ASSERT_TRUE(loadWeakNetConfig(f.path(), &cfg, &err)) << err;

    EXPECT_TRUE(cfg.active_probe.enabled.load());
    EXPECT_TRUE(cfg.active_probe.https_enabled.load());
    EXPECT_TRUE(cfg.active_probe.portal_check_enabled.load());
    EXPECT_EQ(cfg.active_probe.portal_path.get(), "/success.txt");
    EXPECT_EQ(cfg.active_probe.portal_expect_body.get(), "success");
    // 前缀引号必须先被剥离，再进入 "id|host|port|domain" 切分
    EXPECT_EQ(cfg.active_probe.portal_targets.get().substr(0, 2), "fx")
        << "引号残留在值首会让第一个 target 的 id 变成 \"fx";
    EXPECT_NE(cfg.active_probe.targets.get().find("one.one.one.one"),
              std::string::npos);
}

// 无引号的值不应被影响（既有行为不得回退）
TEST(WeakNetConfigTest, UnquotedValueUnchanged) {
    ConfigFile f(
        "monitors:\n"
        "  active_probe:\n"
        "    enabled: true\n"
        "    portal_path: /success.txt\n");
    WeakNetConfig cfg;
    std::string err;
    ASSERT_TRUE(loadWeakNetConfig(f.path(), &cfg, &err)) << err;
    EXPECT_EQ(cfg.active_probe.portal_path.get(), "/success.txt");
}

// 不成对的引号不应被剥离（避免误伤不完整输入）
TEST(WeakNetConfigTest, UnpairedQuoteIsPreserved) {
    ConfigFile f(
        "monitors:\n"
        "  active_probe:\n"
        "    enabled: true\n"
        "    portal_path: \"/success.txt\n");
    WeakNetConfig cfg;
    std::string err;
    ASSERT_TRUE(loadWeakNetConfig(f.path(), &cfg, &err)) << err;
    EXPECT_EQ(cfg.active_probe.portal_path.get(), "\"/success.txt")
        << "只有成对引号才剥离；单个引号应原样保留";
}

// ============================================================================
// ConfigTransaction —— 云端下发配置的三态状态机
// ============================================================================

namespace {

/// 给 ConfigTransaction 一个独立 state_path，避免测试间互相覆盖。
std::string makeTxnStatePath() {
    return "./test_txn_" + std::to_string(::getpid()) + "_" +
           std::to_string(reinterpret_cast<uintptr_t>(new int{0})) + ".state";
}

}  // namespace

TEST(ConfigTransactionTest, StartsInStable) {
    ConfigTransaction txn;
    EXPECT_EQ(txn.state(), ConfigState::STABLE);
    EXPECT_EQ(txn.lastAppliedGeneration(), 0u);
}

TEST(ConfigTransactionTest, StartTrialCapturesPriorAndEntersTrial) {
    WeakNetConfig cfg;
    cfg.rtt.interval_ms.store(10000);
    ConfigTransaction txn;

    std::string err;
    ASSERT_TRUE(txn.startTrial(cfg, "rtt.interval_ms", "5000", "act-1", 1, &err)) << err;
    EXPECT_EQ(txn.state(), ConfigState::TRIAL);
    const auto trial = txn.trial();
    EXPECT_EQ(trial.generation, 1u);
    EXPECT_EQ(trial.pending_action_id, "act-1");
    ASSERT_EQ(trial.prior_values.count("rtt.interval_ms"), 1u);
    EXPECT_EQ(trial.prior_values.at("rtt.interval_ms"), "10000");
}

TEST(ConfigTransactionTest, ExtendTrialPreservesOriginalPriorValues) {
    WeakNetConfig cfg;
    cfg.rtt.interval_ms.store(10000);
    cfg.rtt.timeout_ms.store(800);
    ConfigTransaction txn;

    std::string err;
    ASSERT_TRUE(txn.startTrial(cfg, "rtt.interval_ms", "5000", "act-1", 1, &err)) << err;
    // 在 TRIAL 中再叠一个 key：prior 必须是该 key 在叠入时刻的当前值。
    ASSERT_TRUE(txn.extendTrial(cfg, "rtt.timeout_ms", "1500", "act-2", 2, &err)) << err;
    const auto trial = txn.trial();
    EXPECT_EQ(trial.prior_values.at("rtt.interval_ms"), "10000");
    // timeout_ms 在 extendTrial 时刻还没被改过，prior 是 cfg 当前的 800
    EXPECT_EQ(trial.prior_values.at("rtt.timeout_ms"), "800");
}

TEST(ConfigTransactionTest, RejectsNonTrialableKey) {
    WeakNetConfig cfg;
    ConfigTransaction txn;
    std::string err;
    EXPECT_FALSE(txn.startTrial(cfg, "edge.token", "abc", "act-1", 1, &err));
    EXPECT_EQ(err, "key not trialable: edge.token");
}

TEST(ConfigTransactionTest, RejectsStaleGeneration) {
    WeakNetConfig cfg;
    ConfigTransaction txn;
    std::string err;
    ASSERT_TRUE(txn.startTrial(cfg, "rtt.interval_ms", "5000", "act-1", 5, &err)) << err;
    ASSERT_TRUE(txn.confirmStable(5, &err)) << err;
    // 重放 generation=3（< last_applied=5）必须被拒
    EXPECT_FALSE(txn.startTrial(cfg, "rtt.interval_ms", "3000", "act-2", 3, &err));
    EXPECT_EQ(err, "stale_generation");
}

TEST(ConfigTransactionTest, ConfirmStableClearsTrialAndAdvancesGeneration) {
    WeakNetConfig cfg;
    ConfigTransaction txn;
    std::string err;
    ASSERT_TRUE(txn.startTrial(cfg, "rtt.interval_ms", "5000", "act-1", 7, &err)) << err;
    ASSERT_TRUE(txn.confirmStable(7, &err)) << err;
    EXPECT_EQ(txn.state(), ConfigState::STABLE);
    EXPECT_EQ(txn.lastAppliedGeneration(), 7u);
    EXPECT_EQ(txn.trial().prior_values.empty(), true);
}

TEST(ConfigTransactionTest, ConfirmStableRejectsWrongGeneration) {
    WeakNetConfig cfg;
    ConfigTransaction txn;
    std::string err;
    ASSERT_TRUE(txn.startTrial(cfg, "rtt.interval_ms", "5000", "act-1", 3, &err)) << err;
    EXPECT_FALSE(txn.confirmStable(9, &err));
    EXPECT_EQ(err, "generation_mismatch");
}

TEST(ConfigTransactionTest, ForceRollbackRestoresPriorValues) {
    WeakNetConfig cfg;
    cfg.rtt.interval_ms.store(10000);
    cfg.rtt.timeout_ms.store(800);
    ConfigTransaction txn;

    std::string err;
    ASSERT_TRUE(txn.startTrial(cfg, "rtt.interval_ms", "5000", "act-1", 1, &err)) << err;
    ASSERT_TRUE(txn.extendTrial(cfg, "rtt.timeout_ms", "2000", "act-2", 2, &err)) << err;

    // 应用新值（模拟 exporter 的 apply）
    ASSERT_TRUE(setMonitorParam(&cfg, "rtt.interval_ms", "5000", &err)) << err;
    ASSERT_TRUE(setMonitorParam(&cfg, "rtt.timeout_ms", "2000", &err)) << err;
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 5000u);
    EXPECT_EQ(cfg.rtt.timeout_ms.load(), 2000u);

    ASSERT_TRUE(txn.forceRollback(&cfg, "watchdog_timeout")) << "rollback must succeed";
    EXPECT_EQ(txn.state(), ConfigState::STABLE);
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 10000u);
    EXPECT_EQ(cfg.rtt.timeout_ms.load(), 800u);
    EXPECT_EQ(txn.lastAppliedGeneration(), 2u);
}

TEST(ConfigTransactionTest, DeadlineExpiresAfterWindow) {
    WeakNetConfig cfg;
    ConfigTransaction txn;
    std::string err;
    ASSERT_TRUE(txn.startTrial(cfg, "rtt.interval_ms", "5000", "act-1", 1, &err)) << err;
    const auto armed = txn.trial().armed_at;
    EXPECT_FALSE(txn.deadlineExpired(armed + std::chrono::seconds(179)));
    EXPECT_TRUE(txn.deadlineExpired(armed + std::chrono::seconds(181)));
}

TEST(ConfigTransactionTest, CrashRecoveryFileDetectsPersistedPriorValues) {
    const std::string path = makeTxnStatePath();
    {
        // 模拟崩溃前已写入 prior_values
        std::ofstream out(path);
        out << "rtt.interval_ms\t10000\n";
    }
    ConfigTransaction txn(path);
    EXPECT_TRUE(txn.hasCrashRecoveryFile());
    std::filesystem::remove(path);
}

TEST(ConfigTransactionTest, PersistedFileClearedOnStable) {
    const std::string path = makeTxnStatePath();
    WeakNetConfig cfg;
    ConfigTransaction txn(path);
    std::string err;
    ASSERT_TRUE(txn.startTrial(cfg, "rtt.interval_ms", "5000", "act-1", 1, &err)) << err;
    ASSERT_TRUE(std::filesystem::exists(path));
    ASSERT_TRUE(txn.confirmStable(1, &err)) << err;
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(ConfigTransactionTest, IsTrialableKeyWhitelist) {
    EXPECT_TRUE(isTrialableKey("rtt.interval_ms"));
    EXPECT_TRUE(isTrialableKey("rtt.timeout_ms"));
    EXPECT_TRUE(isTrialableKey("edge.interval_ms"));
    EXPECT_TRUE(isTrialableKey("edge.timeout_ms"));
    EXPECT_FALSE(isTrialableKey("edge.url"));
    EXPECT_FALSE(isTrialableKey("edge.token"));
    EXPECT_FALSE(isTrialableKey("edge.device_id"));
    EXPECT_FALSE(isTrialableKey("edge.private_key_path"));
    EXPECT_FALSE(isTrialableKey("edge.enabled"));
    EXPECT_FALSE(isTrialableKey("dns.assessment_profile"));
    EXPECT_FALSE(isTrialableKey("rtt.bpf_obj"));  // bpf_obj 永不在事务内
}

TEST(ConfigTransactionTest, SnapshotMonitorParamRoundTrip) {
    WeakNetConfig cfg;
    cfg.rtt.interval_ms.store(2500);
    std::string v;
    ASSERT_TRUE(snapshotMonitorParam(cfg, "rtt.interval_ms", &v));
    EXPECT_EQ(v, "2500");
    // 未知键返回 false
    EXPECT_FALSE(snapshotMonitorParam(cfg, "edge.token", &v));
}

// ============================================================================
// 回归：配置代推进（config_generation）
//
// 缺陷背景：ServerContext 曾持有一份只初始化、无人推进的 config_generation
// 副本，导致 AssessmentSnapshotStore::isCurrent() 退化为「只比网络代」，
// 「配置变更 → 旧快照失效」整条生命周期成死代码：改了配置，HealthCheck /
// GetNetworkExperience / 历史持久化仍继续返回旧配置下的结论。
// 修法：代次移入 WeakNetConfig，由 setMonitorParam() 在写入成功后单点推进。
// ============================================================================
TEST(ConfigGenerationTest, SuccessfulWriteAdvancesGeneration) {
    WeakNetConfig cfg;
    const uint32_t before = cfg.config_generation.load();
    std::string err;
    ASSERT_TRUE(setMonitorParam(&cfg, "rtt.interval_ms", "5000", &err)) << err;
    EXPECT_EQ(cfg.config_generation.load(), before + 1);
}

TEST(ConfigGenerationTest, EachSuccessfulWriteAdvancesMonotonically) {
    WeakNetConfig cfg;
    const uint32_t start = cfg.config_generation.load();
    std::string err;
    ASSERT_TRUE(setMonitorParam(&cfg, "rtt.interval_ms", "5000", &err));
    ASSERT_TRUE(setMonitorParam(&cfg, "rssi.interval_ms", "3000", &err));
    ASSERT_TRUE(setMonitorParam(&cfg, "dns.capture_pages", "128", &err));
    EXPECT_EQ(cfg.config_generation.load(), start + 3);
}

TEST(ConfigGenerationTest, RejectedWriteDoesNotAdvanceGeneration) {
    WeakNetConfig cfg;
    const uint32_t before = cfg.config_generation.load();
    std::string err;
    // 未知字段：必须被拒绝，且不得推进代次
    EXPECT_FALSE(setMonitorParam(&cfg, "rtt.no_such_field", "1", &err));
    EXPECT_EQ(cfg.config_generation.load(), before);
    // 非法值：区间外，同样不得推进
    EXPECT_FALSE(setMonitorParam(&cfg, "rtt.interval_ms", "1", &err));
    EXPECT_EQ(cfg.config_generation.load(), before);
    // 未知监控器
    EXPECT_FALSE(setMonitorParam(&cfg, "nosuchmonitor.interval_ms", "1", &err));
    EXPECT_EQ(cfg.config_generation.load(), before);
}

// ============================================================================
// 回归：assessment_profile 的运行时写入必须真正生效
//
// 缺陷背景：ServerContext 曾在启动时把 cfg.dns.assessment_profile 拷贝进一个
// 独立成员，评估线程只读那份拷贝。运行时 SetMonitorParam("dns.assessment_profile")
// 写进 cfg 却永远不生效——D-Bus 回 ok、GetMonitorParam 显示新值，OverallPolicy
// 仍用旧 profile，造成「看起来成功、实际无效」。
// 修法：删除拷贝，评估线程每轮现读 cfg.dns.assessment_profile。
// 本测试锁定该键的写入语义（可写、值可读回、非法值被拒）。
// ============================================================================
TEST(AssessmentProfileConfigTest, RuntimeWriteIsAcceptedAndReadableBack) {
    WeakNetConfig cfg;
    std::string err;
    ASSERT_TRUE(setMonitorParam(&cfg, "dns.assessment_profile", "NETWORK_ONLY", &err)) << err;
    // 权威来源（评估线程每轮现读的位置）必须立即可见新值
    EXPECT_EQ(cfg.dns.assessment_profile.get(), "NETWORK_ONLY");
    ASSERT_TRUE(setMonitorParam(&cfg, "dns.assessment_profile", "INTERNET_ACCESS", &err)) << err;
    EXPECT_EQ(cfg.dns.assessment_profile.get(), "INTERNET_ACCESS");
}

TEST(AssessmentProfileConfigTest, InvalidProfileIsRejectedAndLeavesValueUnchanged) {
    WeakNetConfig cfg;
    std::string err;
    ASSERT_TRUE(setMonitorParam(&cfg, "dns.assessment_profile", "NETWORK_ONLY", &err));
    EXPECT_FALSE(setMonitorParam(&cfg, "dns.assessment_profile", "BOGUS_PROFILE", &err));
    EXPECT_EQ(cfg.dns.assessment_profile.get(), "NETWORK_ONLY");
}
