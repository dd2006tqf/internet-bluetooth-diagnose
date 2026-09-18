# WeakNet 边云协同网络智能诊断平台 — 运行与演示全景指南

本项目由“tanqf”开发。

本项目实现了从**硬件底层内核探针 (eBPF + C++)** 到**中心云工业大模型智能诊断平台 (FastAPI + Next.js + 大模型推理网关)** 的端到端完整闭环。本文档汇总之启动命令、边云网络连通方案与全流程演示操作指南。

---

## 一、 系统架构与组件拓扑

```
+-----------------------------------------------------------------------------------+
|                           【中心云 / 宿主机】                                      |
|                                                                                   |
|   1. 工业大模型与运维管理服务 (Docker Compose / industrial-ops-m1)                 |
|      - FastAPI 业务后端: http://localhost:8000                                    |
|      - PostgreSQL (时序快照/网络资产/控制动作)                                       |
|      - Keycloak / Vault / OPA (身份认证、密码与细粒度 RBAC 策略)                   |
|      - Next.js 可视化前端: http://localhost:3000                                  |
|        * /network          (设备资产池大盘)                                       |
|        * /network/[id]     (五维属性、SLE 矩阵、15分钟时间线、远程下发控制)          |
|        * /network/copilot  (因果护栏大模型排障助手、中转网关免重启热配置)           |
+-----------------------------------------------------------------------------------+
                                      ▲
                                      │ (HTTP POST 经 Windows portproxy 转发)
                                      ▼
+-----------------------------------------------------------------------------------+
|                        【边缘端 / Radxa Cubie A7A 开发板】                         |
|                                                                                   |
|   2. WeakNet 内核网络监测服务 (systemd: weaknet-server)                            |
|      - 8 类 eBPF 内核级探针 (TCP/DNS/丢包/抖动/流量/蓝牙等)                          |
|      - C++ 弱网多维评估引擎 (SLE 评估矩阵，每 15 秒发布不可变评估快照)               |
|      - 本地 SQLite 历史持久化 (history.db，受 CAP_DAC_OVERRIDE 保护)               |
|      - 遥测上报器 (EdgeTelemetryExporter: libcurl + Ed25519 签名)                  |
|      - 网络代次持久化 (NetworkEpochStore: 消除重启后上行遥测静默丢弃)                |
|      - 下行控制回执 (Pull-on-Upload 随路拉取 + 白名单热更新 + 签名回执)              |
+-----------------------------------------------------------------------------------+
```

---

## 二、 边云网络连通方案（关键前置条件）

### 拓扑背景
- **开发板** 通过 Wi-Fi 连接电脑热点，获取 IP：`192.168.137.x`（网关为 `192.168.137.1`）。
- **中心云/宿主机** 运行在宿主机内部有线网卡，IP：`192.168.3.100`。
- **问题**：Windows 热点默认阻止热点客户端向宿主机所在局域网（192.168.3.x）发起反向连接，且 mDNS 无法跨广播域组播。

### 永久稳定方案（Windows portproxy 一键转发）
在 Windows 宿主机打开 **PowerShell（以管理员身份运行）**，执行以下两条命令：

```powershell
# 1. 设置端口转发：将热点网关的 8000 端口映射到宿主机 Linux 的 8000 端口
netsh interface portproxy add v4tov4 listenaddress=0.0.0.0 listenport=8000 connectaddress=192.168.3.100 connectport=8000

# 2. Windows 防火墙放行入站 TCP 8000 端口
New-NetFirewallRule -DisplayName "WeakNet-Edge-API-8000" -Direction Inbound -LocalPort 8000 -Protocol TCP -Action Allow
```
> **生效效果**：开发板访问 `http://192.168.137.1:8000` 将无感直达中心云后端，无需挂起任何临时终端隧道，开机自动生效。

---

## 三、 一键启动与运行

### 1. 启动中心云平台（宿主机）

进入 `Large-Model-Application` 目录使用内置维护脚本：

```bash
cd Large-Model-Application

# 首次启动：生成随机凭证与本地运行时环境
./scripts/dev_lite.sh init

# 启动全套微服务（PostgreSQL, Keycloak, Vault, Redis, OPA, Temporal, API, Web）
M1_RUNTIME_ENV_FILE=.env.m1.local docker compose --env-file .env.m1.local -f compose.lite.yaml up -d

# 检查服务健康状态（返回 {"status":"ready",...} 即为全绿）
curl -s http://localhost:8000/health/ready
```

### 2. 编译并部署开发板边缘端

在项目根目录下，使用项目标准 CI 脚本完成 ARM64 交叉编译、打包、同步与重启：

