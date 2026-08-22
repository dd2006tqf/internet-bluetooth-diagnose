#!/usr/bin/env python3
"""
网络分析知识库
包含网络监控指标的含义、正常范围、异常情况分析等知识
"""

NETWORK_KNOWLEDGE_BASE = {
    "rtt_analysis": {
        "description": "RTT (Round Trip Time) 往返时延分析",
        "normal_range": "1-50ms",
        "excellent": "1-10ms",
        "good": "10-30ms", 
        "fair": "30-50ms",
        "poor": "50-100ms",
        "critical": ">100ms",
        "symptoms": {
            "high_rtt": "RTT过高会导致网络延迟增加，影响实时应用性能",
            "rtt_timeout": "RTT超时(-1ms)表示网络连接失败或目标不可达",
            "rtt_fluctuation": "RTT波动大表示网络不稳定"
        },
        "troubleshooting": {
            "high_rtt": "检查网络拥塞、路由问题、DNS解析延迟",
            "timeout": "检查网络连接、防火墙设置、目标服务器状态",
            "fluctuation": "检查网络质量、干扰源、负载均衡配置"
        }
    },
    
    "tcp_loss_analysis": {
        "description": "TCP丢包率分析",
        "normal_range": "0-1%",
        "excellent": "0%",
        "good": "0-0.5%",
        "fair": "0.5-1%",
        "poor": "1-3%",
        "critical": ">3%",
        "symptoms": {
            "high_loss": "丢包率高会导致数据传输重传，降低网络效率",
            "burst_loss": "突发丢包可能表示网络拥塞或设备故障",
            "persistent_loss": "持续丢包可能表示链路质量问题"
        },
        "troubleshooting": {
            "high_loss": "检查网络设备、链路质量、拥塞控制",
            "burst_loss": "检查网络设备状态、流量突发、QoS配置",
            "persistent_loss": "检查物理链路、设备端口、网络配置"
        }
    },
    
    "traffic_analysis": {
        "description": "网络流量分析",
        "normal_indicators": {
            "bandwidth_utilization": "正常利用率应<80%",
            "flow_count": "活跃连接数应合理",
            "packet_rate": "包速率应稳定"
        },
        "symptoms": {
            "zero_traffic": "流量为0可能表示网络中断或监控问题",
            "high_utilization": "高利用率可能导致网络拥塞",
            "abnormal_flows": "异常连接数可能表示攻击或配置问题"
        },
        "troubleshooting": {
            "zero_traffic": "检查网络连接、eBPF程序、监控配置",
            "high_utilization": "检查带宽限制、流量控制、网络规划",
            "abnormal_flows": "检查安全策略、连接限制、异常检测"
        }
    },
    
    "rssi_analysis": {
        "description": "WiFi信号强度分析",
        "signal_levels": {
            "excellent": "-30 to -50 dBm",
            "good": "-50 to -60 dBm",
            "fair": "-60 to -70 dBm",
            "poor": "-70 to -80 dBm",
            "critical": "< -80 dBm"
        },
        "symptoms": {
            "low_rssi": "信号弱会导致连接不稳定、速度慢",
            "rssi_fluctuation": "信号波动表示环境干扰或设备问题",
            "no_signal": "无信号表示WiFi未连接或设备故障"
        },
        "troubleshooting": {
            "low_rssi": "检查距离、障碍物、天线方向、功率设置",
            "fluctuation": "检查干扰源、设备稳定性、环境变化",
            "no_signal": "检查WiFi配置、设备状态、网络连接"
        }
    },
    
    "interface_analysis": {
        "description": "网络接口状态分析",
        "states": {
            "up": "接口正常，可以传输数据",
            "down": "接口关闭，无法传输数据",
            "unknown": "接口状态未知"
        },
        "symptoms": {
            "interface_down": "接口关闭会导致网络中断",
            "no_active_interface": "无活跃接口表示网络完全中断",
            "interface_changes": "接口频繁变化表示网络不稳定"
        },
        "troubleshooting": {
            "interface_down": "检查物理连接、驱动状态、配置错误",
            "no_active_interface": "检查网络配置、路由设置、服务状态",
            "interface_changes": "检查网络稳定性、配置冲突、设备故障"
        }
    },
    
    "quality_assessment": {
        "description": "网络质量综合评估",
        "quality_levels": {
            "excellent": "所有指标优秀，网络性能最佳",
            "good": "大部分指标良好，网络性能良好",
            "fair": "部分指标一般，网络性能可接受",
            "poor": "多个指标较差，网络性能不佳",
            "critical": "关键指标严重，网络性能极差"
        },
        "assessment_factors": [
            "RTT延迟",
            "TCP丢包率", 
            "流量稳定性",
            "信号强度",
            "接口状态"
        ]
    },
    
    "bluetooth_analysis": {
        "description": "蓝牙监控诊断分析（频段冲突 / A2DP 音频卡顿 / 设备距离估算）",
        "band_conflict": {
            "description": "2.4GHz 频段冲突：Wi-Fi 与蓝牙同频段导致蓝牙卡顿",
            "symptoms": ["蓝牙卡顿", "WiFi 与蓝牙同时弱信号", "频段冲突"],
            "suggestions": [
                "Wi-Fi 切 5GHz 频段",
                "调整 Wi-Fi 信道避开蓝牙广播信道",
                "设备靠近蓝牙适配器",
                "降低 Wi-Fi 发射功率"
            ]
        },
        "audio_stall": {
            "description": "A2DP 音频卡顿/延迟：编解码器/距离/停滞导致蓝牙音频延迟高",
            "symptoms": ["蓝牙音频延迟高", "音频卡顿", "疑似停滞"],
            "suggestions": [
                "更换编解码器（SBC→AAC/LDAC）",
                "缩短设备距离以提升 RSSI",
                "检查音频卡顿源（eBPF 流量停滞 vs D-Bus 状态不同步）",
                "校准 txPower 提升距离估算精度"
            ]
        },
        "distance_estimation": {
            "description": "设备距离估算：基于 RSSI 对数路径损耗模型",
            "symptoms": ["信号弱", "距离过远", "RSSI 过低"],
            "suggestions": [
                "设备靠近适配器",
                "校准 txPower（默认 -59dBm）",
                "减少物理障碍物",
                "检查 RSSI 是否为 0（降级标记）"
            ]
        }
    },

    "common_issues": {
        "network_congestion": {
            "symptoms": ["高RTT", "高丢包率", "流量异常"],
            "causes": ["带宽不足", "设备过载", "配置问题"],
            "solutions": ["增加带宽", "优化配置", "负载均衡"]
        },
        "hardware_failure": {
            "symptoms": ["接口down", "信号丢失", "流量中断"],
            "causes": ["设备故障", "线缆问题", "端口损坏"],
            "solutions": ["更换设备", "检查线缆", "更换端口"]
        },
        "configuration_error": {
            "symptoms": ["连接失败", "性能异常", "状态错误"],
            "causes": ["配置错误", "参数不当", "策略冲突"],
            "solutions": ["检查配置", "修正参数", "解决冲突"]
        },
        "environmental_interference": {
            "symptoms": ["信号波动", "连接不稳定", "性能下降"],
            "causes": ["电磁干扰", "物理障碍", "距离过远"],
            "solutions": ["消除干扰", "调整位置", "增强信号"]
        }
    }
}

