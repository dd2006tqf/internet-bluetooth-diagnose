#!/bin/bash
# generate_vmlinux.sh - 生成 vmlinux.h
# 优先使用 bpftool 从当前内核生成，否则从 board-assets 复制
# 如果都不可用，生成空文件（eBPF 编译将在后续步骤中失败，但 cmake configure 不会中断）

OUTPUT="$1"
BOARD_VMLINUX="${2:-board-assets/vmlinux.h}"

# 交叉编译或非本机编译时（TARGET_ARCH=aarch64 或存在指定 board vmlinux 且为 cross）：
# 如果明确指定了 TARGET_ARCH=aarch64，或者我们在做交叉构建，必须优先使用目标板的 vmlinux.h
if [ "$TARGET_ARCH" = "aarch64" ] || [ "$TARGET_ARCH" = "arm64" ]; then
    if [ -f "$BOARD_VMLINUX" ]; then
        cp "$BOARD_VMLINUX" "$OUTPUT"
        echo "Copied ARM64 vmlinux.h from board-assets (TARGET_ARCH=$TARGET_ARCH)"
        exit 0
    fi
fi

if [ -f /sys/kernel/btf/vmlinux ] && command -v bpftool >/dev/null 2>&1 && [ "$(uname -m)" = "aarch64" ]; then
    bpftool btf dump file /sys/kernel/btf/vmlinux format c > "$OUTPUT"
    echo "Generated vmlinux.h from running aarch64 kernel"
elif [ -f "$BOARD_VMLINUX" ]; then
    cp "$BOARD_VMLINUX" "$OUTPUT"
    echo "Copied vmlinux.h from board-assets"
else
    echo "Warning: vmlinux.h not found, eBPF programs will not compile on this host"
    echo "/* vmlinux.h placeholder - compile eBPF in ARM64 container */" > "$OUTPUT"
fi
