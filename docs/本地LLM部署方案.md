# Cubie A7A 本地 LLM 部署方案

## 一、硬件能力分析

### 1.1 关键资源

| 资源 | 规格 | LLM 推理适用性 |
|------|------|---------------|
| CPU 大核 | 2× Cortex-A76 @ 2.0GHz | 推理主计算单元，单核 IPC 接近 Intel Skylake 的 70% |
| CPU 小核 | 6× Cortex-A55 @ 1.8GHz | 辅助计算，token 生成时参与 batch 处理 |
| RAM | 8GB LPDDR5 (带宽 ~25GB/s) | 足够加载 3-4B 参数的 INT4 量化模型 |
| GPU | Imagination BXM-4-64 MC1 (Vulkan 1.3 / OpenCL 3.0) | 单核 GPU，Vulkan 后端可卸载部分矩阵运算 |
| NPU | Vivante VIP9000 (3 TOPS@INT8, FP16/BF16) | Transformer 算子支持有限，不推荐用于 LLM |
| 存储 | UFS 3.1 128GB (读 ~200MB/s) | 3GB 模型文件 ~15 秒加载完毕 |

### 1.2 内存预算（8GB 总量）

```
组件                        内存占用      说明
──────────────────────────────────────────────────
Linux 系统 + D-Bus          ~400MB        Debian 11 无桌面
weaknet-dbus-server         ~150MB        C++ 守护进程 + eBPF
llama.cpp 运行时             ~80MB         推理框架本身
Qwen2.5-3B Q4_K_M 权重      ~2.0GB        模型参数加载
KV Cache (2048 ctx)         ~1.5GB        推理上下文缓存
Python RAG 服务             ~200MB        FAISS + LangChain
──────────────────────────────────────────────────
合计                        ~4.33GB
可用余量                    ~3.67GB       ✅ 充裕
```

### 1.3 A76 大核纯 CPU 推理的性能预估

基于 llama.cpp ARM NEON 优化，以 Qwen2.5-3B Q4_K_M 为基准：

| 阶段 | 计算量 | 预估耗时 | 说明 |
|------|--------|---------|------|
| Prompt 处理 (500 tokens) | O(n²) attention | ~8-15 秒 | 首 token 延迟 (TTFT) |
| Token 生成 (每 token) | O(n) 自回归 | 55-100ms/tok | 即 10-18 tok/s |
| 典型诊断回答 (300 tokens) | — | ~17-30 秒 | 生成阶段 |
| 完整一次 RAG 分析 | — | **25-45 秒** | prompt 处理 + 生成 |

**结论**：一次网络诊断分析的端到端延迟约 30-45 秒，对异步诊断场景完全可用。

---

## 二、总架构设计

### 2.1 系统拓扑

```
┌──────────────────────────────────────────────────────────────────┐
│  Cubie A7A (Debian 11, ARM64, 8GB LPDDR5)                       │
│                                                                  │
│  ┌─────────────────────┐   ┌──────────────────────────────┐     │
│  │ weaknet-dbus-server │   │ llama.cpp server              │     │
│  │ (C++, D-Bus)        │   │ -m qwen2.5-3b-Q4_K_M.gguf    │     │
│  │                     │   │ --host 127.0.0.1 --port 8080  │     │
│  │ 状态: 始终运行       │   │ -c 2048 -t 4 -np 2           │     │
│  │ 内存: ~150MB         │   │ 状态: 始终运行                │     │
│  └─────────┬───────────┘   │ 内存: ~3.5GB                  │     │
│            │               └──────────────┬─────────────────┘     │
│            │                              │                       │
│            │    D-Bus Session Bus         │ HTTP (OpenAI API)     │
│            │                              │                       │
│  ┌─────────┴──────────────────────────────┴─────────────────┐    │
│  │  Python RAG 分析服务 (weaknet-rag)                        │    │
│  │                                                           │    │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────────────────┐    │    │
│  │  │ 日志解析  │  │ FAISS    │  │ LLM Router           │    │    │
│  │  │ (regex)  │  │ 向量检索  │  │                      │    │    │
│  │  └────┬─────┘  └────┬─────┘  │ 简单查询 → 本地 LLM   │    │    │
│  │       │              │        │ 复杂诊断 → 阿里百炼   │    │    │
│  │       └──────┬───────┘        │ 离线模式 → 本地 LLM   │    │    │
│  │              │                └──────────────────────┘    │    │
│  └──────────────┼────────────────────────────────────────────┘    │
│                 │                                                 │
└─────────────────┼─────────────────────────────────────────────────┘
                  │
        ┌─────────┴─────────┐
        │ 本地 LLM (优先)    │      │ 阿里百炼 API (兜底)  │
        │ 延迟: 25-45s       │      │ 延迟: 2-5s           │
        │ 成本: 0            │      │ 成本: 按 token 计费  │
        │ 隐私: 数据不出设备  │      │ 能力: Qwen-Plus 级   │
        └───────────────────┘      └──────────────────────┘
```

