# WeakNet Web 可视化仪表盘

WeakNet 的现代化暗黑风 Web 态势感知大屏与微服务网关，基于 **FastAPI + ECharts + CTypes + AI 专家知识库** 构建。

---

## 🌟 核心特性

- **综合健康大屏**：0~100 动态健康仪表盘、EXCELLENT / GOOD / FAIR / POOR 评级；
- **4 大 KPI 实时监测**：RTT 往返延迟、Jitter 抖动、nl80211 真实 Wi-Fi 信号强度、内核级 TCP 丢包率；
- **16 大监控器插件态势**：全量插件运行生命周期状态矩阵，支持 Web 界面一键重启；
- **8 大 eBPF 内核探针体检**：DNS、Wi-Fi 丢包、HTTP TTFB、TCP 重传、TCP 连接、skb_drop 等微秒级读写性能看板；
- **时序历史多维走势**：直读 SQLite WAL 数据库，支持 1h / 5h / 24h 自由缩放多轴对比；
- **蓝牙与 2.4GHz 射频共存感知**：周边蓝牙拓扑列表、Wi-Fi 频段感知（5GHz/6GHz 自动正交隔离免干扰）；
- **AI 一键专家根因诊断**：无缝联动 `AI-assisted analysis/` 知识库，智能推断高延迟、弱信号与同频干扰根因并提供优化建议。

---

## 🚀 部署与运行

### 方式 1：一键部署到开发板（ARM64）

在项目根目录下执行部署脚本（自动同步代码并启动 systemd 守护服务）：

```bash
BOARD=radxa@radxa-cubie-a7a.local ./web-dashboard/tools/deploy_web.sh
```

### 方式 2：本地启动调试

```bash
cd web-dashboard/backend
pip3 install -r requirements.txt
python3 -m uvicorn app:app --host 0.0.0.0 --port 8080 --reload
```

---

## 🌐 浏览器访问指南（开发板无屏幕场景）

开发板上没有屏幕或图形浏览器，仪表盘面向你的电脑、平板或手机浏览器：

### 1. 同一局域网直连访问（推荐）
当电脑/手机与开发板连接在同一个 Wi-Fi 或路由器下时，直接在浏览器地址栏打开：
- **mDNS 域名（推荐，IP 变化不失效）**：[http://radxa-cubie-a7a.local:8080](http://radxa-cubie-a7a.local:8080)
- **直接使用开发板 IP**：`http://<开发板IP>:8080`（例如 `http://192.168.137.210:8080`）

### 2. SSH 端口转发隧道（异地 / 远程办公访问）
当设备与开发板不在同一网络时，在本地电脑终端执行一条 SSH 端口隧道映射命令：

```bash
ssh -L 8080:127.0.0.1:8080 radxa@<开发板公网IP或中继>
```

保持该终端连接，然后在本地电脑浏览器打开：
- **[http://localhost:8080](http://localhost:8080)**

---

## 🛠️ 服务管理命令（在开发板终端中）

```bash
# 查看 Web 服务运行状态
sudo systemctl status weaknet-web

# 重启 Web 服务
sudo systemctl restart weaknet-web

# 实时查看 Web 服务日志
sudo journalctl -u weaknet-web -f
```
