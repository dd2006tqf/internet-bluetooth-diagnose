#!/usr/bin/env python3

# 模块职责：本文件实现文件名所对应的日志、数据处理或网络诊断功能。
# 维护提示：扩展公共函数或命令行入口时，应说明输入、输出、异常、外部依赖和降级路径，
# 并保持既有 CLI/API 行为不变。

"""
蓝牙 RAG 诊断查询测试

覆盖 OpenSpec change `bt-a2dp-ebpf-fusion` 的 RAG Diagnostics 需求：
- Scenario: 频段冲突查询  (输入"蓝牙卡顿" → 频段冲突建议)
- Scenario: 音频延迟查询  (输入"蓝牙音频延迟高" → 编解码器/距离/卡顿建议)

纯本地知识库匹配，不依赖外部 LLM API，可纳入 CI。
"""

import os
import sys

# 将知识库所在目录加入 sys.path，便于 `python -m pytest` 直接运行
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from network_knowledge_base import query_bluetooth_diagnosis  # noqa: E402


# 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
def test_band_conflict_query_returns_wifi_channel_suggestion():
    """Scenario: 频段冲突查询

    输入"蓝牙卡顿"时，RAG 必须返回频段冲突相关建议，
    至少命中以下处置关键词之一：5GHz / 信道 / 靠近。
    """
    result = query_bluetooth_diagnosis("蓝牙卡顿")
    assert isinstance(result, str), "query_bluetooth_diagnosis 必须返回字符串"
    assert result, "查询结果不能为空"
    keywords = ["5GHz", "信道", "靠近"]
    assert any(k in result for k in keywords), (
        f"频段冲突查询应返回 Wi-Fi 切 5GHz / 调信道 / 设备靠近 建议，实际: {result!r}"
    )


# 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
def test_audio_latency_query_returns_codec_distance_suggestion():
    """Scenario: 音频延迟查询

    输入"蓝牙音频延迟高"时，RAG 必须返回编解码器/距离/卡顿相关建议，
    至少命中以下关键词之一：编解码器 / 距离 / 卡顿。
    """
    result = query_bluetooth_diagnosis("蓝牙音频延迟高")
    assert isinstance(result, str), "query_bluetooth_diagnosis 必须返回字符串"
    assert result, "查询结果不能为空"
    keywords = ["编解码器", "距离", "卡顿"]
    assert any(k in result for k in keywords), (
        f"音频延迟查询应返回编解码器/距离/卡顿 建议，实际: {result!r}"
    )


# 函数说明：封装一个可复用的处理步骤；请以函数签名和调用方确定输入、输出及异常语义。
def test_band_conflict_diagnosis_entry_exists_in_knowledge_base():
    """知识库必须包含蓝牙诊断条目（频段冲突/音频卡顿/距离估算）。"""
    from network_knowledge_base import get_network_knowledge

    kb = get_network_knowledge()
    assert "bluetooth_analysis" in kb, "知识库缺少 bluetooth_analysis 条目"
    bt = kb["bluetooth_analysis"]
    assert isinstance(bt, dict)
    # 必须覆盖三类诊断子条目
    for key in ("band_conflict", "audio_stall", "distance_estimation"):
        assert key in bt, f"bluetooth_analysis 缺少 {key} 子条目"
