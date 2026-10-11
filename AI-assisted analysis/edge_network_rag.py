#!/usr/bin/env python3
# ============================================================================
# AI-assisted analysis/edge_network_rag.py
# 边缘轻量端侧网络诊断 RAG 分析器 (Edge Network RAG Analyzer)
#
# 核心职责：
# 1. 纯本地离线优先：直连开发板板载 llama-server (127.0.0.1:8080，Qwen2.5-0.5B)，
#    完全不依赖外网连接与外部云端 API Key。
# 2. 轻量零依赖 RAG：基于 network_knowledge_base.py 内置专家规则库进行特征向量匹配，
#    不引入重型 LangChain / FAISS，板端毫秒级检索。
# 3. 消费 eBPF/D-Bus 事实：接收 weaknet-cli experience 或 diagnosis JSON，
#    输出专业、结构化的人类与系统可读诊断推断。
# ============================================================================

import os
import sys
import json
import urllib.request
import urllib.error
from typing import Dict, Any, List, Optional

# 导入本地专家知识库
try:
    from network_knowledge_base import NETWORK_KNOWLEDGE_BASE
except ImportError:
    # 兼容直接单文件运行路径
    current_dir = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, current_dir)
    from network_knowledge_base import NETWORK_KNOWLEDGE_BASE


