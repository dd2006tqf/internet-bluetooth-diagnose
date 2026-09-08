"""
app.py
WeakNet Web 前端仪表盘后端网关：基于 FastAPI 与 Uvicorn 构建，
聚合 C++ D-Bus 状态、SQLite 历史时序与 AI 智能诊断，同时提供 WebSocket 实时推送与前端 SPA 托管。
"""

import asyncio
import json
import logging
import os
import sys
from typing import Any, Dict, List, Set

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from weaknet_bridge import WeakNetBridge
from db_service import DbService
from ai_service import AiDiagnosisService

# 日志基础配置
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] [%(name)s] %(message)s"
)
logger = logging.getLogger("weaknet.app")

# 日志缓存：在内存中保存最近 200 条操作与系统日志
import collections
from datetime import datetime

log_buffer = collections.deque(maxlen=200)

def record_log(level: str, module: str, message: str):
    entry = {
        "time": datetime.now().strftime("%H:%M:%S"),
        "level": level.upper(),
        "module": module,
        "message": message
    }
    log_buffer.append(entry)
    logger.info("[%s] [%s] %s", level, module, message)
    # 尝试实时通过 WebSocket 广播日志给前端
    try:
        loop = asyncio.get_event_loop()
        if loop.is_running():
            asyncio.create_task(ws_manager.broadcast({
                "type": "LOG_ENTRY",
                "entry": entry
            }))
    except Exception:
        pass

record_log("INFO", "SYSTEM", "WeakNet Web Gateway started")

app = FastAPI(
    title="WeakNet Network & Bluetooth Diagnostics",
    description="基于 eBPF 与 D-Bus 的实时网络监控系统 Web 前端可视化仪表盘网关",
    version="1.0.0"
)

# 允许跨域访问（方便本地开发调试）
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# 初始化底层数据源服务
bridge = WeakNetBridge.get_instance()
db = DbService()
ai = AiDiagnosisService()

# 静态资源目录
current_dir = os.path.dirname(os.path.abspath(__file__))
frontend_dir = os.path.abspath(os.path.join(current_dir, "..", "frontend"))


# ============================================================================
# WebSocket 客户端连接池与实时推送广播
# ============================================================================
class ConnectionManager:
    def __init__(self):
        self.active_connections: Set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        async with self._lock:
            self.active_connections.add(websocket)
        logger.info("WebSocket client connected. Total clients: %d", len(self.active_connections))

    async def disconnect(self, websocket: WebSocket):
        async with self._lock:
            self.active_connections.discard(websocket)
        logger.info("WebSocket client disconnected. Total clients: %d", len(self.active_connections))

    async def broadcast(self, message: dict):
        if not self.active_connections:
            return
        data_str = json.dumps(message)
        dead = []
        async with self._lock:
            for ws in list(self.active_connections):
                try:
                    await ws.send_text(data_str)
                except Exception:
                    dead.append(ws)
            for ws in dead:
                self.active_connections.discard(ws)


ws_manager = ConnectionManager()


@app.on_event("startup")
async def startup_event():
    # 启动后台异步轮询与推送协程
    asyncio.create_task(background_metrics_emitter())


async def background_metrics_emitter():
    """后台协程：每 3 秒非阻塞采集一次核心健康数据并广播推送给前端 WebSocket 连接"""
    loop = asyncio.get_event_loop()
    while True:
        try:
            if ws_manager.active_connections:
                health = await loop.run_in_executor(None, bridge.get_health)
                conflict = await loop.run_in_executor(None, bridge.get_coexistence_conflict)
                await ws_manager.broadcast({
                    "type": "METRICS_UPDATE",
                    "timestamp": loop.time(),
                    "health": health.get("data") if health.get("success") else None,
                    "conflict": conflict.get("data") if conflict.get("success") else None
                })
        except Exception as e:
            logger.debug("Broadcast error: %s", e)
        await asyncio.sleep(3.0)


# ============================================================================
# RESTful API 端点
# ============================================================================

@app.get("/api/health")
async def api_health():
    """获取当前网络质量健康快照（质量评分、RTT、信号强度、TCP丢包率、issues等）"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_health)


@app.get("/api/interfaces")
async def api_interfaces():
    """获取系统可用网络接口列表"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_interfaces)


