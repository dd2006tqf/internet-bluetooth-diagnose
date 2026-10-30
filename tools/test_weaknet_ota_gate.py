#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P0-C1 启动门禁的单元测试（weaknet-ota-gate）

门禁是"业务服务启动前"的最后一道闸。它必须是 fail-closed：
只有"没有事务"和"事务已正常结束"两种情况允许启动。

同时校验一个容易踩的坑：门禁**不读** rescue.flag，只读权威事务状态。
rescue.flag 是派生投影，读它就等于承认两套权威状态。
"""

import importlib.machinery
import importlib.util
import json
import os
import shutil
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
GATE_PATH = os.path.join(HERE, "weaknet-ota-gate")

_loader = importlib.machinery.SourceFileLoader("weaknet_ota_gate", GATE_PATH)
_spec = importlib.util.spec_from_loader("weaknet_ota_gate", _loader)
gate = importlib.util.module_from_spec(_spec)
_loader.exec_module(gate)


def make_state(phase, tx_id="T-1"):
    tx = None
    if phase is not None:
        tx = {
            "transaction_id": tx_id,
            "kind": "upgrade",
            "phase": phase,
            "from_version": "1.0.0",
            "to_version": "1.1.0",
            "rollback_target": "1.0.0",
            "previous_stable_before": None,
            "boot_attempts": 0,
            "last_trial_boot_id": "",
            "policy": {},
            "started_at": "2026-01-01T00:00:00Z",
            "updated_at": "2026-01-01T00:00:00Z",
            "last_error": "",
        }
    return {
        "schema_version": 1,
        "revision": 1,
        "stable_version": "1.0.0",
        "previous_stable_version": None,
        "transaction": tx,
    }


class Sandbox(object):
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="wn-ota-gate-test-")
        self.state_dir = os.path.join(self.tmp, "ota-state")
        os.makedirs(self.state_dir)

    def write_state(self, state):
        with open(os.path.join(self.state_dir, "transaction.json"), "w",
                  encoding="utf-8") as fh:
            json.dump(state, fh)

    def write_raw(self, raw):
        with open(os.path.join(self.state_dir, "transaction.json"), "w",
                  encoding="utf-8") as fh:
            fh.write(raw)

    def write_flag(self, content="rescue"):
        with open(os.path.join(self.state_dir, "rescue.flag"), "w",
                  encoding="utf-8") as fh:
            fh.write(content)

    def has_flag(self):
        return os.path.exists(os.path.join(self.state_dir, "rescue.flag"))

    def cleanup(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


class TestGateDecide(unittest.TestCase):
    """decide() 是纯函数：只有"无事务"和"终态"允许启动。"""

    def setUp(self):
        self.sb = Sandbox()

    def tearDown(self):
        self.sb.cleanup()

    def _decide(self, state, error=None):
        return gate.decide(state, error)

    def test_no_state_file_allows(self):
        allow, reason = self._decide(None, None)
        self.assertTrue(allow)
        self.assertIn("全新设备", reason)

    def test_terminal_phases_allow(self):
        for phase in ("COMMITTED", "FORCED_COMMITTED", "ABORTED", "ROLLED_BACK"):
            allow, _ = self._decide(make_state(phase))
            self.assertTrue(allow, phase)

    def test_recovery_required_denies(self):
        allow, reason = self._decide(make_state("RECOVERY_REQUIRED"))
        self.assertFalse(allow)
        self.assertIn("RECOVERY_REQUIRED", reason)

    def test_live_phases_allow(self):
        """升级/回滚进行中必须放行：健康窗口要靠服务在线才有意义。

        这里曾经写成"live 阶段一律拒绝"，结果板上每次升级都在
        do-switch(TRIAL) → restart 处被挡死，健康窗口必然超时 → 回滚 →
        回滚重启又被挡 → 100% 失败。不允许为了让门禁"看起来更严格"
        再把这条改回去。
        """
        for phase in ("PREPARED", "SWITCH_PENDING", "TRIAL",
                      "ROLLBACK_PENDING", "ROLLBACK_VERIFY"):
            allow, reason = self._decide(make_state(phase))
            self.assertTrue(allow, phase)
            self.assertIn("进行中", reason)

    def test_unknown_phase_denies(self):
        """未登记的阶段不放行：新阶段必须显式登记为终态或活跃态。"""
        allow, reason = self._decide(make_state("SOME_FUTURE_PHASE"))
        self.assertFalse(allow)
        self.assertIn("不认识", reason)

    def test_state_without_transaction_allows(self):
        state = make_state(None)
        allow, reason = self._decide(state)
        self.assertTrue(allow)
        self.assertIn("没有进行中的事务", reason)

    def test_corrupt_state_denies(self):
        allow, reason = self._decide(None, "事务状态文件损坏")
        self.assertFalse(allow)
        self.assertIn("损坏", reason)

    def test_unknown_schema_denies(self):
        allow, reason = self._decide(None, "schema_version=99 不受支持")
        self.assertFalse(allow)
        self.assertIn("不受支持", reason)

    def test_missing_phase_denies(self):
        state = make_state("TRIAL")
        state["transaction"]["phase"] = None
        allow, reason = self._decide(state)
        self.assertFalse(allow)
        self.assertIn("不可信", reason)


class TestGateReadState(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox()

    def tearDown(self):
        self.sb.cleanup()

    def test_missing_file_is_not_error(self):
        state, error = gate.read_state(self.sb.state_dir)
        self.assertIsNone(state)
        self.assertIsNone(error)

    def test_corrupt_json_is_error(self):
        self.sb.write_raw("{ broken")
        state, error = gate.read_state(self.sb.state_dir)
        self.assertIsNone(state)
        self.assertIn("损坏", error)

    def test_unknown_schema_is_error(self):
        self.sb.write_raw(json.dumps({"schema_version": 99}))
        state, error = gate.read_state(self.sb.state_dir)
        self.assertIsNone(state)
        self.assertIn("不受支持", error)

    def test_non_object_is_error(self):
        self.sb.write_raw("[1, 2, 3]")
        state, error = gate.read_state(self.sb.state_dir)
        self.assertIsNone(state)
        self.assertIn("必须是对象", error)

    def test_valid_state_reads(self):
        self.sb.write_state(make_state("TRIAL"))
        state, error = gate.read_state(self.sb.state_dir)
        self.assertIsNone(error)
        self.assertEqual(state["transaction"]["phase"], "TRIAL")


class TestGateIgnoresRescueFlag(unittest.TestCase):
    """门禁**不读** rescue.flag——它是派生投影，不是权威。"""

    def setUp(self):
        self.sb = Sandbox()

    def tearDown(self):
        self.sb.cleanup()

    def test_flag_alone_does_not_deny(self):
        """只有标记、没有救援态事务 → 放行（门禁看权威状态）。"""
        self.sb.write_state(make_state("COMMITTED"))
        self.sb.write_flag("rescue")
        state, error = gate.read_state(self.sb.state_dir)
        allow, _ = gate.decide(state, error)
        self.assertTrue(allow)

    def test_rescue_state_denies_even_without_flag(self):
        """权威状态说救援 → 拒绝，哪怕标记缺失（标记可能写失败）。"""
        self.sb.write_state(make_state("RECOVERY_REQUIRED"))
        state, error = gate.read_state(self.sb.state_dir)
        allow, _ = gate.decide(state, error)
        self.assertFalse(allow)


class TestGateCli(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox()

    def tearDown(self):
        self.sb.cleanup()

    def _run(self):
        return gate.main(["--root", "/tmp/x", "--state-dir", self.sb.state_dir])

    def test_exit_allow_when_no_state(self):
        self.assertEqual(self._run(), gate.EXIT_ALLOW)

    def test_exit_deny_when_recovery_required(self):
        self.sb.write_state(make_state("RECOVERY_REQUIRED"))
        self.assertEqual(self._run(), gate.EXIT_DENY)

    def test_exit_schema_when_corrupt(self):
        """状态读不出来 → 专用退出码（systemd 行为相同：非零即拒绝）。"""
        self.sb.write_raw("{ broken")
        self.assertEqual(self._run(), gate.EXIT_SCHEMA)
        self.assertNotEqual(gate.EXIT_SCHEMA, gate.EXIT_ALLOW)

    def test_exit_schema_when_schema_unknown(self):
        self.sb.write_raw(json.dumps({"schema_version": 99}))
        self.assertEqual(self._run(), gate.EXIT_SCHEMA)

    def test_every_deny_path_is_nonzero(self):
        """拒绝必须让 systemd 看到非零；放行必须是 0。"""
        deny = [
            make_state("RECOVERY_REQUIRED"),
            make_state("SOME_FUTURE_PHASE"),
        ]
        for state in deny:
            self.sb.write_state(state)
            self.assertNotEqual(self._run(), gate.EXIT_ALLOW, state)
        self.sb.write_raw("{ broken")
        self.assertNotEqual(self._run(), gate.EXIT_ALLOW)

        allow = [make_state("TRIAL"), make_state("ROLLBACK_VERIFY"),
                 make_state("COMMITTED")]
        for state in allow:
            self.sb.write_state(state)
            self.assertEqual(self._run(), gate.EXIT_ALLOW, state)

    def test_exit_allow_when_terminal(self):
        self.sb.write_state(make_state("COMMITTED"))
        self.assertEqual(self._run(), gate.EXIT_ALLOW)


class TestServiceWiring(unittest.TestCase):
    """systemd 接线是门禁的另一半——接错了门禁再对也拦不住。

    这些断言挡的是"把 Requires 简化成 Wants"这类看似无害、实测致命的改动：
    板端测过 Wants + After 在依赖失败时**照样启动**业务服务。
    """

    def _unit(self, name):
        with open(os.path.join(HERE, name), "r", encoding="utf-8") as fh:
            return fh.read()

    def _directives(self, text, key):
        return [ln.split("=", 1)[1].strip()
                for ln in text.splitlines()
                if ln.split("=", 1)[0].strip() == key]

    def test_server_requires_gate(self):
        text = self._unit("weaknet-server.service")
        self.assertIn("weaknet-ota-gate.service", self._directives(text, "Requires"))
        # Wants 不是门禁：依赖失败时业务服务照样启动（板端实测 ON-B-RAN）
        self.assertFalse(any("weaknet-ota-gate" in d
                             for d in self._directives(text, "Wants")))

    def test_server_orders_after_gate(self):
        text = self._unit("weaknet-server.service")
        after = " ".join(self._directives(text, "After"))
        self.assertIn("weaknet-ota-gate.service", after)

    def test_server_condition_negates_rescue_flag(self):
        text = self._unit("weaknet-server.service")
        conds = self._directives(text, "ConditionPathExists")
        self.assertTrue(any(c.startswith("!") and c.endswith("rescue.flag")
                            for c in conds), conds)

    def test_gate_is_oneshot_without_remain_after_exit(self):
        """没有 RemainAfterExit 才会在每次业务启动（含崩溃重启）时重跑。"""
        text = self._unit("weaknet-ota-gate.service")
        self.assertIn("Type=oneshot", text)
        self.assertNotIn("RemainAfterExit=yes", text)

    def test_gate_does_not_depend_on_business_service(self):
        """门禁不能依赖 weaknet-server——那可能正是起不来的那个。"""
        text = self._unit("weaknet-ota-gate.service")
        for key in ("Requires", "After", "Wants", "BindsTo"):
            for dep in self._directives(text, key):
                self.assertNotIn("weaknet-server", dep)

    def test_gate_unit_has_no_function_condition(self):
        """门禁单元自身不能靠 Condition 跳过：被跳过的单元对 Requires
        而言不算失败，业务服务照样起（板端实测 CONSUMER-RAN）。"""
        text = self._unit("weaknet-ota-gate.service")
        for key in ("ConditionPathExists", "ConditionFileNotEmpty"):
            self.assertEqual(self._directives(text, key), [], key)


if __name__ == "__main__":
    unittest.main()
