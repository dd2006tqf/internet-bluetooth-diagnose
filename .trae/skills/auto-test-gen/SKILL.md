---
name: "auto-test-gen"
description: "为项目同步生成单元测试与集成测试代码。Invoke when 进行新功能开发、功能扩展、代码优化或修复 bug 时，或用户要求编写/补充测试代码时。"
---

# 自动化测试生成技能

为 AI-powered-Network-Diagnostics 项目同步生成测试代码，确保新功能开发、功能扩展、代码优化时测试同步跟进，覆盖关键功能点与边界情况。

## 一、触发场景

当出现以下任一情况时，**必须**主动应用本技能：

1. **新功能开发**：新增监控线程、新增采集模块、新增 D-Bus 方法/信号等
2. **功能扩展**：为现有类添加新方法（如 NetInfo 新增序列化方法）、新增字段
3. **代码优化**：重构算法、修改核心逻辑（如质量评估算法调整）
4. **Bug 修复**：修复任何缺陷后，补充回归测试
5. **用户明确要求**：用户要求编写测试、补充测试、提高覆盖率

## 二、项目测试规范

### 2.1 C++ 测试规范（server 端）

**测试框架**：项目不依赖第三方框架，使用手写 `CHECK` 宏 + `main` 函数风格（参考 [server/test/test_net_info.cpp](file:///home/tanqf/AI-powered-Network-Diagnostics/server/test/test_net_info.cpp)）。

**目录结构**：
```
server/
├── src/           # 源码
├── include/       # 头文件
├── test/          # 测试代码
│   ├── test_net_info.cpp        # 已有样例
│   ├── test_<module>.cpp        # 新测试按模块命名
│   └── run_all_tests.sh         # 自动化运行脚本
└── Makefile
```

**测试文件模板**（严格遵守）：

```cpp
// test_<module>.cpp
// <模块名> 的单元测试
// 编译: g++ -std=c++17 -O2 -Wall -Iinclude -o test/test_<module> test/test_<module>.cpp src/<module>.cpp [依赖的其他src]

#include "<module>.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

using namespace weaknet_dbus;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::cerr << "FAIL: " << #cond << " (line " << __LINE__ << ")" << std::endl; } \
} while (0)

// 浮点比较辅助宏（容差比较）
#define CHECK_FLOAT_EQ(a, b, eps) CHECK(std::abs((a) - (b)) < (eps))

static void test<FeatureName>() {
    std::cout << "[test] <功能描述>..." << std::endl;

    // 正常用例
    // ...

    // 边界用例
    // ...

    // 异常/畸形输入用例
    // ...
}

int main() {
    std::cout << "========== <ModuleName> 测试 ==========" << std::endl;

    test<FeatureName>();
    // ... 其他测试函数

    std::cout << "==========================================" << std::endl;
    std::cout << "通过: " << g_pass << ", 失败: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
```

**命名规范**：
- 测试文件：`test_<模块名>.cpp`（如 `test_network_quality_assessor.cpp`）
- 测试函数：`test<功能名>`（如 `testQualityScoring`）
- 编译产物：`test/<模块名>`（如 `test/network_quality_assessor`）

**编译命令规范**：
- 编译标志：`-std=c++17 -O2 -Wall -Wextra`（与主项目一致）
- 头文件路径：`-Iinclude`
- 仅链接被测模块必要的源文件，不链接整个项目（避免依赖 dbus/bpf 等）
- 编译命令写在测试文件头部注释中

### 2.2 Python 测试规范（AI 模块）

**测试框架**：使用 `pytest`。

**目录结构**：
```
AI-assisted analysis/
├── simple_rag_analyzer.py
├── test_simple_rag_analyzer.py    # 测试与源码同目录
└── ...
```

**测试文件模板**：

```python
#!/usr/bin/env python3
"""<模块名> 的单元测试"""

import pytest
from <module> import <ClassName>, <function>


class Test<FeatureName>:
    """<功能描述>测试"""

    def test_normal_case(self):
        """正常用例"""
        # ...

    def test_boundary_case(self):
        """边界用例"""
        # ...

    def test_invalid_input(self):
        """异常输入用例"""
        with pytest.raises(<ExpectedException>):
            # ...

    @pytest.mark.parametrize("input,expected", [
        # 参数化测试
    ])
    def test_parametrized(self, input, expected):
        assert <function>(input) == expected
```

## 三、测试覆盖要求

### 3.1 必须覆盖的测试类型

每个被测模块**必须**包含以下三类用例：

| 用例类型 | 说明 | 占比建议 |
|---------|------|---------|
| **正常用例** | 典型输入、常见场景 | ~50% |
| **边界用例** | 最小/最大值、空输入、临界条件 | ~30% |
| **异常用例** | 畸形输入、错误数据、资源失败 | ~20% |

### 3.2 关键模块的测试要点

**NetInfo（数据结构）**：
- 序列化/反序列化往返一致性
- 数据验证（isValid、hasXxx）
- JSON 特殊字符转义
- 二进制畸形输入（截断、版本号错误）

**NetworkQualityAssessor（质量评估）**：
- 各指标评分算法（RTT/丢包/RSSI/流量）
- 加权计算正确性
- 等级判定边界（90/75/50 分临界）
- 缺失指标时的降级处理

**TrafficAnomalyDetector（异常检测）**：
- 突发流量检测（历史均值倍数）
- 高流量阈值
- 空历史数据时的处理

**Serializer（序列化）**：
- 字符串/整数序列化往返
- 文件读写
- ChangedPayload 序列化
- 畸形缓冲区处理

**JitterMonitor（抖动监控）**：
- 标准差计算正确性
- 滑动窗口边界（满窗口、空窗口）
- 等级判定（good/degraded/poor）

**RAG 分析器（Python）**：
- 日志解析（正则提取 RTT/丢包/RSSI）
- 知识库检索
- 降级模式（TF-IDF 回退）
- 畸形日志输入

### 3.3 不需要测试的模块

以下模块因强依赖系统环境/硬件，**不强制**单元测试，但可编写集成测试（需 mock 或条件跳过）：
- eBPF 程序（flow_rate.bpf.c）—— 需 root 和特定内核
- DBus 通信 —— 需 dbus-daemon 运行
- wpa_supplicant / BlueZ 交互 —— 需硬件
- netlink / raw socket —— 需 root

对这些模块，测试中应使用 `#ifdef` 或运行时检测进行条件跳过：

```cpp
static void testEBPFLoading() {
    std::cout << "[test] eBPF 加载（需 root）..." << std::endl;
    if (getuid() != 0) {
        std::cout << "  [SKIP] 需要 root 权限" << std::endl;
        return;
    }
    // 实际测试逻辑
}
```

## 四、自动化测试流程

### 4.1 C++ 测试自动化脚本

在 `server/test/` 下维护 `run_all_tests.sh`：

```bash
#!/bin/bash
# 运行所有 C++ 单元测试
set -e

cd "$(dirname "$0")/.."  # 进入 server/ 目录

PASS=0
FAIL=0
FAILED_TESTS=""

run_test() {
    local name=$1
    local src=$2
    local deps=$3
    local bin="test/${name}"

    echo "========== 编译 ${name} =========="
    if ! g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -o "$bin" "test/${src}" $deps; then
        echo "[COMPILE FAIL] ${name}"
        FAIL=$((FAIL + 1))
        FAILED_TESTS="${FAILED_TESTS} ${name}(compile)"
        return
    fi

    echo "========== 运行 ${name} =========="
    if "$bin"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED_TESTS="${FAILED_TESTS} ${name}(run)"
    fi
}

# 注册所有测试（新增测试时在此添加一行）
run_test test_net_info test_net_info.cpp "src/net_info.cpp src/serializer.cpp"
# run_test test_<module> test_<module>.cpp "src/<module>.cpp ..."

echo ""
echo "========================================"
echo "测试结果: 通过 ${PASS} 个套件, 失败 ${FAIL} 个套件"
if [ -n "$FAILED_TESTS" ]; then
    echo "失败套件:${FAILED_TESTS}"
    exit 1
fi
exit 0
```

### 4.2 Python 测试自动化

```bash
# 在 "AI-assisted analysis/" 目录下运行
cd "AI-assisted analysis"
python3 -m pytest test_*.py -v --tb=short
```

### 4.3 集成测试（可选）

对于需要完整服务运行的集成测试，编写在 `server/test/integration/` 下：

```bash
# 启动服务 → 调用 D-Bus 接口 → 验证响应 → 停止服务
server/test/integration/test_dbus_health_check.sh
```

集成测试通过 `dbus-send` 验证服务端接口：

```bash
#!/bin/bash
# 测试 HealthCheck 接口
dbus-send --session --print-reply --dest=com.example.WeakNet \
    /com/example/WeakNet com.example.WeakNet.HealthCheck
```

## 五、执行流程

当触发本技能时，按以下步骤执行：

### 步骤 1：分析改动

1. 确定被改动/新增的模块（源文件 + 头文件）
2. 识别该模块的公开接口（public 方法、导出函数）
3. 识别核心算法和业务逻辑

### 步骤 2：设计测试用例

1. 列出所有公开接口的测试用例清单
2. 为每个接口设计正常用例、边界用例、异常用例
3. 标注哪些用例需要系统环境（root/硬件），需条件跳过

### 步骤 3：编写测试代码

1. 按模板创建 `test_<module>.cpp` 或 `test_<module>.py`
2. 编译命令写入文件头部注释
3. 测试函数按功能分组，每组前打印 `[test] <功能描述>`
4. 使用 `CHECK` 宏（C++）或 `assert`（Python）进行断言

### 步骤 4：编译与运行

1. **C++**：按文件头部注释的命令编译并运行
2. **Python**：`python3 -m pytest test_<module>.py -v`
3. 确保所有测试通过（0 失败）
4. 如有失败，修复测试代码或反馈源码问题

### 步骤 5：更新自动化脚本

1. 在 `server/test/run_all_tests.sh` 中注册新测试
2. 验证脚本可一键运行所有测试

### 步骤 6：提交

1. 将测试代码与源码改动一起提交
2. Commit message 中说明测试覆盖情况（如"新增 35 个测试用例，覆盖正常/边界/异常场景"）

## 六、质量标准

测试代码必须满足以下标准：

1. **编译无警告**：`-Wall -Wextra -Wpedantic` 下零警告（C++）
2. **全部通过**：所有测试用例必须通过，不允许遗留失败
3. **覆盖三类用例**：正常、边界、异常缺一不可
4. **失败不变性**：反序列化/解析类操作失败时不能修改原对象
5. **独立性**：测试之间无依赖，可单独运行任意测试函数
6. **可读性**：测试函数名表达意图，注释说明测试目的
7. **无外部依赖**：单元测试不依赖网络/硬件/dbus（集成测试除外）
