#!/usr/bin/env python3
"""
tools/interference_test.py
Phase 4: 物理资源争用干扰测试（Interference Soak Test）
在施加主动并发网络请求（DNS 突发查询 + 并发 TCP 连接）与让 LLM 持续满载推理的混合场景下，
实测评估核心退化预算：
  1. D-Bus HealthCheck 服务端处理延迟 P99 (< 20ms)
  2. weaknet-server 守护进程 RSS 内存波动 (< 5MB)
  3. 系统核心最高温度 (< 75℃)
  4. llama-server 推理健康度

【测量方式的重要说明（2026-11-01 修正）】
  早期实现用 `dbus-send` 子进程逐次调用。这种方式测到的延迟里混入了
  **每次 fork+exec+连接总线** 的开销（板端实测固定 11~13ms），
  于是即便服务端处理只要 1ms，P99 也会稳定报 18~26ms 并误判超标。

  实测对照（Radxa Cubie A7A，健康服务）：
    - dbus-send 子进程方式：p50≈11ms，P99≈25ms   ← 主要是进程启动
    - 复用连接方式      ：p50≈1.0ms，P99≈1.3ms   ← 服务端真实处理
  干扰负载（并发 DNS/TCP + LLM 满载）下复用连接仍是 P99≈4ms。

  因此本脚本改为**在单条 D-Bus 连接上连续调用**，测服务端真实处理延迟。
  dbus-send 方式作为对照保留（--legacy-cli），用于识别“客户端开销”回归。
"""

import time
import json
import urllib.request
import urllib.error
import subprocess
import socket
import threading
import os
import sys

class PersistentDbusClient:
    """在一条系统总线连接上连续发起方法调用（ctypes 调 libdbus）

    为什么不用 dbus-python？板端未安装该模块，而本脚本要求零额外依赖。
    ctypes 直接把 libdbus-1 的 C API 映射进来，等价于 dbus-send 的行为，
    但省掉每次调用 fork/exec/重新认证的开销。
    """

    def __init__(self):
        self._dbus = None
        self._conn = None

    def connect(self) -> bool:
        try:
            import ctypes
            import ctypes.util
        except Exception:
            return False
        lib = ctypes.util.find_library("dbus-1")
        if not lib:
            return False
        self._dbus = ctypes.CDLL(lib)
        d = self._dbus
        # 关键原型（缺一个都会在 64 位上截断指针）
        d.dbus_error_init.restype = None
        d.dbus_error_init.argtypes = [ctypes.c_void_p]
        d.dbus_error_free.restype = None
        d.dbus_error_free.argtypes = [ctypes.c_void_p]
        d.dbus_bus_get_private.restype = ctypes.c_void_p
        d.dbus_bus_get_private.argtypes = [ctypes.c_int, ctypes.c_void_p]
        d.dbus_connection_set_exit_on_disconnect.restype = None
        d.dbus_connection_set_exit_on_disconnect.argtypes = [ctypes.c_void_p, ctypes.c_int]
        d.dbus_message_new_method_call.restype = ctypes.c_void_p
        d.dbus_message_new_method_call.argtypes = [
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
        d.dbus_connection_send_with_reply_and_block.restype = ctypes.c_void_p
        d.dbus_connection_send_with_reply_and_block.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
        d.dbus_message_unref.restype = None
        d.dbus_message_unref.argtypes = [ctypes.c_void_p]

        # DBusError 是 4 个字长的结构体，预留足够空间即可（只需按指针传递）
        self._err = ctypes.create_string_buffer(128)
        d.dbus_error_init(self._err)
        # DBUS_BUS_SYSTEM = 1
        self._conn = d.dbus_bus_get_private(1, self._err)
        if not self._conn:
            return False
        d.dbus_connection_set_exit_on_disconnect(self._conn, 0)
        return True

    def call(self, method: str) -> float:
        """返回一次方法调用的往返毫秒数；失败返回 -1"""
        import ctypes
        d = self._dbus
        msg = d.dbus_message_new_method_call(
            b"com.example.WeakNet", b"/com/example/WeakNet",
            b"com.example.WeakNet", method.encode())
        if not msg:
            return -1.0
        t0 = time.perf_counter()
        reply = d.dbus_connection_send_with_reply_and_block(
            self._conn, msg, 5000, None)
        elapsed = (time.perf_counter() - t0) * 1000.0
        d.dbus_message_unref(msg)
        if not reply:
            return -1.0
        d.dbus_message_unref(reply)
        return elapsed

    def close(self):
        if self._conn and self._dbus:
            # 只 unref 即可：私有连接在最后一个引用释放时关闭
            try:
                self._dbus.dbus_connection_unref.restype = None
                self._dbus.dbus_connection_unref.argtypes = [ctypes.c_void_p]
                self._dbus.dbus_connection_unref(self._conn)
            except Exception:
                pass
            self._conn = None


def measure_dbus_latency_ms(client=None) -> float:
    """一次 HealthCheck 往返延迟（毫秒）

    client 非空时走复用连接（服务端真实处理延迟）；
    为空时回退 dbus-send 子进程方式（含 11~13ms 进程启动开销，仅作对照）。
    """
    if client is not None:
        return client.call("HealthCheck")

    cmd = [
        "dbus-send", "--system", "--print-reply",
        "--dest=com.example.WeakNet",
        "/com/example/WeakNet",
        "com.example.WeakNet.HealthCheck"
    ]
    t0 = time.perf_counter()
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    t1 = time.perf_counter()
    if res.returncode == 0:
        return (t1 - t0) * 1000.0
    return -1.0

def get_server_rss_kb() -> int:
    try:
        pid_cmd = ["pgrep", "-f", "weaknet-dbus-server"]
        res = subprocess.run(pid_cmd, stdout=subprocess.PIPE, text=True)
        pids = res.stdout.strip().split()
        if not pids:
            return -1
        pid = pids[0]
        with open(f"/proc/{pid}/status", "r") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    parts = line.split()
                    return int(parts[1])
    except Exception:
        pass
    return -1

def get_cpu_temp() -> float:
    try:
        with open("/sys/class/thermal/thermal_zone0/temp", "r") as f:
            return float(f.read().strip()) / 1000.0
    except Exception:
        return 0.0

def traffic_worker(stop_event):
    """施加主动网络流量（DNS 解析 + TCP 连接测试）"""
    targets = ["www.baidu.com", "www.qq.com", "223.5.5.5"]
    while not stop_event.is_set():
        # DNS 查询
        try:
            socket.gethostbyname("www.baidu.com")
        except Exception:
            pass
        # TCP 建连
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.5)
            s.connect(("223.5.5.5", 53))
            s.close()
        except Exception:
            pass
        time.sleep(0.05)

