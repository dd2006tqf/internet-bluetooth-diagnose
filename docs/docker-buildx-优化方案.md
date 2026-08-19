# Docker Buildx ARM64 编译优化方案

## 当前瓶颈

QEMU 用户态模拟 ARM64 指令，C++ 编译器是 CPU 密集型任务，导致：
- 首次全量编译：~30 分钟
- ccache 增量编译：~2 秒（已优化）

## Buildx 方案分析

### 方案 1：QEMU Buildx（当前等效）

```bash
docker buildx build --platform linux/arm64 .
```

**问题**：本质还是 QEMU 模拟，和现有方案速度一样。

### 方案 2：远程 ARM64 Builder（推荐）

使用 Docker Buildx 的 remote builder 功能，连接一台 ARM64 机器作为编译节点：

```bash
# 在 ARM64 机器上启动 buildkit
docker buildx create --name arm64-builder \
    --driver remote \
    ssh://user@arm64-machine

# 使用远程 ARM64 编译
docker buildx build --builder arm64-builder --platform linux/arm64 .
```

**优势**：
- ARM64 指令原生执行，无 QEMU 开销
- 编译速度提升 10-20 倍
- 可以利用开发板本身作为 builder（如果性能足够）

**要求**：
- 一台 ARM64 机器（可以是云服务器、另一个开发板、或 Apple Silicon Mac）
- Docker Buildx v0.8+
- SSH 免密访问

### 方案 3：交叉编译（已验证不可行）

CentOS host 的 aarch64-linux-gnu-g++ 与 Debian 11 sysroot ABI 不兼容，链接失败。

## 推荐实施路径

### 短期（当前）
- 使用 ccache 加速增量编译 ✅ 已完成

### 中期
1. 租用一台 ARM64 云服务器（如 AWS Graviton、阿里云 ARM）
2. 配置 Docker Buildx remote builder
3. 修改 `deploy_and_test.sh` 支持远程编译

### 长期
1. 购买专用 ARM64 编译服务器
2. 配置 CI/CD 流水线（GitHub Actions ARM64 runner）
3. 实现全自动编译-部署-测试

## 性能对比预估

| 方案 | 首次编译 | 增量编译 | 成本 |
|------|----------|----------|------|
| QEMU + ccache | ~30min | ~2s | 免费 |
| Remote ARM64 Builder | ~3min | ~10s | 云服务器费用 |
| 专用 ARM64 服务器 | ~2min | ~5s | 硬件费用 |
