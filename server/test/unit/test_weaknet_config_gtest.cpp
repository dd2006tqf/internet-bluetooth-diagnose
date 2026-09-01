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
        "  dbus_name: com.example.WeakNet\n"
        "  data_dir: /var/lib/weaknet\n"
        "  log_level: debug\n"
        "monitors:\n"
        "  rtt:\n"
        "    enabled: false\n"
        "    target: 8.8.8.8\n"
        "    interval: 5s\n"
        "    timeout: 500ms\n"
        "  jitter:\n"
        "    interval: 2s\n"
        "    window: 50\n"
        "  dns:\n"
        "    bpf_obj: /usr/lib/weaknet/dns_monitor.bpf.o\n",
        &cfg);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.dbus_name.get(), "com.example.WeakNet");
    EXPECT_EQ(cfg.data_dir.get(), "/var/lib/weaknet");
    EXPECT_EQ(cfg.log_level.get(), "debug");
    EXPECT_FALSE(cfg.rtt.enabled.load());
    EXPECT_EQ(cfg.rtt.target.get(), "8.8.8.8");
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 5000u);
    EXPECT_EQ(cfg.rtt.timeout_ms.load(), 500u);
    EXPECT_EQ(cfg.jitter.interval_ms.load(), 2000u);
    EXPECT_EQ(cfg.jitter.window_size.load(), 50u);
    EXPECT_EQ(cfg.dns.bpf_obj.get(), "/usr/lib/weaknet/dns_monitor.bpf.o");
    // 未覆盖字段保持默认
    EXPECT_EQ(cfg.traffic.interval_ms.load(), 10000u);
    EXPECT_EQ(cfg.quality.enabled.load(), true);
}

// 时长后缀解析：裸整数=ms、s、m
TEST(WeakNetConfigTest, DurationSuffixParsing) {
    WeakNetConfig cfg;
    bool ok = parse(
        "monitors:\n"
        "  rtt:\n"
        "    interval: 700\n"
        "  jitter:\n"
        "    interval: 2m\n",
        &cfg);
    EXPECT_TRUE(ok);
    EXPECT_EQ(cfg.rtt.interval_ms.load(), 700u);
    EXPECT_EQ(cfg.jitter.interval_ms.load(), 120000u);
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
    EXPECT_TRUE(setMonitorParam(&cfg, "jitter.window", "100", &err));
    EXPECT_EQ(cfg.jitter.window_size.load(), 100u);
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
    EXPECT_FALSE(setMonitorParam(&cfg, "jitter.window", "1", &err));

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