### 2.2 为什么选 llama.cpp server 而非 Python 直接加载

| 方案 | 内存效率 | 首次加载 | 并发 | 与 Python 集成 |
|------|---------|---------|------|---------------|
| llama.cpp server (HTTP) | ⭐⭐⭐⭐⭐ 独立进程，重启 RAG 服务不影响 LLM | 常驻内存，一次加载 | 支持并发请求 | 通过 OpenAI Python SDK 调用 |
| llama-cpp-python | ⭐⭐⭐ Python 进程内加载，内存碎片化 | 每次重启服务都需重新加载 | 单线程 GIL | 直接调用 |
| Ollama | ⭐⭐ 额外抽象层 | 慢，需 pull 镜像 | 好 | OpenAI SDK |

**选 llama.cpp server** 的理由：
1. 模型权重在独立进程，只加载一次；RAG 服务可随意重启而不需要重新加载 2GB 模型
2. 暴露 OpenAI 兼容 API（`/v1/chat/completions`），现有代码改一行 `base_url` 即可切换
3. 原生 C++，零 Python 开销，内存效率最高

### 2.3 为什么选 Qwen2.5-3B

| 候选模型 | 大小 | 中文能力 | 网络技术理解 | 资源占用 | 综合 |
|---------|------|---------|------------|---------|------|
| Qwen2.5-1.5B | 1.0GB | ⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐⭐⭐ | 太小，分析质量差 |
| **Qwen2.5-3B** | **2.0GB** | **⭐⭐⭐⭐** | **⭐⭐⭐⭐** | **⭐⭐⭐⭐⭐** | **最佳** |
| Qwen2.5-7B | 4.5GB | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | 内存紧张，速度慢 |
| Gemma-3-4B | 2.5GB | ⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐ | 中文弱 |
| DeepSeek-R1-1.5B | 1.0GB | ⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ | 推理能力强但太小 |

Qwen2.5-3B 与阿里百炼云端的 Qwen 系列同一生态，prompt 迁移成本最低。

---

## 三、编译部署流程

### 3.1 总体流程

```
开发机 (x86_64)                        Cubie A7A (ARM64)
     │                                       │
     │  步骤1: Docker 容器内交叉编译          │
     │  weaknet-builder:bullseye-arm64       │
     │  ├─ git clone llama.cpp               │
     │  ├─ cmake + make -j2                  │
     │  ├─ 产出 llama-server (ARM64)         │
     │  └─ 产出 libllama.so (ARM64)          │
     │                                       │
     │  步骤2: 下载 GGUF 模型                 │
     │  wget qwen2.5-3b-Q4_K_M.gguf          │
     │                                       │
     │  步骤3: rsync 到开发板                  │
     │  ──────────────────────────────────→  │
     │                                       │  步骤4: 启动测试
     │                                       │  llama-server 启动
     │                                       │  模型加载验证
     │                                       │  推理速度测试
```

### 3.2 步骤 1：修改 Dockerfile 增加 llama.cpp 编译依赖

在现有 `.docker/arm64-builder/Dockerfile` 基础上，增加 cmake 和 BLAS 库：

```dockerfile
# 在原有 apt-get install 行中追加：
    libopenblas-dev \
    cmake \
    ninja-build \
```

或者单独创建一个新的 Dockerfile 用于 llama.cpp 编译（不修改现有镜像，保持隔离）：

```dockerfile
# .docker/arm64-builder/Dockerfile.llama
FROM weaknet-builder:bullseye-arm64

RUN apt-get update && apt-get install -y --no-install-recommends \
    libopenblas-dev \
    cmake \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*
```

