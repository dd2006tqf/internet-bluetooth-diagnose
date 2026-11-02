/**
 * @file test_client_cli_contract_gtest.cpp
 * @brief 校验 test_client 的 CLI 子命令契约与板端冒烟脚本保持一致
 *
 * 背景（2026-11-02 发现）：客户端订阅族删除（8 个导出符号）后，
 * `tools/weaknet-test-full.sh`（板端冒烟脚本）仍在调用 `test-events`、
 * `test-quality`、`check`、`event-types` 四个已删除的子命令。
 * 由于 `main()` 当时丢弃了 `runSingleTest()` 的返回值、无论成败都返回 0，
 * 这四项在板端被 `run_client_test()` 判成 **PASS**——是假绿灯。
 *
 * 两处缺陷已分别修复（`main()` 传播退出码 + 冒烟脚本删掉失效命令），
 * 但两者靠人工记忆同步，容易再次漂移。本测试把这份契约固化下来：
 *
 *   1. 冒烟脚本里 `run_client_test "<name>" "<cmd>"` 调用的每一个 cmd，
 *      必须在 `test_client.cpp` 的 `runSingleTest()` 中存在对应的
 *      `command == "<cmd>"` 分支；
 *   2. `main()` 必须把 `runSingleTest()` 的返回值传播成非零退出码，
 *      否则未知/失败的子命令会被脚本误判为通过。
 *
 * 纯静态分析（读源码文本），不需要 D-Bus、不需要板端，可在 x86/CI 直接跑。
 */

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef WEAKNET_REPO_SOURCE_DIR
#  error "WEAKNET_REPO_SOURCE_DIR must be defined by CMake"
#endif

namespace {

/// 仓库根目录（由 CMake 注入）
const std::string kRepoDir = WEAKNET_REPO_SOURCE_DIR;

/// 读取整个文件，失败返回空串
std::string readFile(const std::string& rel_path) {
    std::ifstream in(kRepoDir + "/" + rel_path);
    if (!in) return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

/// 冒烟脚本里 run_client_test "<name>" "<cmd>" 调用的全部 cmd（去重排序）
std::set<std::string> collectSmokeCommands() {
    const std::string src = readFile("tools/weaknet-test-full.sh");
    std::set<std::string> cmds;
    // run_client_test "显示名" "子命令"
    static const std::regex re(
        R"re(run_client_test\s+"[^"]*"\s+"([A-Za-z0-9_-]+)")re");
    for (std::sregex_iterator it(src.begin(), src.end(), re), end;
         it != end; ++it) {
        cmds.insert((*it)[1].str());
    }
    return cmds;
}

/// runSingleTest() 中实际实现的子命令（command == "xxx" 分支）
std::set<std::string> collectImplementedCommands() {
    const std::string src = readFile("client/test_client.cpp");
    std::set<std::string> cmds;
    static const std::regex re(R"re(command\s*==\s*"([A-Za-z0-9_-]+)")re");
    for (std::sregex_iterator it(src.begin(), src.end(), re), end;
         it != end; ++it) {
        cmds.insert((*it)[1].str());
    }
    return cmds;
}

}  // namespace

/// 契约 1：冒烟脚本调用的每个子命令都必须有实现，否则板端会假 PASS 或假 FAIL
TEST(ClientCliContractTest, EverySmokeScriptCommandIsImplemented) {
    const auto smoke_cmds = collectSmokeCommands();
    const auto impl_cmds = collectImplementedCommands();

    ASSERT_FALSE(smoke_cmds.empty())
        << "未解析到 tools/weaknet-test-full.sh 中的 run_client_test 调用，"
           "请检查脚本格式是否变化";
    ASSERT_FALSE(impl_cmds.empty())
        << "未解析到 client/test_client.cpp 中的 command == \"...\" 分支，"
           "请检查源码结构是否变化";

    std::vector<std::string> missing;
    for (const auto& cmd : smoke_cmds) {
        if (impl_cmds.find(cmd) == impl_cmds.end()) {
            missing.push_back(cmd);
        }
    }

    std::ostringstream msg;
    for (const auto& cmd : missing) {
        msg << "\n  冒烟脚本调用了未实现的子命令: " << cmd;
    }
    EXPECT_TRUE(missing.empty())
        << "板端冒烟脚本与 test_client 的实现已经漂移：" << msg.str()
        << "\n  修法二选一：在 runSingleTest() 中补上实现，"
           "或从 tools/weaknet-test-full.sh 中删掉该调用。";
}

/// 契约 2：main() 必须把 runSingleTest() 的失败传播成非零退出码
///
/// 若写成 `runSingleTest(...); return 0;`，调用方（板和 CI 的脚本）无法区分
/// 「命令成功」与「命令不存在/查询失败」，会把失败判成 PASS。
TEST(ClientCliContractTest, MainPropagatesSingleTestFailure) {
    const std::string src = readFile("client/test_client.cpp");
    ASSERT_FALSE(src.empty()) << "读不到 client/test_client.cpp";

    // 定位 main() 函数体起始位置
    const size_t main_pos = src.find("int main(int argc");
    ASSERT_NE(main_pos, std::string::npos) << "未找到 main() 定义";
    const std::string main_body = src.substr(main_pos);

    // 必须存在对 runSingleTest 调用的结果判断（!runSingleTest 或 == false）
    const bool checks_result =
        main_body.find("!runSingleTest") != std::string::npos ||
        std::regex_search(main_body,
                          std::regex(R"re(runSingleTest\s*\([^)]*\)\s*==\s*false)re"));

    EXPECT_TRUE(checks_result)
        << "main() 未检查 runSingleTest() 的返回值。\n"
           "  未检查时，未知子命令（如已删除的 test-events）仍会返回 0，\n"
           "  板端冒烟脚本 run_client_test() 会把它判成 PASS（假绿灯）。\n"
           "  正确写法: `if (!runSingleTest(...)) { return 1; }`";
}

/// 契约 3：已被删除的订阅族子命令不得重新出现在冒烟脚本里
///
/// 服务端自 2026-10-19 起停发全部 D-Bus 信号，这些子命令对应的订阅 API
/// 已于 2026-11-01 从 libweaknet.so 删除。若它们回到冒烟脚本，不是脚本错
/// 就是有人未经规划复活了发射端。
TEST(ClientCliContractTest, RemovedSubscriptionCommandsStayRemoved) {
    const auto smoke_cmds = collectSmokeCommands();
    static const std::vector<std::string> kRemoved = {
        "test-events",
        "test-quality",
        "check",
        "event-types",
        "event-sub",
        "quality-sub",
        "subscribe",
        "bt-events",
        "test-bt-callback",
        "test-events-callback",
        "test-quality-callback",
    };

    std::ostringstream msg;
    for (const auto& cmd : kRemoved) {
        if (smoke_cmds.find(cmd) != smoke_cmds.end()) {
            msg << "\n  冒烟脚本仍在调用已删除的订阅族子命令: " << cmd;
        }
    }
    EXPECT_TRUE(msg.str().empty())
        << "订阅族已于 2026-11-01 删除，不应再被调用：" << msg.str()
        << "\n  若要恢复事件推送，须先有真实订阅者，走 OpenSpec change 重建"
           "服务端发射端，不得凭空恢复。";
}
