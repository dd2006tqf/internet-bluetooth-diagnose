#!/bin/bash
# run_all_tests.sh - 运行所有 C++ 单元测试
# 用法: ./test/run_all_tests.sh
set -e

cd "$(dirname "$0")/.."  # 进入 server/ 目录

PASS=0
FAIL=0
FAILED_TESTS=""

# 运行单个测试套件
# 参数: $1=测试名 $2=测试源文件 $3=依赖的源文件列表
run_test() {
    local name=$1
    local src=$2
    local deps=$3
    local bin="test/${name#test_}"  # 去掉 test_ 前缀作为二进制名

    echo "========== 编译 ${name} =========="
    if ! g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude -o "$bin" "test/${src}" $deps 2>/tmp/build_err_${name}.log; then
        echo "[COMPILE FAIL] ${name}"
        cat /tmp/build_err_${name}.log
        FAIL=$((FAIL + 1))
        FAILED_TESTS="${FAILED_TESTS} ${name}(compile)"
        return
    fi

    echo "========== 运行 ${name} =========="
    if "$bin"; then
        PASS=$((PASS + 1))
        echo "[PASS] ${name}"
    else
        FAIL=$((FAIL + 1))
        FAILED_TESTS="${FAILED_TESTS} ${name}(run)"
        echo "[FAIL] ${name}"
    fi
}

echo "########################################"
echo "#  AI-powered-Network-Diagnostics 测试 #"
echo "########################################"
echo ""

# 注册所有测试（新增测试时在此添加一行）
run_test test_net_info test_net_info.cpp "src/net_info.cpp src/serializer.cpp"
# run_test test_network_quality_assessor test_network_quality_assessor.cpp "src/network_quality_assessor.cpp src/net_info.cpp"
# run_test test_serializer test_serializer.cpp "src/serializer.cpp"

# Python 测试（如果存在）
if [ -d "../AI-assisted analysis" ] && ls "../AI-assisted analysis"/test_*.py >/dev/null 2>&1; then
    echo ""
    echo "========== Python 测试 =========="
    cd "../AI-assisted analysis"
    if python3 -m pytest test_*.py -v --tb=short 2>/dev/null; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILED_TESTS="${FAILED_TESTS} python_tests"
    fi
fi

echo ""
echo "########################################"
echo "测试结果: 通过 ${PASS} 个套件, 失败 ${FAIL} 个套件"
if [ -n "$FAILED_TESTS" ]; then
    echo "失败套件:${FAILED_TESTS}"
    echo "########################################"
    exit 1
fi
echo "全部通过"
echo "########################################"
exit 0