然后：

```bash
docker build --platform linux/arm64 \
  -f .docker/arm64-builder/Dockerfile.llama \
  -t weaknet-builder:llama-arm64 \
  .
```

### 3.3 步骤 2：交叉编译 llama.cpp

创建编译脚本 `scripts/build_llama_for_board.sh`：

```bash
#!/usr/bin/env bash
set -euo pipefail

CONTAINER="${CONTAINER:-weaknet-arm64-llama-dev}"
IMAGE="weaknet-builder:llama-arm64"
LLAMA_VERSION="b4902"  # 锁定已知稳定版本

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLAMA_SRC="$ROOT/llama.cpp"

# 1. 克隆 llama.cpp（如果已存在则 git pull）
if [ ! -d "$LLAMA_SRC" ]; then
    git clone --depth 1 https://github.com/ggerganov/llama.cpp.git "$LLAMA_SRC"
fi

# 2. 启动常驻容器（如果没有）
if ! docker inspect "$CONTAINER" >/dev/null 2>&1; then
    docker run -d \
        --name "$CONTAINER" \
        --platform linux/arm64 \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        -v "$LLAMA_SRC":/src:Z \
        -w /src \
        "$IMAGE" \
        sleep infinity
elif [ "$(docker inspect -f '{{.State.Running}}' "$CONTAINER")" != "true" ]; then
    docker start "$CONTAINER" >/dev/null
fi

# 3. 编译
docker exec "$CONTAINER" bash -c '
set -euo pipefail
cd /src

cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_BLAS=ON \
    -DGGML_BLAS_VENDOR=OpenBLAS \
    -DGGML_NATIVE=OFF \
    -DGGML_OPENMP=OFF \
    -DLLAMA_CURL=OFF

cmake --build build --config Release -j2 --target llama-server llama-cli
'

echo "===== 编译完成 ====="
echo "产物位置: $LLAMA_SRC/build/bin/"
ls -lh "$LLAMA_SRC/build/bin/llama-server" "$LLAMA_SRC/build/bin/llama-cli"
file "$LLAMA_SRC/build/bin/llama-server"
```

编译选项说明：

| 选项 | 选择 | 原因 |
|------|------|------|
| `GGML_BLAS=ON` | 启用 | 用 OpenBLAS 加速矩阵乘法，A76 上的 NEON SGEMM 比朴素实现快 3-5× |
| `GGML_BLAS_VENDOR=OpenBLAS` | OpenBLAS | ARM NEON 优化最好，比 Apple Accelerate/Intel MKL 更适合 ARM SBC |
| `GGML_NATIVE=OFF` | 关闭 | 交叉编译不能开 native，否则会编出 Docker 宿主机的指令 |
| `GGML_OPENMP=OFF` | 关闭 | QEMU 模拟下 OpenMP 线程调度有问题，用 llama.cpp 内置线程池 |
| `LLAMA_CURL=OFF` | 关闭 | 开发板不需要从 HF 下载模型 |
| `-j2` | 2 线程 | QEMU 模拟下高并行度会崩溃 |

### 3.4 步骤 3：扩展 dev-deploy.sh 增加 LLM 产物

在你的部署脚本（当前在 `tools/dev-deploy.sh`）中追加 llama.cpp 的同步逻辑，使其成为统一部署流程的一部分。或者采用更简洁的方式——**单独写 LLM 部署脚本**，保持职责分离：

新建 `scripts/deploy_llm_to_board.sh`：

