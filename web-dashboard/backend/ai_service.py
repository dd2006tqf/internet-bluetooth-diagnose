"""
ai_service.py
封装 WeakNet 本地 RAG 知识库与专家诊断规则，为 Web 仪表盘提供实时多维度故障归因。
"""

import logging
import os
import sys
from typing import Any, Dict, List

# 引入已有的 AI-assisted analysis 模块
current_dir = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.abspath(os.path.join(current_dir, "..", ".."))
ai_analysis_dir = os.path.join(repo_root, "AI-assisted analysis")
if ai_analysis_dir not in sys.path:
    sys.path.insert(0, ai_analysis_dir)

logger = logging.getLogger("weaknet.ai")

try:
    from network_knowledge_base import (
        NETWORK_KNOWLEDGE_BASE,
        query_bluetooth_diagnosis
    )
    AI_MODULE_AVAILABLE = True
except Exception as e:
    logger.warning("Could not load network_knowledge_base: %s. Using internal fallback rules.", e)
    AI_MODULE_AVAILABLE = False


class AiDiagnosisService:
    def __init__(self):
        self.available = AI_MODULE_AVAILABLE

    def diagnose_current_state(self, health_data: Dict[str, Any], conflict_data: Dict[str, Any]) -> Dict[str, Any]:
        """
        基于当前实时网络质量与蓝牙共存指标，生成专家诊断报告
        """
        findings = []
        recommendations = []
        severity = "NORMAL"

        rtt = health_data.get("rtt_ms", 0)
        rssi = health_data.get("rssi_dbm", -1000)
        tcp_loss = health_data.get("tcp_loss_rate") or 0.0
        score = health_data.get("quality_score") or health_data.get("overall_score") or 100.0
        issues = health_data.get("issues", [])

        # 1. 延迟分析
        if rtt > 200:
            severity = "WARNING" if severity == "NORMAL" else severity
            findings.append({
                "dim": "RTT (网络往返延迟)",
                "level": "CRITICAL" if rtt > 500 else "HIGH",
                "desc": f"当前往返延迟达到 {rtt}ms，远高于健康网络阈值 (80ms)。",
                "cause": "可能为上行蜂窝/宽带拥塞、远端网关距离过远或局域网信道竞争加剧。"
            })
            recommendations.append("优先检查默认网关与上行路由器负载，或尝试调整 DNS 优先解析低延迟节点。")

        # 2. 信号衰减分析
        if -1000 < rssi < -75:
            severity = "WARNING" if severity == "NORMAL" else severity
            findings.append({
                "dim": "RSSI (Wi-Fi 信号强度)",
                "level": "HIGH" if rssi < -82 else "MEDIUM",
                "desc": f"当前 Wi-Fi 信号为 {rssi}dBm，处于弱信号边缘。",
                "cause": "设备与无线 AP 间存在较厚障碍物遮挡、天线增益不足或距离过远。"
            })
            recommendations.append("调整开发板板载天线朝向，或缩短与无线路由器之间的直线距离，避免多重实体墙阻隔。")

        # 3. 内核丢包分析
        if tcp_loss > 1.0:
            severity = "CRITICAL"
            findings.append({
                "dim": "TCP 内核重传与丢包",
                "level": "CRITICAL",
                "desc": f"TCP 内核协议栈监测到 {tcp_loss:.2f}% 的活跃丢包率。",
                "cause": "网络底层发生队列溢出、报文校验和错或中间路由器主动限速丢包。"
            })
            recommendations.append("运行 'skb-drop' 探针排查具体丢弃原因（如 NETFILTER 防火墙拒绝或校验和错误）。")

        # 4. 2.4GHz 射频共存冲突分析
        if conflict_data and conflict_data.get("detected"):
            confidence = conflict_data.get("confidence", 0)
            wifi_band = conflict_data.get("wifi_band", "2.4GHz")
            wifi_drop = conflict_data.get("wifi_rssi_drop", 0)
            bt_drop = conflict_data.get("bt_rssi_drop", 0)

            severity = "CRITICAL" if confidence > 70 else "WARNING"
            findings.append({
                "dim": "2.4GHz Wi-Fi / 蓝牙共存射频干扰",
                "level": "HIGH",
                "desc": f"监测到 Wi-Fi 与蓝牙 RSSI 同步衰减 (Wi-Fi 跌落 {wifi_drop}dBm, 蓝牙跌落 {bt_drop}dBm)，置信度 {confidence:.1f}%。",
                "cause": f"Wi-Fi 工作在 {wifi_band} 频段，与蓝牙天线物理距离过近或信道产生互调微波干扰。"
            })

            # 联动现有知识库生成专业建议
            if self.available:
                rag_sugg = query_bluetooth_diagnosis("蓝牙卡顿")
                recommendations.append(f"射频优化建议：{rag_sugg}")
            else:
                recommendations.append("将 Wi-Fi 热点无缝切换到 5GHz / 6GHz 纯正交频段；或调整 2.4GHz 信道避开 1/6/11 蓝牙广播信道。")
        elif conflict_data and conflict_data.get("wifi_band") in ["5GHz", "6GHz"]:
            findings.append({
                "dim": "射频频段隔离状态",
                "level": "INFO",
                "desc": f"当前 Wi-Fi 工作在 {conflict_data.get('wifi_band')} 纯正交频段。",
                "cause": "与 2.4GHz 蓝牙硬件天然物理隔离，免受同频共存干扰影响。"
            })

        if not findings:
            findings.append({
                "dim": "全局链路状态",
                "level": "OPTIMAL",
                "desc": "各网络维度指标均在物理黄金区间内，无内核丢包与射频干扰征兆。",
                "cause": "无线信道质量优良，传输协议栈工作平稳。"
            })
            recommendations.append("保持当前网络拓扑与天线布局，系统处于最佳健康状态。")

        return {
            "severity": severity,
            "overall_score": score,
            "findings_count": len(findings),
            "findings": findings,
            "recommendations": recommendations,
            "knowledge_base_connected": self.available
        }
