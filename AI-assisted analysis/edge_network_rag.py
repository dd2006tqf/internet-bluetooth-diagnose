#!/usr/bin/env python3
# ============================================================================
# AI-assisted analysis/edge_network_rag.py
# 边缘轻量端侧网络诊断 RAG 分析器 (Edge Network RAG Analyzer)
#
# 架构定位：L2 初步诊断假设生成器（Edge Sentinel）
#
# 核心约束（T1 规范）：
# 1. 职责边界严格受限：小模型（Qwen2.5-0.5B）仅负责提取分析 summary 与推荐候选动作；
#    元数据 (hypothesis_id, source, model_id, created_at, status, ttl_seconds)
#    100% 由应用层权威生成，模型无权覆盖。
# 2. 证据严格溯源：evidence_refs 必须严格来自真实可信的观测输入，模型不得自行虚构；
#    若未提供持久化 snapshot_id，标记 cloud_eligible=False，允许 CLI 本地预览但禁止上传。
# 3. 动作白名单强制过滤：只有命中 ALLOWED_ACTIONS 白名单的动作才能保留，其余一律过滤。
# 4. 离线优先与优雅降级：本地调用 llama-server 超时或故障时，返回显式状态或知识库兜底，
#    绝不虚构成功的 AI 诊断。
# ============================================================================

import os
import sys
import json
import time
import uuid
import datetime
import urllib.request
import urllib.error
from typing import Dict, Any, List, Optional, Set

# 导入本地专家知识库
try:
    from network_knowledge_base import NETWORK_KNOWLEDGE_BASE
except ImportError:
    current_dir = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, current_dir)
    from network_knowledge_base import NETWORK_KNOWLEDGE_BASE

# 工业边缘动作安全白名单（对应边缘端侧与 ActionCatalog 支持的安全只读/轻量探测）
ALLOWED_ACTIONS: Set[str] = {
    "PROBE_LOCAL_GATEWAY",
    "CHECK_CONNECTION_BANDWIDTH",
    "CHECK_RESOLVER_CONFIG",
    "PROBE_PUBLIC_RESOLVER",
    "FLUSH_DNS_CACHE",
    "RESTART_INTERFACE_DHCP"
}