```bash
#!/usr/bin/env bash
set -euo pipefail

BOARD="${BOARD:-radxa@radxa-cubie-a7a.local}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLAMA_SRC="$ROOT/llama.cpp"

# ── 1. 同步 llama.cpp 运行时 ──
echo "===== 同步 llama.cpp 运行时库 ====="
ssh "$BOARD" 'mkdir -p /home/radxa/llama/bin /home/radxa/llama/lib /home/radxa/llama/models'

rsync -avz -e ssh \
    "$LLAMA_SRC/build/bin/llama-server" \
    "$LLAMA_SRC/build/bin/llama-cli" \
    "$BOARD:/home/radxa/llama/bin/"

rsync -avz -e ssh \
    "$LLAMA_SRC/build/bin/"*.so \
    "$BOARD:/home/radxa/llama/lib/" 2>/dev/null || true

# ── 2. 同步模型文件（如果本地已下载） ──
MODEL_DIR="$ROOT/models"
if [ -d "$MODEL_DIR" ] && ls "$MODEL_DIR"/*.gguf >/dev/null 2>&1; then
    echo "===== 同步模型文件 ====="
    rsync -avz --progress -e ssh \
        "$MODEL_DIR/"*.gguf \
        "$BOARD:/home/radxa/llama/models/"
else
    echo "===== 模型文件未在本地找到，将在开发板上直接下载 ====="
    ssh "$BOARD" 'cd /home/radxa/llama/models && wget -c "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf"'
fi

# ── 3. 创建 systemd service 文件 ──
echo "===== 安装 systemd 服务 ====="
ssh "$BOARD" "sudo tee /etc/systemd/system/llama-server.service" <<'SERVICE'
[Unit]
Description=llama.cpp LLM Inference Server
After=network.target

[Service]
Type=simple
User=radxa
WorkingDirectory=/home/radxa/llama
Environment=LD_LIBRARY_PATH=/home/radxa/llama/lib
ExecStart=/home/radxa/llama/bin/llama-server \
    -m /home/radxa/llama/models/qwen2.5-3b-instruct-q4_k_m.gguf \
    --host 127.0.0.1 --port 8080 \
    -c 2048 -t 4 -np 2 \
    --mlock
Restart=on-failure
RestartSec=10
MemoryMax=5G
CPUQuota=300%

[Install]
WantedBy=multi-user.target
SERVICE

ssh "$BOARD" 'sudo systemctl daemon-reload'
echo "===== LLM 部署完成 ====="
echo "启动: ssh $BOARD 'sudo systemctl start llama-server'"
echo "状态: ssh $BOARD 'sudo systemctl status llama-server'"
```

`llama-server` 参数详解：

| 参数 | 值 | 含义 |
|------|---|------|
| `-m` | `.gguf` 路径 | 量化模型文件 |
| `--host 127.0.0.1` | 仅本地 | 安全：不暴露到网络 |
| `--port 8080` | 8080 | HTTP 服务端口 |
| `-c 2048` | 2048 | 上下文窗口（token 数） |
| `-t 4` | 4 线程 | A76×2 + A55×2，留核给弱网服务 |
| `-np 2` | 2 线程 | prompt 处理并行度 |
| `--mlock` | 锁定内存 | 防止模型被 swap 到 UFS（利用 8GB 充裕的优势） |

线程分配逻辑：2 个 A76 大核承担推理主力 + 2 个 A55 辅助，留 4 个 A55 给弱网服务和系统。

### 3.5 步骤 4：开发板模型下载策略

由于 GGUF 模型文件通常 2-5GB，从 HuggingFace 直接下载可能很慢。两种策略：

**策略 A：开发机下载后 rsync（推荐）**
```bash
# 开发机：利用国内镜像下载，然后 rsync 到开发板
# 推荐使用 modelscope 镜像
wget https://www.modelscope.cn/models/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/master/qwen2.5-3b-instruct-q4_k_m.gguf \
  -O models/qwen2.5-3b-instruct-q4_k_m.gguf
```

**策略 B：开发板直接下载**
```bash
# 利用 HuggingFace 镜像或 ModelScope 国内源
ssh radxa@radxa-cubie-a7a.local \
  "wget -c 'https://hf-mirror.com/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf' \
   -O /home/radxa/llama/models/qwen2.5-3b-instruct-q4_k_m.gguf"
```

---

## 四、Python RAG 模块集成

### 4.1 现有代码分析

当前 `local_vector_rag_analyzer.py` 使用 OpenAI SDK 调用阿里百炼 API：

```python
from openai import OpenAI

client = OpenAI(
    api_key="sk-xxx",
    base_url="https://dashscope.aliyuncs.com/compatible-mode/v1",
)
response = client.chat.completions.create(
    model="qwen-plus",
    messages=[...]
)
```

### 4.2 改造方案：LLM Router

只需创建一个路由层，核心改动不到 30 行：

