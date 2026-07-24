// test_common.hpp
// WeakNet 测试公共头文件：统一测试宏、计数器与 glog 初始化
//
// 所有新增单元测试均 include 本头文件，保持风格一致（沿用项目现有手写宏风格）。
// 被测模块内部使用 LOG_INFO 宏（依赖 glog），故测试启动时需初始化 glog。

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>

// glog 初始化（被测模块依赖）
#include <glog/logging.h>

// glog 已定义 CHECK/CHECK_EQ 等宏（失败时 abort），但测试需要计数器风格。
// 先 undef glog 的宏，再定义项目自己的计数器版本（沿用 test_net_info.cpp 风格）。
#undef CHECK
#undef CHECK_EQ
#undef CHECK_NE
#undef CHECK_GT
#undef CHECK_LT
#undef CHECK_GE
#undef CHECK_LE
#undef CHECK_NEAR
#undef CHECK_STREQ
#undef CHECK_STRNE

// ============================================================================
// 测试计数器
// ============================================================================
static int g_pass = 0;
static int g_fail = 0;

// ============================================================================
// 断言宏（沿用 test_net_info.cpp 风格，新增 CHECK_EQ / CHECK_NEAR）
// ============================================================================
#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::cerr << "  FAIL: " << #cond \
        << " (line " << __LINE__ << ")" << std::endl; } \
} while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))
#define CHECK_GT(a, b) CHECK((a) > (b))
#define CHECK_LT(a, b) CHECK((a) < (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))

// 浮点近似比较
#define CHECK_NEAR(a, b, eps) CHECK(std::abs((double)(a) - (double)(b)) < (eps))

// 字符串包含
#define CHECK_CONTAINS(str, sub) CHECK((str).find(sub) != std::string::npos)

// ============================================================================
// 测试用例分组输出
// ============================================================================
#define TEST_CASE(name) \
    std::cout << "\n[TEST] " << name << std::endl

// ============================================================================
// glog 初始化（最小化：输出到 stderr，避免日志目录依赖）
// ============================================================================
inline void initTestLogging(const char* progName) {
    // 防止多次初始化
    static bool inited = false;
    if (inited) return;
    inited = true;
    google::InitGoogleLogging(progName);
    FLAGS_logtostderr = true;      // 日志输出到 stderr，不写文件
    FLAGS_minloglevel = 2;         // 仅 ERROR 及以上输出到 stderr（屏蔽 INFO 噪声）
}

// ============================================================================
// 测试结果汇总输出（main 末尾调用）
// ============================================================================
inline int printTestResult() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试结果: PASS=" << g_pass << " FAIL=" << g_fail << std::endl;
    if (g_fail == 0) {
        std::cout << "✅ 全部通过" << std::endl;
    } else {
        std::cout << "❌ 存在失败用例" << std::endl;
    }
    std::cout << "========================================" << std::endl;
    return g_fail > 0 ? 1 : 0;
}