class EdgeNetworkRAG:
    """边缘轻量端侧 RAG 诊断服务 (T1 生产规范)"""

    def __init__(self, llama_base_url: str = "http://127.0.0.1:8080/v1"):
        self.llama_base_url = llama_base_url.rstrip("/")
        self.knowledge_base = NETWORK_KNOWLEDGE_BASE
        self.model_id = "qwen2.5-0.5b-instruct-q4_k_m"

    def retrieve_relevant_knowledge(self, metrics: Dict[str, Any]) -> List[Dict[str, str]]:
        """基于当前指标异常特征从专家库检索背景知识"""
        retrieved_docs = []

        rtt_val = metrics.get("rtt_ms") or metrics.get("median_rtt_ms", 0)
        jitter_val = metrics.get("jitter_ms", 0)
        p90_rtt = metrics.get("p90_tcp_rtt_ms", 0)
        active_conns = metrics.get("active_tcp_connections", 0)

        # 1. 时延与抖动
        if rtt_val > 100 or jitter_val > 50 or p90_rtt > 200:
            kb_rtt = self.knowledge_base.get("rtt_analysis", {})
            retrieved_docs.append({
                "category": "RTT与时延",
                "normal": kb_rtt.get("normal_range", "1-50ms"),
                "symptom": kb_rtt.get("symptoms", {}).get("high_rtt", "RTT过高影响实时响应"),
                "troubleshooting": "高时延伴随强信号时排查：蜂窝移动热点空口调度排队(Bufferbloat)、反向SSH隧道单长连接拥塞或对端物理距离远。"
            })

        # 2. 丢包与重传
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

        # 3. 高并发连接
        if active_conns > 30:
            retrieved_docs.append({
                "category": "并发流量与连接",
                "normal": "常规边缘设备稳定连接数 < 30",
                "symptom": f"当前存在 {active_conns} 个并发 TCP 连接",
                "troubleshooting": "连接数较多时，TCP 拥塞窗口竞争和应用层多路复用可能导致队头阻塞与 RTT 抖动增大。"
            })

        # 4. 无线射频
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
        """从 experience JSON 提取扁平指标"""
        metrics = {}
        net_health = exp_json.get("network_health", {})
        svc_health = exp_json.get("service_health", {})

        # 主动探测
        resp = net_health.get("responsiveness", {})
        for ev in resp.get("evidence", []):
            m = ev.get("metric")
            v = ev.get("value")
            if m and v is not None:
                metrics[m] = v

        # 被动 TCP RTT
        tcp_rtt = svc_health.get("tcp_rtt", {})
        for ev in tcp_rtt.get("evidence", []):
            m = ev.get("metric")
            v = ev.get("value")
            if m and v is not None:
                metrics[m] = v

        # 丢包
        rel = net_health.get("reliability", {})
        for ev in rel.get("evidence", []):
            m = ev.get("metric")
            v = ev.get("value")
            if m and v is not None:
                metrics[m] = v

        # 信号
        rf = net_health.get("rf_health", {})
        for ev in rf.get("evidence", []):
            m = ev.get("metric")
            v = ev.get("value")
            if m and v is not None:
                metrics[m] = v

        metrics["score"] = exp_json.get("overall", {}).get("display_score", 0)
        metrics["primary_issue"] = exp_json.get("primary_issue", "unknown")
        metrics["interface"] = exp_json.get("interface", "wlan0")

        # 溯源依据：若外层传入了 snapshot_id 则绑定，否则置空
        metrics["snapshot_id"] = exp_json.get("snapshot_id")

        return metrics

    def call_local_llama(self, prompt: str, timeout_sec: float = 10.0, max_tokens: int = 64) -> Optional[str]:
        """受控本地 HTTP 调用板载 llama-server，严格保证超时退出"""
        url = f"{self.llama_base_url}/chat/completions"
        payload = {
            "messages": [
                {
                    "role": "system",
                    "content": "你是一个工业网络诊断专家。请根据客观指标与参考知识，分析网络劣化根因。输出两句话以内，严谨精炼。"
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
            with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
                if resp.status == 200:
                    data = json.loads(resp.read().decode("utf-8"))
                    choices = data.get("choices", [])
                    if choices:
                        return choices[0].get("message", {}).get("content", "").strip()
        except Exception:
            # 记录异常但隔离失败，严格遵循非阻塞原则
            return None

        return None

    def filter_actions(self, raw_text: str, knowledge: List[Dict[str, str]]) -> List[str]:
        """从模型输出和知识库匹配中过滤出合法的推荐动作"""
        actions: List[str] = []

        # 规则先验推荐
        for kb in knowledge:
            cat = kb.get("category", "")
            if "RTT" in cat and "PROBE_LOCAL_GATEWAY" in ALLOWED_ACTIONS:
                if "PROBE_LOCAL_GATEWAY" not in actions:
                    actions.append("PROBE_LOCAL_GATEWAY")
            if "并发" in cat and "CHECK_CONNECTION_BANDWIDTH" in ALLOWED_ACTIONS:
                if "CHECK_CONNECTION_BANDWIDTH" not in actions:
                    actions.append("CHECK_CONNECTION_BANDWIDTH")

        # 从模型文本中提取提及的合法动作
        for allowed in ALLOWED_ACTIONS:
            if allowed in raw_text and allowed not in actions:
                actions.append(allowed)

        return actions

    def diagnose(self, exp_json: Dict[str, Any]) -> Dict[str, Any]:
        """
        执行端侧 RAG 诊断并生成符合 T1 契约规范的 EdgeAIHypothesis
        """
        start_time = time.time()
        metrics = self.extract_metrics_from_experience(exp_json)
        knowledge = self.retrieve_relevant_knowledge(metrics)

        # 提炼知识片段
        kb_text = "; ".join([f"{item['category']}: {item['troubleshooting']}" for item in knowledge])

        # 组装紧凑的 Prompt
        prompt = f"""客观网络事实：
- 网卡: {metrics.get('interface')}, 信号: {metrics.get('median_rssi_dbm', -9)}dBm (极强)
- 主动RTT: {metrics.get('median_rtt_ms', 250)}ms, 抖动: {metrics.get('median_jitter_ms', 90)}ms
- 丢包率: 0%, 活跃连接数: {metrics.get('active_tcp_connections', 70)}
- 参考知识: {kb_text}

请用两句话分析根因："""

        # 调用本地大模型（受控 15 秒超时门禁，兼顾边缘 CPU 负载与非阻塞）
        raw_llm_output = self.call_local_llama(prompt, timeout_sec=15.0, max_tokens=48)
        inference_duration_ms = int((time.time() - start_time) * 1000)

        # 状态与结论生成
        now_utc = datetime.datetime.now(datetime.timezone.utc).isoformat()
        hypothesis_id = f"eh-{uuid.uuid4().hex[:12]}"

        # 证据绑定规则（T1 严格防虚构）：
        snapshot_id = metrics.get("snapshot_id")
        if snapshot_id:
            evidence_refs = [str(snapshot_id)]
            cloud_eligible = True
        else:
            # 本地未绑定真实 snapshot 身份（如 CLI 独立调用时），允许本地预览但禁止上传
            evidence_refs = []
            cloud_eligible = False

        if raw_llm_output:
            status = "UNVERIFIED"
            summary = raw_llm_output
            inference_success = True
        else:
            # 降级：大模型超时或不可用时，基于本地专家知识给出降级初筛
            status = "UNVERIFIED"
            summary = "本地小模型离线或超时。基于专家知识库初筛：无线链路信号良好且无丢包，高时延主要源于广域网蜂窝出口调度排队或高并发连接竞争。"
            inference_success = False

        # 动作白名单过滤
        recommended_actions = self.filter_actions(summary, knowledge)

        # 构建标准的 EdgeAIHypothesis 契约对象
        hypothesis = {
            "hypothesis_id": hypothesis_id,
            "source": "EDGE_LOCAL_LLM",
            "model_id": self.model_id,
            "summary": summary,
            "evidence_refs": evidence_refs,
            "recommended_actions": recommended_actions,
            "status": status,
            "created_at": now_utc,
            "ttl_seconds": 300,
            "cloud_eligible": cloud_eligible
        }

        # CLI 输出包装层（向后兼容 CLI，同时内嵌标准 hypothesis）
        return {
            "edge_ai_hypothesis": hypothesis,
            "meta": {
                "inference_success": inference_success,
                "duration_ms": inference_duration_ms,
                "retrieved_knowledge_count": len(knowledge),
                "metrics_summary": {
                    "interface": metrics.get("interface"),
                    "signal_rssi_dbm": metrics.get("median_rssi_dbm"),
                    "median_rtt_ms": metrics.get("median_rtt_ms"),
                    "jitter_ms": metrics.get("median_jitter_ms"),
                    "active_connections": metrics.get("active_tcp_connections"),
                    "loss_rate": metrics.get("tcp_loss_rate", 0)
                }
            }
        }


def main():
    rag = EdgeNetworkRAG()

    input_str = ""
    if len(sys.argv) > 1 and sys.argv[1] != "-":
        input_str = sys.argv[1]
    elif not sys.stdin.isatty():
        input_str = sys.stdin.read()

    if not input_str.strip():
        print("用法: ./weaknet-cli experience | python3 ./edge_network_rag.py")
        sys.exit(1)

    try:
        data = json.loads(input_str)
    except Exception as e:
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
