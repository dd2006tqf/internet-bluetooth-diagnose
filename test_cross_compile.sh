#!/bin/bash
# test_cross_compile.sh - 测试 ARM64 交叉编译

set -e

echo "=== ARM64 交叉编译测试 ==="

# 1. 检查交叉编译工具链
echo "[1/5] 检查交叉编译工具链..."
if ! command -v aarch64-linux-gnu-g++ &> /dev/null; then
    echo "❌ 未找到 aarch64-linux-gnu-g++"
    echo "安装: sudo dnf install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu"
    exit 1
fi
echo "✅ 工具链已安装: $(aarch64-linux-gnu-g++ --version | head -1)"

# 2. 创建测试程序
echo "[2/5] 创建测试程序..."
cat > /tmp/test_arm64.cpp << 'EOF'
#include <iostream>
#include <string>

int main() {
    std::cout << "Hello from ARM64!" << std::endl;
    std::cout << "Architecture: " << 
#ifdef __aarch64__
        "aarch64 (ARM64)"
#else
        "unknown"
#endif
    << std::endl;
    return 0;
}
EOF

# 3. 交叉编译
echo "[3/5] 交叉编译 ARM64 二进制..."
cd /home/tanqf/AI-powered-Network-Diagnostics
aarch64-linux-gnu-g++ -std=c++17 -O2 -o /tmp/test_arm64 /tmp/test_arm64.cpp

# 4. 检查二进制架构
echo "[4/5] 检查二进制架构..."
file /tmp/test_arm64
if file /tmp/test_arm64 | grep -q "ARM aarch64"; then
    echo "✅ 二进制架构正确: ARM64"
else
    echo "❌ 二进制架构不正确"
    exit 1
fi

# 5. 显示依赖库
echo "[5/5] 显示动态库依赖..."
echo "动态库依赖:"
aarch64-linux-gnu-readelf -d /tmp/test_arm64 | grep NEEDED || echo "  (静态链接)"

echo ""
echo "=== 交叉编译测试完成 ==="
echo ""
echo "下一步:"
echo "1. 将 /tmp/test_arm64 传输到开发板"
echo "   scp /tmp/test_arm64 root@<开发板IP>:/tmp/"
echo ""
echo "2. 在开发板上运行"
echo "   ssh root@<开发板IP>"
echo "   chmod +x /tmp/test_arm64"
echo "   /tmp/test_arm64"
echo ""
echo "3. 如果成功，开始交叉编译完整项目"
echo "   cd /home/tanqf/AI-powered-Network-Diagnostics/server"
echo "   make CROSS_COMPILE=aarch64-linux-gnu- clean"
echo "   make CROSS_COMPILE=aarch64-linux-gnu- server"
