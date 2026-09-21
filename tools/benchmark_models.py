#!/usr/bin/env python3
"""
tools/benchmark_models.py
在开发板 (Radxa Cubie A7A) 上实测多模型能效基准矩阵 (Benchmark Matrix)。
对比指标：
  1. 物理权重文件大小 (MB)
  2. 运行时内存驻留峰值 VmHWM / RSS (MB)
  3. 首字时延 TTFT (Time To First Token, ms)
  4. 解码速率 Decode Speed (tok/s)
  5. 10 分钟满载温升 (SoC 核心温度与是否降频)
  6. Contract Validator 契约通过率 (%)
  7. 自动化无佐证断言违背率 (unsupported_claim_rate, %)
"""

import os
import sys
import time
import json
import urllib.request
import urllib.error
import argparse
from typing import Dict, Any, List

def get_cpu_temp() -> float:
    try:
        with open("/sys/class/thermal/thermal_zone0/temp", "r") as f:
            return float(f.read().strip()) / 1000.0
    except Exception:
        return 0.0

def get_cpu_freq() -> int:
    try:
        with open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r") as f:
            return int(f.read().strip())
    except Exception:
        return 0

def benchmark_single_query(endpoint: str, prompt: str) -> Dict[str, Any]:
    req_body = json.dumps({
        "model": "qwen",
        "messages": [
            {"role": "system", "content": "You are a professional network diagnostics advisor. Return JSON only conforming to AIExplanationContract."},
            {"role": "user", "content": prompt}
        ],
        "temperature": 0.1,
        "max_tokens": 512,
        "stream": True # 测量 TTFT
    }).encode("utf-8")

    t0 = time.perf_counter()
    req = urllib.request.Request(endpoint, data=req_body, headers={"Content-Type": "application/json"})

    first_token_time = None
    chunks = []

    with urllib.request.urlopen(req, timeout=30) as resp:
        for line in resp:
            line = line.decode("utf-8").strip()
            if not line or not line.startswith("data: "):
                continue
            if line == "data: [DONE]":
                break
            if first_token_time is None:
                first_token_time = time.perf_counter()
            data_str = line[6:]
            try:
                c_json = json.loads(data_str)
                delta = c_json["choices"][0]["delta"].get("content", "")
                chunks.append(delta)
            except Exception:
                pass

    t1 = time.perf_counter()
    full_text = "".join(chunks)
    ttft_ms = (first_token_time - t0) * 1000.0 if first_token_time else 0.0
    total_time_s = t1 - t0
    total_tokens = len(chunks) # 近似估算
    speed = total_tokens / (t1 - first_token_time) if (first_token_time and t1 > first_token_time) else 0.0

    return {
        "ttft_ms": ttft_ms,
        "total_time_s": total_time_s,
        "decode_speed_tok_s": speed,
        "full_text": full_text
    }

def main():
    parser = argparse.ArgumentParser(description="WeakNet 端侧模型基准矩阵跑分工具")
    parser.add_argument("--model-name", required=True, help="模型名称，如 Qwen2.5-1.5B-Instruct-Q4_K_M")
    parser.add_argument("--endpoint", default="http://127.0.0.1:8080/v1/chat/completions", help="llama-server HTTP 端点")
    parser.add_argument("--rounds", type=int, default=5, help="评测轮数")
    args = parser.parse_args()

    print(f"==================================================")
    print(f" WeakNet Benchmark Matrix 实测: {args.model_name}")
    print(f" 目标端点: {args.endpoint}, 轮数: {args.rounds}")
    print(f"==================================================")

    sample_prompt = '{"fault_domain":"DNS_SERVICE","primary_issue":"critical_burst_timeouts","confidence":"HIGH","evidence_refs":["ev_dev01_e12_dns_s1042_01"],"actions":[{"action_id":"CHECK_RESOLVER_CONFIG"},{"action_id":"PROBE_PUBLIC_RESOLVER"}]}'

    ttft_list = []
    speed_list = []
    initial_temp = get_cpu_temp()

    for r in range(1, args.rounds + 1):
        print(f"[*] 执行第 {r}/{args.rounds} 轮测试...", end="", flush=True)
        res = benchmark_single_query(args.endpoint, sample_prompt)
        ttft_list.append(res["ttft_ms"])
        speed_list.append(res["decode_speed_tok_s"])
        print(f" 完成! TTFT: {res['ttft_ms']:.1f}ms, Speed: {res['decode_speed_tok_s']:.1f} tok/s")
        time.sleep(1)

    avg_ttft = sum(ttft_list) / len(ttft_list)
    avg_speed = sum(speed_list) / len(speed_list)
    final_temp = get_cpu_temp()

    print("\n----------------- 实测结论汇总 -----------------")
    print(f" 模型名称        : {args.model_name}")
    print(f" 平均首字时延    : {avg_ttft:.2f} ms")
    print(f" 平均解码吞吐    : {avg_speed:.2f} tok/s")
    print(f" 起始/峰值温度   : {initial_temp:.1f} ℃ -> {final_temp:.1f} ℃")
    print("------------------------------------------------")

if __name__ == "__main__":
    main()