@app.get("/api/monitors")
async def api_monitors():
    """获取 16 个监控插件生命周期运行状态矩阵"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.list_monitors)


@app.post("/api/monitors/{name}/restart")
async def api_restart_monitor(name: str):
    """动态重启指定监控器插件"""
    record_log("INFO", "MONITOR", f"Triggered restart for monitor plugin '{name}'")
    loop = asyncio.get_event_loop()
    res = await loop.run_in_executor(None, bridge.restart_monitor, name)
    if not res.get("success"):
        err = res.get("error", "Failed to restart monitor")
        record_log("ERROR", "MONITOR", f"Failed to restart '{name}': {err}")
        raise HTTPException(status_code=500, detail=err)
    record_log("SUCCESS", "MONITOR", f"Successfully restarted monitor '{name}'")
    return res


@app.post("/api/monitors/{name}/enable")
async def api_enable_monitor(name: str):
    """启动/开启指定监控器插件（从 stopped 状态恢复）"""
    record_log("INFO", "MONITOR", f"Triggered start/enable for monitor plugin '{name}'")
    loop = asyncio.get_event_loop()
    res = await loop.run_in_executor(None, bridge.enable_monitor, name)
    if not res.get("success"):
        err = res.get("error", "Failed to enable monitor")
        record_log("ERROR", "MONITOR", f"Failed to start '{name}': {err}")
        raise HTTPException(status_code=500, detail=err)
    record_log("SUCCESS", "MONITOR", f"Successfully started monitor '{name}'")
    return res


@app.post("/api/monitors/{name}/disable")
async def api_disable_monitor(name: str):
    """停止/禁用指定监控器插件"""
    record_log("INFO", "MONITOR", f"Triggered stop/disable for monitor plugin '{name}'")
    loop = asyncio.get_event_loop()
    res = await loop.run_in_executor(None, bridge.disable_monitor, name)
    if not res.get("success"):
        err = res.get("error", "Failed to disable monitor")
        record_log("ERROR", "MONITOR", f"Failed to stop '{name}': {err}")
        raise HTTPException(status_code=500, detail=err)
    record_log("SUCCESS", "MONITOR", f"Successfully stopped monitor '{name}'")
    return res


@app.get("/api/logs")
async def api_get_logs():
    """获取最近内存中的系统与操作审计日志"""
    return {"success": True, "logs": list(log_buffer)}


@app.get("/api/ebpf/health")
async def api_ebpf_health():
    """获取 8 大内核 eBPF 探针的加载状态、探针数及微秒级性能读写指标"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_ebpf_health)


@app.get("/api/ebpf/skb-drop")
async def api_skb_drop():
    """获取内核 Socket 丢包原因精确归因统计快照"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_skb_drop_stats)


@app.get("/api/ebpf/dns")
async def api_dns_stats():
    """获取 DNS eBPF 监控解析统计"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_dns_stats)


@app.get("/api/ebpf/wifi-loss")
async def api_wifi_loss_stats():
    """获取 Wi-Fi 协议链路层丢包统计"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_wifi_loss_stats)


@app.get("/api/ebpf/http-latency")
async def api_http_latency():
    """获取 HTTP 事务延迟统计指标"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_http_latency_stats)


@app.get("/api/ebpf/profiling")
async def api_process_profiling():
    """获取进程级网络流量与重传画像"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_process_profiling)


@app.get("/api/bluetooth/adapter")
async def api_bluetooth_adapter():
    """获取蓝牙适配器状态"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_bluetooth_adapter)


@app.get("/api/bluetooth/devices")
async def api_bluetooth_devices():
    """获取周围发现的蓝牙设备列表与信号评级"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_bluetooth_devices)


@app.get("/api/coexistence")
async def api_coexistence():
    """获取 Wi-Fi 与蓝牙 2.4GHz 射频共存与干扰分析"""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, bridge.get_coexistence_conflict)


@app.get("/api/history")
async def api_history(iface: str = "wlan0", limit: int = 60):
    """从 SQLite 查询时序历史记录，供前端趋势折线图渲染"""
    rows = db.query_history(iface=iface, max_points=limit)
    return {"success": True, "count": len(rows), "data": rows}


@app.get("/api/history/info")
async def api_history_info():
    """获取 SQLite 数据库元信息"""
    return {"success": True, "info": db.get_db_info()}


@app.get("/api/history/distribution")
async def api_history_distribution():
    """获取各质量等级历史占比分布"""
    return {"success": True, "distribution": db.query_quality_distribution()}


@app.post("/api/ai/diagnose")
async def api_ai_diagnose():
    """基于当前实时指标与 2.4GHz 射频共存情况，触发 RAG 知识库大模型专家诊断"""
    record_log("INFO", "AI", "User triggered AI-assisted root-cause diagnosis")
    health_res = bridge.get_health()
    conflict_res = bridge.get_coexistence_conflict()

    health_data = health_res.get("data", {}) if health_res.get("success") else {}
    conflict_data = conflict_res.get("data", {}) if conflict_res.get("success") else {}

    report = ai.diagnose_current_state(health_data, conflict_data)
    record_log("SUCCESS", "AI", f"Diagnosis generated with severity: {report.get('severity')}")
    return {"success": True, "report": report}


# ============================================================================
# WebSocket 接入
# ============================================================================

@app.websocket("/ws/live")
async def websocket_live_endpoint(websocket: WebSocket):
    await ws_manager.connect(websocket)
    try:
        while True:
            # 保持长连接活跃，接收客户端心跳
            data = await websocket.receive_text()
            if data == "ping":
                await websocket.send_text("pong")
    except WebSocketDisconnect:
        await ws_manager.disconnect(websocket)
    except Exception:
        await ws_manager.disconnect(websocket)


# ============================================================================
# 前端静态单页面托管
# ============================================================================

if os.path.exists(frontend_dir):
    app.mount("/static", StaticFiles(directory=frontend_dir), name="static")

    @app.get("/")
    async def serve_index():
        index_file = os.path.join(frontend_dir, "index.html")
        if os.path.exists(index_file):
            return FileResponse(index_file)
        return JSONResponse({"message": "Frontend index.html not found, but API is running", "docs": "/docs"})
else:
    @app.get("/")
    async def serve_fallback():
        return JSONResponse({"message": "WeakNet Web Gateway API is running", "docs": "/docs"})


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8080)