```python
# AI-assisted analysis/llm_router.py
from openai import OpenAI
import os

class LLMRouter:
    """LLM 路由器：本地优先 + 远程兜底"""

    def __init__(self):
        self.local_client = OpenAI(
            api_key="not-needed",  # llama.cpp server 不需要 key
            base_url="http://127.0.0.1:8080/v1",
        )
        self.remote_client = OpenAI(
            api_key=os.getenv("DASHSCOPE_API_KEY", "sk-xxx"),
            base_url="https://dashscope.aliyuncs.com/compatible-mode/v1",
        )

    def chat(self, messages, max_tokens=512, prefer_local=True):
        """
        Args:
            prefer_local: True 优先本地 LLM，False 直接用远程
        """
        results = []

        if prefer_local:
            try:
                resp = self.local_client.chat.completions.create(
                    model="local-model",  # llama.cpp server 忽略此参数
                    messages=messages,
                    max_tokens=max_tokens,
                    temperature=0.1,
                    timeout=90,  # 本地推理较慢，放宽超时
                )
                results.append(("local", resp.choices[0].message.content))
            except Exception as e:
                results.append(("local_error", str(e)))
                # 本地失败 → 自动 fallback 远程
                try:
                    resp = self.remote_client.chat.completions.create(
                        model="qwen-plus",
                        messages=messages,
                        max_tokens=max_tokens,
                        temperature=0.1,
                        timeout=30,
                    )
                    results.append(("remote", resp.choices[0].message.content))
                except Exception as e2:
                    results.append(("remote_error", str(e2)))
        else:
            # 直接调用远程（复杂诊断场景）
            try:
                resp = self.remote_client.chat.completions.create(
                    model="qwen-plus",
                    messages=messages,
                    max_tokens=max_tokens,
                    temperature=0.1,
                    timeout=30,
                )
                results.append(("remote", resp.choices[0].message.content))
            except Exception as e:
                # 远程失败 fallback 本地
                try:
                    resp = self.local_client.chat.completions.create(
                        model="local-model",
                        messages=messages,
                        max_tokens=max_tokens,
                        temperature=0.1,
                        timeout=90,
                    )
                    results.append(("local", resp.choices[0].message.content))
                except Exception as e2:
                    results.append(("local_error", str(e2)))

        return results
```

### 4.3 决策模型：何时用本地，何时用远程

```python
def decide_backend(query_complexity: str, network_available: bool) -> str:
    """
    根据查询复杂度和网络状况决定使用哪个 LLM 后端
    """
    if not network_available:
        return "local"  # 离线时只能用本地

    # 简单查询用本地 LLM（离线、低延迟、0 成本）
    simple_patterns = [
        "当前网络状态",
        "有哪些网卡",
        "WiFi信号",
        "蓝牙设备",
        "网络速度",
        "延迟多少",
    ]
    if any(p in query for p in simple_patterns):
        return "local"

    # 复杂诊断用远程大模型
    complex_patterns = [
        "根因分析",
        "为什么频繁断线",
        "帮我排查",
        "历史趋势对比",
        "安全威胁",
    ]
    if any(p in query for p in complex_patterns):
        return "remote"

    # 默认本地优先
    return "local"
```

### 4.4 Prompt 模板适配

由于 Qwen2.5-3B（本地）和 Qwen-Plus（远程）能力有差距，建议用不同的 system prompt：

```python
SYSTEM_PROMPTS = {
    "local": """你是 WeakNet 网络诊断助手（轻量版），运行在嵌入式设备上。
请给出简洁、结构化的诊断结果。
格式：1) 状态摘要 2) 关键指标 3) 建议操作
保持回答在 300 字以内。""",

    "remote": """你是 WeakNet 高级网络诊断专家。请综合分析以下网络指标：
- RTT 延迟、TCP 丢包率、WiFi 信号强度、流量模式
- 结合网络知识库进行根因分析
- 给出详细的排查步骤和优化建议
请提供完整的诊断报告。""",
}
```

---

## 五、GPU / NPU 的定位

### 5.1 GPU Vulkan 加速（实验性优化）

BXM-4-64 是单核 GPU，算力有限，但值得一试。在 llama.cpp 编译时开启 Vulkan：

```bash
cmake -B build -G Ninja \
    ... \
    -DGGML_VULKAN=ON

# 启动时指定 offload 层数
llama-server -m model.gguf -ngl 20  # 前 20 层卸载到 GPU
```

预期收益：prompt 处理阶段可能加速 20-40%，生成阶段提升有限（受限于 GPU 单核和内存带宽）。