```bash
# 一键完成：ARM64 容器内增量编译 + 打包产物 + rsync 到板端 + systemd 重启
./tools/ci.sh --skip-test
```
*注：板端服务启动后，若需手工查验板端状态：*
```bash
ssh radxa@radxa-cubie-a7a.local 'sudo systemctl status weaknet-server'
```

---

## 四、 平台演示全流程指南（建议演示步骤）

### 演示一：边云遥测不可变上报与资产监控大盘
1. 浏览器打开 Web 控制台：`http://localhost:3000/network`。
2. 观察设备卡片：`radxa-cubie-a7a` 显示为 **ONLINE**（或根据网络评级显示绿色/黄色），最后心跳在几秒内持续刷新。
3. 点击进入设备详情页 `http://localhost:3000/network/radxa-cubie-a7a`：
   - **当前健康**：仪表盘展示综合得分与根因诊断（如 `primary_issue`）。
   - **五维属性**：展示 aarch64 硬件架构、Linux 5.15 内核版本、活动网卡 `wlan0`、IP 与 MAC。
   - **多维 SLE 矩阵**：展示物理层（RTT、抖动、信号强度）与服务层（DNS、TCP、HTTP）健康度。
   - **评估时间线**：展示每 15 秒一条由边缘推送的不可变快照，包含 `seq` 序号、`epoch` 代次与分值。

### 演示二：因果护栏大模型排障助手 (Copilot)
1. 进入 `http://localhost:3000/network/copilot`。
2. 点击右上角 **【大模型热配置】**：
   - 支持动态修改中转站地址（如 `https://vectide.cn/v1`）与 Model Name（如 `deepseek-v4-pro-0813`）。
   - 点击 **【测试连通性】**，即刻展示真实往返时延及模型连通验证结果。
3. 在提问框中提问：
   > *“分析当前 radxa-cubie-a7a 的网络状态，为什么之前评分为 DEGRADED？瓶颈在哪？”*
4. 观察回答：
   - 包含蓝色徽标 **`大模型解释（已过因果护栏）`**。
   - 系统采用“确定性因果链先行”机制，严格依据快照事实生成解释，绝不产生虚构幻觉。

### 演示三：边云下行控制通道（参数热调优与回执闭环）
1. 在设备详情页 `http://localhost:3000/network/radxa-cubie-a7a` 点击右上角 **【远程调参 / 下发控制】** 按钮。
2. 在弹出窗口中选择：
   - **参数键名**：`RTT 采样周期 (rtt.interval)`
   - **参数值**：输入 `5s`
3. 点击 **【排队下发】**，页面弹出排队成功通知。
4. **终端实时查验（双屏见证）**：
   - 边缘端会在下一次遥测响应中随路拉取指令，经过 C++ 内核白名单校验后直接生效：
     ```bash
     ssh radxa@radxa-cubie-a7a.local 'sudo /home/radxa/weaknet/client/bin/weaknet-cli get rtt'
     ```
     查验输出中的 `interval_ms` 已就地热变更为 `5000`！
   - 查看板端系统日志：
     ```bash
     ssh radxa@radxa-cubie-a7a.local 'sudo journalctl -u weaknet-server -n 10 --no-pager | grep "已应用"'
     ```
     打印日志：`已应用服务端下发的配置: rtt.interval=5s`。
   - 边缘端完成 Ed25519 签名后异步调用 `/network/edge/action-results`，中心云数据库内该指令状态自动变为 **`APPLIED`**。
5. 回到页面点击 **【刷新】** 按钮，五维属性中的“RTT 采样间隔”已自动更新为 `5s`。

---

## 五、 常见问题与故障排查

### 1. 设备详情页提示“设备不存在或不可见”
- **原因**：权限隔离策略拦截。云端 API `_require_read_scope` 过去绑定了工业生产工单资产（`asset-m1-pump`），若当前登录用户未分配该资产的权限范围，会被 `device_scope_denied` 拦截。
- **状态**：**已彻底修复**。网络设备已被正确确认为网络基础设施资源，租户运维角色持有 `network_assurance.read` 权限即可自由查看。

### 2. 开发板重启后云端数据库没有新数据
- **原因**：开发板 `sequence_id` 进程计数器从 1 重启，若 `network_epoch` 也是 1，云端会将 `(epoch=1, seq=1..N)` 误判为历史重复项而静默丢弃。
- **状态**：**已彻底修复**。开发板引入了 `NetworkEpochStore`，重启时代次严格持久化递增（1 -> 2 -> 3...），彻底杜绝静默去重问题。

### 3. 开发板本地 history.db 打不开
- **原因**：`weaknet-server.service` 以 root 运行但 `data/` 目录属于 `radxa:radxa`，缺少 `CAP_DAC_OVERRIDE` 导致 root 被当成 other 用户拒绝写入。
- **状态**：**已彻底修复**。服务单元已固化添加 `CAP_DAC_OVERRIDE` 权能。