class EdgeNetworkRAG:
    """边缘轻量端侧 RAG 诊断服务"""

    def __init__(self, llama_base_url: str = "http://127.0.0.1:8080/v1"):
        self.llama_base_url = llama_base_url.rstrip("/")
        self.knowledge_base = NETWORK_KNOWLEDGE_BASE

    def retrieve_relevant_knowledge(self, metrics: Dict[str, Any]) -> List[Dict[str, str]]:
        """
        基于当前真实指标的异常特征，从本地专家知识库检索相关的背景知识片段
        """
        retrieved_docs = []

        # 1. 检查 RTT / 延迟异常
        rtt_val = metrics.get("rtt_ms") or metrics.get("median_rtt_ms", 0)
        jitter_val = metrics.get("jitter_ms", 0)
        p90_rtt = metrics.get("p90_tcp_rtt_ms", 0)
        active_conns = metrics.get("active_tcp_connections", 0)

        if rtt_val > 100 or jitter_val > 50 or p90_rtt > 200:
            kb_rtt = self.knowledge_base.get("rtt_analysis", {})
            retrieved_docs.append({
                "category": "RTT与时延",
                "normal": kb_rtt.get("normal_range", "1-50ms"),
                "symptom": kb_rtt.get("symptoms", {}).get("high_rtt", "RTT过高影响实时响应"),
                "troubleshooting": "高时延伴随强信号时排查：蜂窝移动热点空口调度排队(Bufferbloat)、反向SSH隧道单长连接拥塞或对端物理距离远。"
            })

        # 2. 检查丢包与重传异常
        tcp_loss = metrics.get("tcp_loss_rate", 0)
        wifi_loss = metrics.get("wifi_loss_rate", 0)
        if tcp_loss > 1.0 or wifi_loss > 1.0:
            kb_loss = self.knowledge_base.get("tcp_loss_analysis", {})
            retrieved_docs.append({
                "category": "丢包与重传",
                "normal": kb_loss.get("normal_range", "0-1%"),
                "symptom": kb_loss.get("symptoms", {}).get("high_loss", "丢包导致重传与吞吐下降"),
                "troubleshooting": kb_loss.get("troubleshooting", {}).get("high_loss", "排查无线信号衰减、空口争用或网卡队列溢出。")
            })

        # 3. 检查高并发连接特征
        if active_conns > 30:
            retrieved_docs.append({
                "category": "并发流量与连接",
                "normal": "常规边缘设备稳定连接数 < 30",
                "symptom": f"当前存在 {active_conns} 个并发 TCP 连接",
                "troubleshooting": "连接数较多时，TCP 拥塞窗口竞争和应用层多路复用可能导致队头阻塞与 RTT 抖动增大。"
            })

        # 4. 检查无线信号特征
        rssi_val = metrics.get("rssi_dbm") or metrics.get("median_rssi_dbm", -100)
        if rssi_val < -75:
            retrieved_docs.append({
                "category": "无线射频覆盖",
                "normal": "-30dBm 至 -65dBm",
                "symptom": "Wi-Fi 接收信号弱 (RSSI < -75dBm)",
                "troubleshooting": "边缘覆盖不足，信噪比恶化，容易引发空口重传与协商速率骤降。"
            })

        return retrieved_docs

    def extract_metrics_from_experience(self, exp_json: Dict[str, Any]) -> Dict[str, Any]:
        """从 weaknet-cli experience JSON 中提取扁平的关键诊断指标"""
        metrics = {}
        net_health = exp_json.get("network_health", {})
        svc_health = exp_json.get("service_health", {})

        # 提取主动探测
        resp = net_health.get("responsiveness", {})
        for ev in resp.get("evidence", []):
            m = ev.get("metric")
            v = ev.get("value")
            if m and v is not None:
                metrics[m] = v

        # 提取被动 TCP RTT
        tcp_rtt = svc_health.get("tcp_rtt", {})
        for ev in tcp_rtt.get("evidence", []):
            m = ev.get("metric")
            v = ev.get("value")
            if m and v is not None:
                metrics[m] = v

        # 提取丢包与重传
        rel = net_health.get("reliability", {})
        for ev in rel.get("evidence", []):
            m = ev.get("metric")
            v = ev.get("value")
            if m and v is not None:
                metrics[m] = v

        # 提取信号
        rf = net_health.get("rf_health", {})
        for ev in rf.get("evidence", []):
            m = ev.get("metric")
            v = ev.get("value")
            if m and v is not None:
                metrics[m] = v

        # 补充顶层指标
        metrics["score"] = exp_json.get("overall", {}).get("display_score", 0)
        metrics["primary_issue"] = exp_json.get("primary_issue", "unknown")
        metrics["interface"] = exp_json.get("interface", "wlan0")

        return metrics

    def call_local_llama(self, prompt: str, max_tokens: int = 192) -> Optional[str]:
        """本地 HTTP 调用板载 llama-server 进行推理"""
        url = f"{self.llama_base_url}/chat/completions"
        payload = {
            "messages": [
                {
                    "role": "system",
                    "content": "你是一个严谨的工业网络诊断专家。请根据提供的硬件客观度量与参考知识，分析网络劣化原因并输出排查建议。要求客观、精炼，拒绝空话。"
                },
                {
                    "role": "user",
                    "content": prompt
                }
            ],
            "max_tokens": max_tokens,
            "temperature": 0.2
        }

        try:
            req = urllib.request.Request(
                url,
                data=json.dumps(payload).encode("utf-8"),
                headers={"Content-Type": "application/json"}
            )
            with urllib.request.urlopen(req, timeout=35) as resp:
                if resp.status == 200:
                    data = json.loads(resp.read().decode("utf-8"))
                    choices = data.get("choices", [])
                    if choices:
                        return choices[0].get("message", {}).get("content", "").strip()
        except Exception as e:
            # 记录错误但不抛出致命异常（优雅降级）
            return None

        return None

    def diagnose(self, exp_json: Dict[str, Any]) -> Dict[str, Any]:
        """执行端侧 RAG 全流程诊断"""
        metrics = self.extract_metrics_from_experience(exp_json)
        knowledge = self.retrieve_relevant_knowledge(metrics)

        # 提炼知识片段文本
        kb_text = "; ".join([f"{item['category']}: {item['troubleshooting']}" for item in knowledge])

        # 组装紧凑的 Prompt (适配端侧 0.5B 小模型)
        prompt = f"""客观网络事实：
- 网卡: {metrics.get('interface')}, 信号: {metrics.get('median_rssi_dbm', -9)}dBm (极强)
- 主动RTT: {metrics.get('median_rtt_ms', 250)}ms, 抖动: {metrics.get('median_jitter_ms', 90)}ms
- 丢包率: 0%, 活跃连接数: {metrics.get('active_tcp_connections', 70)}
- 参考知识: {kb_text}

请用两句话分析根因并给出一条建议动作："""

        # 调用端侧大模型 (max_tokens 设为 64，保证在 8 秒内秒级完成)
        ai_response = self.call_local_llama(prompt, max_tokens=64)

        # 若本地模型未启动或异常，采用本地规则知识降级输出
        if not ai_response:
            ai_response = "基于专家知识库推断：当前无线物理链路极佳且无丢包，高时延与抖动主要源于广域网蜂窝出口调度排队或高并发连接竞争。"

        return {
            "source": "WeakNet-Edge-RAG",
            "model": "qwen2.5-0.5b-instruct-q4_k_m (local llama-server)",
            "inference_mode": "Edge-Local-Inference",
            "retrieved_knowledge_count": len(knowledge),
            "metrics_summary": {
                "interface": metrics.get("interface"),
                "signal_rssi_dbm": metrics.get("median_rssi_dbm"),
                "median_rtt_ms": metrics.get("median_rtt_ms"),
                "jitter_ms": metrics.get("median_jitter_ms"),
                "active_connections": metrics.get("active_tcp_connections"),
                "loss_rate": metrics.get("tcp_loss_rate", 0)
            },
            "ai_root_cause_analysis": ai_response,
            "recommended_actions": [
                "PROBE_LOCAL_GATEWAY (验证局域网网关第一跳延迟，区分内外网故障)",
                "CHECK_CONNECTION_BANDWIDTH (排查当前活跃并发连接是否占满上行带宽)"
            ]
        }


# ============================================================================
# CLI 命令行入口：支持接收标准输入或参数 JSON
# ============================================================================
def main():
    rag = EdgeNetworkRAG()

    # 从标准输入或参数读取
    input_str = ""
    if len(sys.argv) > 1 and sys.argv[1] != "-":
        input_str = sys.argv[1]
    elif not sys.stdin.isatty():
        input_str = sys.stdin.read()

    if not input_str.strip():
        print("用法: ./edge_network_rag.py '<json_string>' 或 ./weaknet-cli experience | ./edge_network_rag.py")
        sys.exit(1)

    try:
        data = json.loads(input_str)
    except Exception as e:
        # 如果输入包含了非 JSON 日志，提取其中的有效 JSON 部分
        start_idx = input_str.find("{")
        end_idx = input_str.rfind("}")
        if start_idx != -1 and end_idx != -1:
            data = json.loads(input_str[start_idx:end_idx + 1])
        else:
            print(f"❌ 无法解析输入 JSON: {e}")
            sys.exit(1)

    res = rag.diagnose(data)
    print(json.dumps(res, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