**风险提示**：Imagination GPU 的 Vulkan 驱动在 llama.cpp 社区未经充分测试，可能遇到渲染异常或崩溃。建议先以纯 CPU 路线跑稳，Vulkan 作为后续优化尝试。

### 5.2 NPU：不适合 LLM，但适合其他 AI 任务

VIP9000 的 SDK（TIM-VX）面向 CNN/RNN，对 Transformer Attention 的支持不成熟。但在你的网络诊断场景中，NPU 可以做以下事情：

```
┌────────────────────────────────────────────┐
│  NPU 可承担的任务（不抢 CPU）               │
│                                            │
│  实时流量异常检测                           │
│  ├─ 模型: 1D-CNN / LSTM (~2MB)            │
│  ├─ 输入: eBPF 流量统计 (pkt/s, byte/s)    │
│  └─ 延迟: <5ms, 持续运行                   │
│                                            │
│  蓝牙设备类型识别                           │
│  ├─ 模型: MobileNetV2-tiny (~5MB)          │
│  ├─ 输入: 设备特征向量                      │
│  └─ 延迟: <10ms, 按需运行                   │
│                                            │
│  语音命令识别 (如果有麦克风)                 │
│  ├─ 模型: Whisper-tiny (~40MB)             │
│  ├─ 输入: 音频流                            │
│  └─ 延迟: 实时, ~50ms/秒音频                │
└────────────────────────────────────────────┘
```

这些任务可以在 NPU 上 24/7 运行，不占用 A76 大核的 LLM 推理资源。

---

## 六、完整部署步骤清单

### Phase 1：环境准备（1-2 小时）

| # | 步骤 | 命令/文件 | 在何处执行 |
|---|------|----------|-----------|
| 1 | 构建 llama 编译镜像 | `docker build -f .docker/arm64-builder/Dockerfile.llama -t weaknet-builder:llama-arm64 .` | 开发机 |
| 2 | 创建编译脚本 | `scripts/build_llama_for_board.sh` | 开发机 |
| 3 | 运行编译 | `./scripts/build_llama_for_board.sh` | 开发机 |
| 4 | 下载 Qwen2.5-3B GGUF 模型 | `wget ... -O models/qwen2.5-3b-instruct-q4_k_m.gguf` | 开发机 |
| 5 | 创建部署脚本 | `scripts/deploy_llm_to_board.sh` | 开发机 |
| 6 | 执行部署 | `./scripts/deploy_llm_to_board.sh` | 开发机 |

### Phase 2：验证测试（30 分钟）

| # | 测试项 | 命令 | 预期结果 |
|---|--------|------|---------|
| 1 | 架构检查 | `file /home/radxa/llama/bin/llama-server` | `ELF 64-bit LSB ... ARM aarch64` |
| 2 | 启动服务 | `sudo systemctl start llama-server` | 无报错 |
| 3 | 检查内存 | `sudo systemctl status llama-server` | Memory < 5G |
| 4 | 简单推理 | `curl -X POST http://127.0.0.1:8080/v1/chat/completions -H 'Content-Type: application/json' -d '{"messages":[{"role":"user","content":"你好，1+1=?"}]}'` | 返回正常 JSON 回答 |
| 5 | 测速 | `llama-cli -m model.gguf -p "你好" -n 50 --no-display-prompt 2>&1 \| grep "eval time"` | 10-18 tok/s |
| 6 | 内存峰值 | `smem -t -P llama-server` | ~3.5GB |
| 7 | 网络诊断测试 | `curl ... -d '{"messages":[{"role":"user","content":"eth0 延迟 150ms，丢包 2%，帮我分析"}]}'` | 返回有意义的诊断 |

### Phase 3：RAG 集成（2-3 小时）

| # | 步骤 | 文件 |
|---|------|------|
| 1 | 创建 LLM Router | `AI-assisted analysis/llm_router.py` |
| 2 | 修改主分析器，注入 Router | `AI-assisted analysis/local_vector_rag_analyzer.py` |
| 3 | 添加 systemd service 自启动 | 开发板 `/etc/systemd/system/weaknet-rag.service` |
| 4 | 端到端测试 | 模拟弱网日志，触发完整 RAG + 本地 LLM 分析链路 |