def get_network_knowledge():
    """获取网络分析知识库"""
    return NETWORK_KNOWLEDGE_BASE

def analyze_metric(metric_type, value, context=""):
    """分析特定网络指标"""
    knowledge = NETWORK_KNOWLEDGE_BASE.get(metric_type, {})

    if not knowledge:
        return f"未知指标类型: {metric_type}"

    analysis = {
        "metric": metric_type,
        "value": value,
        "context": context,
        "analysis": knowledge
    }

    return analysis


def query_bluetooth_diagnosis(query: str) -> str:
    """基于自然语言关键词查询蓝牙诊断建议。

    覆盖 OpenSpec `bt-a2dp-ebpf-fusion` 的 RAG Diagnostics 需求：
      - "蓝牙卡顿"        → 频段冲突建议（5GHz / 信道 / 靠近）
      - "蓝牙音频延迟高"   → 编解码器 / 距离 / 卡顿建议

    匹配策略：遍历 bluetooth_analysis 子条目，命中 symptom 关键词即返回其建议；
    若无精确命中，返回全部子条目建议作为兜底（避免空响应）。
    """
    bt = NETWORK_KNOWLEDGE_BASE.get("bluetooth_analysis", {})
    if not bt:
        return ""

    matched_entries = []
    for key in ("band_conflict", "audio_stall", "distance_estimation"):
        entry = bt.get(key, {})
        symptoms = entry.get("symptoms", [])
        # symptom 字符串包含匹配（中文不分词）
        if any(sym in query for sym in symptoms):
            matched_entries.append(entry)

    if not matched_entries:
        # 兜底：无精确命中时返回全部子条目建议
        matched_entries = [
            bt.get(key, {}) for key in ("band_conflict", "audio_stall", "distance_estimation")
        ]

    parts = []
    for entry in matched_entries:
        desc = entry.get("description", "")
        suggestions = entry.get("suggestions", [])
        if suggestions:
            parts.append(f"{desc}：{', '.join(suggestions)}")

    return "; ".join(parts)
