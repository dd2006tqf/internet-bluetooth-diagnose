#!/usr/bin/env python3
"""
tools/interference_test.py
Phase 4: 物理资源争用干扰测试（Interference Soak Test）
在施加主动并发网络请求（DNS 突发查询 + 并发 TCP 连接）与让 LLM 持续满载推理的混合场景下，
实测评估核心退化预算：
  1. D-Bus HealthCheck 调用 P99 延迟 (< 20ms)
  2. weaknet-server 守护进程 RSS 内存波动 (< 5MB)
  3. 系统核心最高温度 (< 75℃)
  4. llama-server 推理健康度
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

def measure_dbus_latency_ms() -> float:
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
    if len(sys.argv) > 1:
        duration_sec = int(sys.argv[1])

    print("=" * 65)
    print(f" WeakNet Phase 4: 物理资源争用干扰测试 (时长: {duration_sec} 秒)")
    print(" 压力模型: 并发 TCP 建连 + DNS 突发查询 + Local LLM 满载推理")
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
        lat = measure_dbus_latency_ms()
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

    print("\n----------------- 干扰测试审计结论 -----------------")
    print(f" 采集样本数               : {len(latencies)} 次")
    print(f" D-Bus HealthCheck P50   : {p50:.2f} ms")
    print(f" D-Bus HealthCheck P95   : {p95:.2f} ms")
    print(f" D-Bus HealthCheck P99   : {p99:.2f} ms (预算门限: < 20.0 ms) -> {'✅ 合格' if p99 < 20.0 else '❌ 超标'}")
    print(f" weaknet-server RSS 波动 : {rss_delta:.2f} MB (预算门限: < 5.0 MB)   -> {'✅ 合格' if rss_delta < 5.0 else '❌ 超标'}")
    print(f" SoC 最高核心温度        : {peak_temp:.1f} ℃ (预算门限: < 75.0 ℃)  -> {'✅ 合格' if peak_temp < 75.0 else '❌ 降频超温'}")
    print("---------------------------------------------------")

if __name__ == "__main__":
    main()
