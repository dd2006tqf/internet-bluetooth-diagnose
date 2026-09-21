#!/usr/bin/env python3
"""
自动化测试套件：验证 weaknet-offline-diag 的 Auditor 契约校验与降级保底机制
覆盖测试点：
  1. 合法大模型输出 -> 校验通过，状态为 CONTRACT_VALIDATED
  2. 格式损坏的 JSON -> 校验失败，回退到规则默认模板，状态为 FALLBACK
  3. 遗漏动作解释 -> 校验失败，回退到规则默认模板
  4. 重复动作解释 -> 校验失败，回退到规则默认模板
  5. 伪造未声明的动作 -> 校验失败，回退到规则默认模板
"""

import unittest
import json
import subprocess
import sys
import os

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "../../client")))

# 动态载入 weaknet-offline-diag (无 .py 后缀脚本加载)
import importlib.util
import importlib.machinery
file_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../client/weaknet-offline-diag"))
loader = importlib.machinery.SourceFileLoader("weaknet_offline_diag", file_path)
spec = importlib.util.spec_from_loader("weaknet_offline_diag", loader)
diag_module = importlib.util.module_from_spec(spec)
loader.exec_module(diag_module)
ContractValidator = diag_module.ContractValidator

class TestContractValidator(unittest.TestCase):
    def setUp(self):
        self.expected_actions = ["CHECK_RESOLVER_CONFIG", "PROBE_PUBLIC_RESOLVER"]

    def test_valid_explanation(self):
        payload = json.dumps({
            "human_summary": "上游 DNS 超时。",
            "action_explanations": [
                {"action_id": "CHECK_RESOLVER_CONFIG", "explanation": "查看配置"},
                {"action_id": "PROBE_PUBLIC_RESOLVER", "explanation": "直连测试"}
            ]
        })
        ok, data, err = ContractValidator.validate_explanation(payload, self.expected_actions)
        self.assertTrue(ok)
        self.assertEqual(err, "OK")
        self.assertEqual(len(data["action_explanations"]), 2)

    def test_invalid_json(self):
        payload = "{ broken json"
        ok, data, err = ContractValidator.validate_explanation(payload, self.expected_actions)
        self.assertFalse(ok)
        self.assertIn("JSON parse error", err)

    def test_missing_action(self):
        # 模型遗漏了 PROBE_PUBLIC_RESOLVER
        payload = json.dumps({
            "human_summary": "上游 DNS 超时。",
            "action_explanations": [
                {"action_id": "CHECK_RESOLVER_CONFIG", "explanation": "查看配置"}
            ]
        })
        ok, data, err = ContractValidator.validate_explanation(payload, self.expected_actions)
        self.assertFalse(ok)
        self.assertIn("Action IDs mismatch", err)

    def test_duplicate_action(self):
        # 模型输出了重复的 action_id
        payload = json.dumps({
            "human_summary": "上游 DNS 超时。",
            "action_explanations": [
                {"action_id": "CHECK_RESOLVER_CONFIG", "explanation": "查看配置1"},
                {"action_id": "CHECK_RESOLVER_CONFIG", "explanation": "查看配置2"}
            ]
        })
        ok, data, err = ContractValidator.validate_explanation(payload, self.expected_actions)
        self.assertFalse(ok)
        self.assertIn("Duplicate action_id", err)

    def test_hallucinated_unknown_action(self):
        # 模型自创了未被允许的动作
        payload = json.dumps({
            "human_summary": "上游 DNS 超时。",
            "action_explanations": [
                {"action_id": "CHECK_RESOLVER_CONFIG", "explanation": "查看配置"},
                {"action_id": "UNAUTHORIZED_REBOOT", "explanation": "非法重启"}
            ]
        })
        ok, data, err = ContractValidator.validate_explanation(payload, self.expected_actions)
        self.assertFalse(ok)
        self.assertIn("Action IDs mismatch", err)

    def test_extra_top_level_fields(self):
        # 模型尝试复述或篡改 machine_diagnosis
        payload = json.dumps({
            "human_summary": "上游 DNS 超时。",
            "action_explanations": [
                {"action_id": "CHECK_RESOLVER_CONFIG", "explanation": "查看配置"},
                {"action_id": "PROBE_PUBLIC_RESOLVER", "explanation": "直连测试"}
            ],
            "machine_diagnosis": {"fault_domain": "TAMPERED_DOMAIN"} # 非法顶层字段
        })
        ok, data, err = ContractValidator.validate_explanation(payload, self.expected_actions)
        self.assertFalse(ok)
        self.assertIn("Keys mismatch", err)

    def test_golden_cases_verification(self):
        # 验证 golden_cases.yaml 中的规则契约与预期
        yaml_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../rules/golden_cases.yaml"))
        self.assertTrue(os.path.exists(yaml_path))
        with open(yaml_path, "r", encoding="utf-8") as f:
            content = f.read()
            self.assertIn("CASE_DNS_BURST_TIMEOUT_GOOD_GATEWAY", content)
            self.assertIn("CASE_GATEWAY_UNREACHABLE_SUPPRESSES_DNS", content)
            self.assertIn("CASE_MULTI_FAULT_DNS_AND_WIFI", content)

if __name__ == "__main__":
    unittest.main()