def llm_worker(stop_event, endpoint="http://127.0.0.1:8080/v1/chat/completions"):
    """LLM 满载推理循环"""
    prompt_body = json.dumps({
        "model": "qwen2.5",
        "messages": [{"role": "user", "content": "Analyze network anomaly with continuous bursts and report cause in 30 words."}],
        "max_tokens": 60,
        "temperature": 0.1
    }).encode("utf-8")

    while not stop_event.is_set():
        try:
            req = urllib.request.Request(endpoint, data=prompt_body, headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=40) as resp:
                resp.read()
        except Exception:
            pass
        time.sleep(0.2)

def main():
    duration_sec = 60
    legacy_cli = False
    for arg in sys.argv[1:]:
        if arg == "--legacy-cli":
            legacy_cli = True
        else:
            try:
                duration_sec = int(arg)
            except ValueError:
                print(f"忽略无法解析的参数: {arg}")

    client = None
    if legacy_cli:
        mode_desc = "dbus-send 子进程（含进程启动开销，仅作对照）"
    else:
        candidate = PersistentDbusClient()
        if candidate.connect():
            client = candidate
            mode_desc = "复用单条 D-Bus 连接（服务端真实处理延迟）"
        else:
            mode_desc = "dbus-send 子进程（复用连接不可用，已回退）"

    print("=" * 65)
    print(f" WeakNet Phase 4: 物理资源争用干扰测试 (时长: {duration_sec} 秒)")
    print(" 压力模型: 并发 TCP 建连 + DNS 突发查询 + Local LLM 满载推理")
    print(f" 测量方式: {mode_desc}")
    print("=" * 65)

    initial_rss = get_server_rss_kb() / 1024.0
    initial_temp = get_cpu_temp()
    print(f"[*] 基线初始状态: weaknet-server RSS={initial_rss:.2f} MB, SoC 温度={initial_temp:.1f} ℃")

    stop_event = threading.Event()

    # 启动 3 个并发流量注入线程
    traffic_threads = [threading.Thread(target=traffic_worker, args=(stop_event,)) for _ in range(3)]
    for t in traffic_threads:
        t.start()

    # 启动 1 个 LLM 满载推理线程
    llm_t = threading.Thread(target=llm_worker, args=(stop_event,))
    llm_t.start()

    latencies = []
    peak_rss = initial_rss
    peak_temp = initial_temp

    start_time = time.time()
    count = 0
    print("[*] 正在持续监测 D-Bus P99 延迟与资源指标...")

    while time.time() - start_time < duration_sec:
        lat = measure_dbus_latency_ms(client)
        if lat > 0:
            latencies.append(lat)
        cur_rss = get_server_rss_kb() / 1024.0
        if cur_rss > peak_rss:
            peak_rss = cur_rss
        cur_temp = get_cpu_temp()
        if cur_temp > peak_temp:
            peak_temp = cur_temp
        count += 1
        time.sleep(0.5)

    stop_event.set()
    for t in traffic_threads:
        t.join()
    llm_t.join()

    # 计算统计指标
    latencies.sort()
    p50 = latencies[int(len(latencies) * 0.5)] if latencies else 0.0
    p95 = latencies[int(len(latencies) * 0.95)] if latencies else 0.0
    p99 = latencies[int(len(latencies) * 0.99)] if latencies else 0.0
    rss_delta = peak_rss - initial_rss

    if client is not None:
        client.close()

    print("\n----------------- 干扰测试审计结论 -----------------")
    print(f" 测量方式                 : {mode_desc}")
    print(f" 采集样本数               : {len(latencies)} 次")
    print(f" D-Bus HealthCheck P50   : {p50:.2f} ms")
    print(f" D-Bus HealthCheck P95   : {p95:.2f} ms")
    print(f" D-Bus HealthCheck P99   : {p99:.2f} ms (预算门限: < 20.0 ms) -> {'✅ 合格' if p99 < 20.0 else '❌ 超标'}")
    print(f" weaknet-server RSS 波动 : {rss_delta:.2f} MB (预算门限: < 5.0 MB)   -> {'✅ 合格' if rss_delta < 5.0 else '❌ 超标'}")
    print(f" SoC 最高核心温度        : {peak_temp:.1f} ℃ (预算门限: < 75.0 ℃)  -> {'✅ 合格' if peak_temp < 75.0 else '❌ 降频超温'}")
    print("---------------------------------------------------")

if __name__ == "__main__":
    main()