### Phase 4：Vulkan GPU 加速（可选，1-2 小时）

| # | 步骤 |
|---|------|
| 1 | 在镜像中安装 Vulkan SDK (`libvulkan-dev mesa-vulkan-drivers`) |
| 2 | 重新编译 llama.cpp，开启 `-DGGML_VULKAN=ON` |
| 3 | 在开发板上安装 Vulkan 驱动 |
| 4 | A/B 对比测试：`-ngl 0` vs `-ngl 10` vs `-ngl 20` 的 token 生成速度 |

---

## 七、故障排查

### 7.1 llama-server 启动失败

| 现象 | 可能原因 | 解决 |
|------|---------|------|
| `error loading model: failed to mmap` | 模型文件路径错误或损坏 | `ls -lh /home/radxa/llama/models/`，检查 md5 |
| `Failed to allocate XX MB` | 内存不足 | 检查 `free -h`，确认没有其他大进程 |
| `Illegal instruction` | 编译时开启了 `GGML_NATIVE` | 重新编译，确保 `-DGGML_NATIVE=OFF` |
| `Address already in use: 8080` | 端口被占用 | 换端口或 `kill` 占用进程 |

### 7.2 推理速度过慢（<5 tok/s）

```
排查清单：
1. 确认 A76 大核被使用：taskset -cp $(pgrep llama-server)
   应显示 0-7（8 核），或手动绑定 taskset -c 0,1,4,5
2. 检查是否在 swap：grep VmSwap /proc/$(pgrep llama-server)/status
3. 检查 CPU 频率：cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
4. 如果没有开启 BLAS，重新编译开启 OpenBLAS
5. 如果开启了 Vulkan 但驱动有问题，回退到纯 CPU
```

### 7.3 Python RAG 调用 llama-server 超时

```python
# 本地推理 30-60 秒是正常的，超时设置不要太短
client = OpenAI(
    base_url="http://127.0.0.1:8080/v1",
    timeout=120,  # 给够时间
    max_retries=0,  # 不要在客户端重试，让 Router 处理 fallback
)
```

---

## 八、资源清单

### 需要新增的文件

```
项目根目录/
├── .docker/
│   └── arm64-builder/
│       └── Dockerfile.llama          # 新增：llama.cpp 编译镜像
├── scripts/
│   ├── build_llama_for_board.sh      # 新增：交叉编译脚本
│   └── deploy_llm_to_board.sh        # 新增：部署脚本
├── llama.cpp/                        # git submodule 或独立 clone（gitignore）
├── models/                           # GGUF 模型文件目录（gitignore）
│   └── qwen2.5-3b-instruct-q4_k_m.gguf
├── AI-assisted analysis/
│   └── llm_router.py                 # 新增：LLM 路由层
└── docs/
    └── 本地LLM部署方案.md             # 本文档
```

### 开发板新增文件

```
/home/radxa/llama/
├── bin/
│   ├── llama-server                  # HTTP 推理服务
│   └── llama-cli                     # 命令行测试工具
├── lib/
│   └── libllama.so                   # 运行时库
├── models/
│   └── qwen2.5-3b-instruct-q4_k_m.gguf
└── logs/
    └── llama-server.log

/etc/systemd/system/
└── llama-server.service              # 自启动服务
```

---

## 九、总结

| 维度 | 结论 |
|------|------|
| **可行性** | ✅ 完全可行。8GB 内存够跑 3B 量化模型，且有余量 |
| **最优推理路径** | CPU + llama.cpp + OpenBLAS，稳定且性能可接受 |
| **推荐模型** | Qwen2.5-3B Q4_K_M（中文好、与阿里百炼同生态、10-18 tok/s） |
| **与现有项目的集成方式** | llama.cpp server 暴露 OpenAI API → LLM Router 封装 → RAG 模块无缝切换 |
| **本地 vs 远程策略** | 简单查询走本地（30s），复杂诊断走远程（5s），离线时全走本地 |
| **GPU 路线** | Vulkan 作为实验性优化，不是主路径 |
| **NPU 路线** | 不用于 LLM，留给流量异常检测/设备识别等 CNN 任务 |
| **端到端延迟** | 一次 RAG 诊断约 25-45 秒，适合异步场景 |
| **总工作量** | 4-8 小时（含编译、部署、集成、测试） |
