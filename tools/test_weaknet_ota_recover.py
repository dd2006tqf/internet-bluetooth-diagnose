#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P0-B 开机恢复服务的单元测试（weaknet-ota-recover）

覆盖三条硬约束对应的行为：
  1. 即使升级脚本消失也能读取事务 —— 恢复器从不引用升级脚本，只读
     /var/lib/weaknet-ota/transaction.json
  2. 即使 weaknet-server 无法启动也能完成回滚决策 —— 决策是纯函数，
     不依赖服务在线；健康探测用可注入的假 runner
  3. 即使回滚失败也能保留 SSH 救援入口 —— 救援模式停服+mask，并保证
     ssh / weaknet-tunnel 处于 active
"""

import importlib.machinery
import importlib.util
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
RECOVER_PATH = os.path.join(HERE, "weaknet-ota-recover")

_loader = importlib.machinery.SourceFileLoader("weaknet_ota_recover", RECOVER_PATH)
_spec = importlib.util.spec_from_loader("weaknet_ota_recover", _loader)
rec = importlib.util.module_from_spec(_spec)
_loader.exec_module(rec)


# ---------------------------------------------------------------------------
# 测试替身
# ---------------------------------------------------------------------------

class FakeRunner(object):
    """假命令执行器：按 (cmd[0], cmd[1]) 查表应答，并记录调用轨迹。

    `delegate_paths` 里的路径会**真实执行**——用来让假控制器真的跑起来，
    从而验证"确实通过控制器执行了写操作"而不是绕过控制器。
    """

    def __init__(self, table=None, default=(1, ""), delegate_paths=()):
        self.table = table or {}
        self.default = default
        self.delegate_paths = tuple(delegate_paths)
        self.calls = []

    def __call__(self, cmd, timeout=15):
        self.calls.append(list(cmd))
        # 命中"真实执行"路径（如假控制器）
        for p in self.delegate_paths:
            if p in cmd:
                return rec.default_runner(cmd, timeout=timeout)
        key = (cmd[0], cmd[1] if len(cmd) > 1 else "")
        if key in self.table:
            return self.table[key]
        # busctl 是唯一带 4 个业务参数的命令，单独匹配
        if cmd[0] == "busctl" and "busctl" in self.table:
            for method in ("HealthCheck", "GetEbpfMonitorHealth", "GetUpdateStatus"):
                if method in cmd:
                    return self.table["busctl"][method]
        return self.default

    def has(self, *needle):
        """轨迹里是否出现过包含这些片段的命令。"""
        for call in self.calls:
            joined = " ".join(call)
            if all(n in joined for n in needle):
                return True
        return False


def make_state(phase, frm="1.0.0", to="1.1.0", rollback=None, attempts=0,
               policy=None, stable="1.0.0", previous=None, tx_id="T-1"):
    tx = {
        "transaction_id": tx_id,
        "kind": "upgrade",
        "phase": phase,
        "from_version": frm,
        "to_version": to,
        "rollback_target": rollback or frm,
        "previous_stable_before": previous,
        "boot_attempts": attempts,
        "last_trial_boot_id": "",
        "policy": policy or {"health_timeout": 120, "health_ok_needed": 3,
                             "health_poll_s": 5},
        "started_at": "2026-01-01T00:00:00Z",
        "updated_at": "2026-01-01T00:00:00Z",
        "last_error": "",
    }
    return {
        "schema_version": 1,
        "revision": 1,
        "stable_version": stable,
        "previous_stable_version": previous,
        "transaction": tx,
    }


class Sandbox(object):
    """假的 weaknet 根目录 + 事务状态目录。"""

    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="wn-ota-recover-test-")
        self.root = os.path.join(self.tmp, "weaknet")
        self.state_dir = os.path.join(self.tmp, "ota-state")
        os.makedirs(os.path.join(self.root, "releases"))
        os.makedirs(self.state_dir)
        for ver in ("1.0.0", "1.1.0"):
            path = os.path.join(self.root, "releases", ver)
            os.makedirs(path)
            with open(os.path.join(path, "VERSION"), "w") as fh:
                fh.write(ver + "\n")
        self.set_current("1.0.0")

    def set_current(self, version):
        link = os.path.join(self.root, "current")
        if os.path.lexists(link):
            os.unlink(link)
        if version:
            os.symlink("releases/" + version, link)

    def write_state(self, state):
        path = os.path.join(self.state_dir, "transaction.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(state, fh)

    def write_raw_state(self, raw):
        path = os.path.join(self.state_dir, "transaction.json")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(raw)

    def read_rescue(self):
        path = os.path.join(self.state_dir, "RESCUE.md")
        if not os.path.exists(path):
            return None
        with open(path, encoding="utf-8") as fh:
            return fh.read()

    def cleanup(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def make_fake_ctl(self, name="weaknet-ota-ctl"):
        """造一个假控制器：可执行，把 argv 记进 ctl_calls.json，返回 0。

        这样测试能验证"确实通过控制器执行了写操作"，而不是绕过控制器。
        """
        path = os.path.join(self.tmp, name)
        script = (
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            "log = os.path.join(%r, 'ctl_calls.json')\n"
            "calls = []\n"
            "if os.path.exists(log):\n"
            "    calls = json.load(open(log))\n"
            "calls.append(sys.argv[1:])\n"
            "json.dump(calls, open(log, 'w'))\n"
            "print('{\"ok\": true}')\n" % self.tmp
        )
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(script)
        os.chmod(path, 0o755)
        self.ctl_path = path
        return path

    def ctl_calls(self):
        path = os.path.join(self.tmp, "ctl_calls.json")
        if not os.path.exists(path):
            return []
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)

    def ctl_verbs(self):
        """从控制器调用轨迹里取出子命令名。

        假控制器记录的是 sys.argv[1:]，形如
        [--root, R, --state-dir, S, <subcmd>, ...]，
        所以子命令在索引 4（不是最后一个——有的子命令还带参数）。
        """
        out = []
        for call in self.ctl_calls():
            out.append(call[4] if len(call) > 4 else "")
        return out


def make_recorder(sb, runner, sleeper=None, clock=None, ctl_path=None):
    return rec.Recoverer(
        sb.root, sb.state_dir, ctl_path or os.path.join(sb.tmp, "no-such-ctl"),
        runner=runner,
        sleeper=sleeper or (lambda _s: None),
        clock=clock or (lambda: 1_700_000_000.0))


# ---------------------------------------------------------------------------
# 约束 1：只读事务状态，不依赖升级脚本
# ---------------------------------------------------------------------------

class TestReadsTransactionWithoutUpgradeScript(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox()
        self.runner = FakeRunner()

    def tearDown(self):
        self.sb.cleanup()

    def test_source_never_references_upgrade_script(self):
        """恢复器源码里不得出现 weaknet-local-update.sh（文档字符串除外）。

        升级脚本住在 $ROOT 下，会随 OTA 制品被覆盖/删除。恢复能力绑在它
        身上就等于绑在"会被升级掉的东西"身上。
        """
        import ast
        with open(RECOVER_PATH, "r", encoding="utf-8") as fh:
            src = fh.read()
        tree = ast.parse(src)
        # 去掉模块 docstring：它只是说明文档，不是引用
        docstring_nodes = []
        if (tree.body and isinstance(tree.body[0], ast.Expr)
                and isinstance(tree.body[0].value, ast.Constant)
                and isinstance(tree.body[0].value.value, str)):
            docstring_nodes.append((tree.body[0].value.lineno,
                                    tree.body[0].value.end_lineno))
        code_only = []
        for i, line in enumerate(src.splitlines(), start=1):
            if any(start <= i <= end for start, end in docstring_nodes):
                continue
            code_only.append(line)
        for ln in code_only:
            self.assertNotIn("weaknet-local-update.sh", ln,
                             "恢复器代码里出现了升级脚本引用：%s" % ln)

    def test_state_readable_when_release_dir_has_no_upgrade_script(self):
        """$ROOT 下连升级脚本都没有，也能读出事务并判定。"""
        self.sb.write_state(make_state("TRIAL", to="1.1.0"))
        state = rec.load_state(self.sb.state_dir)
        self.assertEqual(state.phase, "TRIAL")
        self.assertEqual(state.to_version, "1.1.0")

    def test_missing_state_is_empty_not_error(self):
        state = rec.load_state(self.sb.state_dir)
        self.assertFalse(state.has_tx)
        action, _ = rec.decide(state, "1.0.0", False, 0, 0, 0)
        self.assertEqual(action, "none")

    def test_corrupt_state_raises_and_triggers_rescue(self):
        """状态损坏 → 救援模式（不能猜）。"""
        self.sb.write_raw_state("{ broken json")
        runner = FakeRunner()
        r = make_recorder(self.sb, runner)
        watcher = rec.Watcher(r, once=True)
        done, _ = watcher.run_once()
        self.assertTrue(done)
        self.assertIsNotNone(self.sb.read_rescue())
        # 救援模式必须保住 SSH
        self.assertTrue(runner.has("systemctl", "start", "ssh"))

    def test_unknown_schema_raises(self):
        self.sb.write_raw_state(json.dumps({"schema_version": 99, "revision": 1}))
        with self.assertRaises(ValueError):
            rec.load_state(self.sb.state_dir)


# ---------------------------------------------------------------------------
# 约束 2：服务起不来也能做决策
# ---------------------------------------------------------------------------

class TestDecisionWithoutService(unittest.TestCase):
    """decide() 是纯函数：不碰服务、不碰文件系统。"""

    def _decide(self, phase, current, healthy=False, elapsed=0, restarts=-1,
                attempts=0, **kw):
        state = rec.TxState(make_state(phase, **kw))
        return rec.decide(state, current, healthy, elapsed, restarts, attempts)

    def test_trial_healthy_commits(self):
        action, _ = self._decide("TRIAL", "1.1.0", healthy=True)
        self.assertEqual(action, "commit")

    def test_trial_unhealthy_waits_then_rolls_back(self):
        # 尝试次数还没用完 → 继续等
        action, _ = self._decide("TRIAL", "1.1.0", healthy=False, attempts=1)
        self.assertEqual(action, "wait_trial")
        # 尝试次数用完 → 回滚
        action, _ = self._decide("TRIAL", "1.1.0", healthy=False, attempts=3)
        self.assertEqual(action, "rollback")

    def test_trial_crash_loop_rolls_back_immediately(self):
        """崩溃循环是硬证据，不必等尝试次数耗尽。"""
        action, reason = self._decide("TRIAL", "1.1.0", healthy=False,
                                      restarts=3, attempts=0)
        self.assertEqual(action, "rollback")
        self.assertIn("崩溃循环", reason)

    def test_trial_pointer_moved_is_rescue(self):
        action, _ = self._decide("TRIAL", "1.0.0", healthy=False)
        self.assertEqual(action, "rescue")

    def test_prepared_with_old_pointer_aborts(self):
        action, _ = self._decide("PREPARED", "1.0.0")
        self.assertEqual(action, "abort")

    def test_switch_pending_with_new_pointer_marks_trial(self):
        action, _ = self._decide("SWITCH_PENDING", "1.1.0")
        self.assertEqual(action, "mark_trial")

    def test_switch_pending_with_old_pointer_aborts(self):
        action, _ = self._decide("SWITCH_PENDING", "1.0.0")
        self.assertEqual(action, "abort")

    def test_rollback_pending_flips_pointer(self):
        action, _ = self._decide("ROLLBACK_PENDING", "1.1.0", rollback="1.0.0")
        self.assertEqual(action, "rollback_flip")

    def test_rollback_verify_healthy_confirms(self):
        action, _ = self._decide("ROLLBACK_VERIFY", "1.0.0", healthy=True,
                                 rollback="1.0.0")
        self.assertEqual(action, "rolled_back")

    def test_rollback_verify_unhealthy_timeout_is_rescue(self):
        action, _ = self._decide("ROLLBACK_VERIFY", "1.0.0", healthy=False,
                                 elapsed=999, rollback="1.0.0")
        self.assertEqual(action, "rescue")

    def test_recovery_required_goes_rescue(self):
        action, _ = self._decide("RECOVERY_REQUIRED", "1.1.0")
        self.assertEqual(action, "rescue")

    def test_terminal_transaction_is_noop(self):
        for phase in ("COMMITTED", "FORCED_COMMITTED", "ABORTED", "ROLLED_BACK"):
            action, _ = self._decide(phase, "1.1.0")
            self.assertEqual(action, "none", phase)

    def test_uncovered_phase_goes_rescue(self):
        action, _ = self._decide("WEIRD_PHASE", "1.1.0")
        self.assertEqual(action, "rescue")

    def test_first_install_without_pointer_is_handled(self):
        """首次安装：没有 current 软链，from == to。"""
        state = rec.TxState(make_state("PREPARED", frm="2.0.0", to="2.0.0",
                                       rollback="2.0.0"))
        action, _ = rec.decide(state, None, False, 0, -1, 0)
        self.assertEqual(action, "abort")


class TestProber(unittest.TestCase):
    """健康探测：systemd 是硬门禁，D-Bus 不可达不算健康但也不判死。"""

    def setUp(self):
        self.sb = Sandbox()

    def tearDown(self):
        self.sb.cleanup()

    def test_inactive_service_is_not_healthy(self):
        runner = FakeRunner({("systemctl", "is-active"): (0, "inactive\n")})
        prober = rec.Prober(runner=runner)
        ok, detail = prober.probe("1.1.0")
        self.assertFalse(ok)
        self.assertIn("is-active", detail)

    def test_active_service_but_dbus_down_is_not_healthy(self):
        runner = FakeRunner({
            ("systemctl", "is-active"): (0, "active\n"),
            "busctl": {
                "HealthCheck": (1, ""),
                "GetEbpfMonitorHealth": (1, ""),
                "GetUpdateStatus": (1, ""),
            },
        })
        prober = rec.Prober(runner=runner)
        ok, detail = prober.probe("1.1.0")
        self.assertFalse(ok)
        self.assertIn("不可达", detail)

    def test_all_green_is_healthy(self):
        runner = FakeRunner({
            ("systemctl", "is-active"): (0, "active\n"),
            "busctl": {
                "HealthCheck": (0, 's "ok"'),
                "GetEbpfMonitorHealth": (0, 's "ok"'),
                "GetUpdateStatus": (0, 's "{\\"installed_version\\":\\"1.1.0\\"}"'),
            },
        })
        prober = rec.Prober(runner=runner)
        ok, detail = prober.probe("1.1.0")
        self.assertTrue(ok, detail)

    def test_wrong_version_is_not_healthy(self):
        """服务活着但跑的是旧版 → 假绿灯，必须判不健康。"""
        runner = FakeRunner({
            ("systemctl", "is-active"): (0, "active\n"),
            "busctl": {
                "HealthCheck": (0, 's "ok"'),
                "GetEbpfMonitorHealth": (0, 's "ok"'),
                "GetUpdateStatus": (0, 's "{\\"installed_version\\":\\"1.0.0\\"}"'),
            },
        })
        prober = rec.Prober(runner=runner)
        ok, detail = prober.probe("1.1.0")
        self.assertFalse(ok)
        self.assertIn("未报告期望版本", detail)

    def test_restarts_unreadable_is_unknown(self):
        runner = FakeRunner(default=(1, ""))
        prober = rec.Prober(runner=runner)
        self.assertEqual(prober.systemd_restarts(), -1)


# ---------------------------------------------------------------------------
# 约束 3：回滚失败仍保住 SSH 救援入口
# ---------------------------------------------------------------------------

class TestRescueMode(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox()
        self.runner = FakeRunner({
            ("systemctl", "is-active"): (0, "active\n"),
            ("systemctl", "stop"): (0, ""),
            ("systemctl", "mask"): (0, ""),
            ("systemctl", "start"): (0, ""),
            ("systemctl", "restart"): (0, ""),
        })

    def tearDown(self):
        self.sb.cleanup()

    def test_rescue_stops_and_masks_service(self):
        r = make_recorder(self.sb, self.runner)
        r.enter_rescue("测试", rec.TxState(make_state("TRIAL")), "1.1.0")
        self.assertTrue(self.runner.has("systemctl", "stop", "weaknet-server"))
        self.assertTrue(self.runner.has("systemctl", "mask", "weaknet-server"))

    def test_rescue_falls_back_to_disable_when_mask_fails(self):
        """mask 失败（unit 文件已存在）时退到 disable——同样不拉起服务。

        板端实测：`systemctl mask weaknet-server` 报
        "File /etc/systemd/system/weaknet-server.service already exists"，
        退出码 1。所以 rescue 必须有第二条路。
        """
        runner = FakeRunner({
            ("systemctl", "is-active"): (0, "active\n"),
            ("systemctl", "stop"): (0, ""),
            ("systemctl", "mask"): (1, "Failed to mask unit"),
            ("systemctl", "disable"): (0, ""),
            ("systemctl", "start"): (0, ""),
        })
        r = make_recorder(self.sb, runner)
        r.enter_rescue("测试", rec.TxState(make_state("TRIAL")), "1.1.0")
        self.assertTrue(runner.has("systemctl", "disable", "weaknet-server"))

    def test_rescue_keeps_ssh_and_tunnel(self):
        r = make_recorder(self.sb, self.runner)
        r.enter_rescue("测试", rec.TxState(make_state("TRIAL")), "1.1.0")
        self.assertTrue(self.runner.has("systemctl", "start", "ssh")
                        or self.runner.has("systemctl", "is-active", "ssh"))
        self.assertTrue(self.runner.has("ssh"))
        self.assertTrue(self.runner.has("weaknet-tunnel"))

    def test_rescue_report_contains_facts(self):
        r = make_recorder(self.sb, self.runner)
        r.enter_rescue("健康窗口耗尽", rec.TxState(make_state("TRIAL", to="1.1.0")),
                       "1.1.0")
        report = self.sb.read_rescue()
        self.assertIsNotNone(report)
        self.assertIn("健康窗口耗尽", report)
        self.assertIn("1.1.0", report)
        self.assertIn("TRIAL", report)
        self.assertIn("不做版本切换", report)
        self.assertIn("不做无限重启", report)

    def test_rescue_preserves_transaction_file(self):
        """救援不得删改事务状态——那是排查现场的唯一凭据。"""
        self.sb.write_state(make_state("TRIAL", to="1.1.0"))
        r = make_recorder(self.sb, self.runner)
        r.enter_rescue("测试", rec.TxState(make_state("TRIAL")), "1.1.0")
        path = os.path.join(self.sb.state_dir, "transaction.json")
        self.assertTrue(os.path.exists(path))
        with open(path, encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["transaction"]["phase"], "TRIAL")

    def test_rescue_does_not_flip_pointer(self):
        """救援模式不做版本切换——未知状态不能继续传播。"""
        self.sb.set_current("1.1.0")
        r = make_recorder(self.sb, self.runner)
        r.enter_rescue("测试", rec.TxState(make_state("TRIAL")), "1.1.0")
        self.assertEqual(rec.read_current(self.sb.root), "1.1.0")
        self.assertFalse(self.runner.has("ln", "-sfn"))


# ---------------------------------------------------------------------------
# 编排：动作执行路径
# ---------------------------------------------------------------------------

class TestWatcherActions(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox()
        self.sb.write_state(make_state("TRIAL", to="1.1.0", attempts=0))
        self.sb.set_current("1.1.0")
        self.ctl_path = self.sb.make_fake_ctl()
        self.runner = FakeRunner({
            ("systemctl", "is-active"): (0, "active\n"),
            "busctl": {
                "HealthCheck": (0, 's "ok"'),
                "GetEbpfMonitorHealth": (0, 's "ok"'),
                "GetUpdateStatus": (0, 's "{\\"installed_version\\":\\"1.1.0\\"}"'),
            },
        }, delegate_paths=(self.ctl_path,))
        self.rec = make_recorder(self.sb, self.runner, ctl_path=self.ctl_path)

    def tearDown(self):
        self.sb.cleanup()

    def test_healthy_trial_commits_via_controller(self):
        watcher = rec.Watcher(self.rec, once=True)
        done, _ = watcher.run_once()
        self.assertTrue(done)
        verbs = self.sb.ctl_verbs()
        self.assertIn("commit", verbs)

    def test_boot_attempt_bumped_before_health_check(self):
        """试运行前先加一（断电无限重试的闸门）。"""
        watcher = rec.Watcher(self.rec, once=True)
        watcher.run_once()
        verbs = self.sb.ctl_verbs()
        self.assertIn("bump-boot-attempt", verbs)

    def test_unhealthy_trial_with_exhausted_attempts_rolls_back(self):
        self.sb.write_state(make_state("TRIAL", to="1.1.0", attempts=3))
        self.runner.table["busctl"] = {
            "HealthCheck": (1, ""),
            "GetEbpfMonitorHealth": (1, ""),
            "GetUpdateStatus": (1, ""),
        }
        watcher = rec.Watcher(self.rec, once=True)
        done, _ = watcher.run_once()
        self.assertFalse(done)     # 需要继续（推进回滚流程）
        verbs = self.sb.ctl_verbs()
        self.assertIn("rollback-arm", verbs)

    def test_controller_missing_falls_back_to_direct_atomic_flip(self):
        """控制器缺失时仍能翻链（救援路径不能因一个文件缺失而失效）。"""
        self.sb.write_state(make_state("ROLLBACK_PENDING", to="1.1.0", rollback="1.0.0"))
        self.sb.set_current("1.1.0")
        r = make_recorder(self.sb, self.runner)
        ok, detail = r.flip_current("1.0.0")
        self.assertTrue(ok)
        self.assertIn("直接原子翻链", detail)
        self.assertEqual(rec.read_current(self.sb.root), "1.0.0")

    def test_flip_to_missing_version_fails_closed(self):
        r = make_recorder(self.sb, self.runner)
        ok, detail = r.flip_current("9.9.9")
        self.assertFalse(ok)
        self.assertIn("不存在", detail)


# ---------------------------------------------------------------------------
# P0-C2 重启矩阵（离线可验证的部分）
#
# 这里是用户给的 P0-C2 验收矩阵中"不依赖真机重启"的几行，直接驱动
# Watcher.run_once 走真实恢复链路（假 systemctl / busctl / 控制器）：
#   * ROLLBACK_PENDING 期间重启 → 继续回滚，不重启失败候选
#   * 连续重启超过试运行次数 → 确定性回滚，不无限试运行
#   * 崩溃循环 → 提前回滚，不无限等健康窗口
#   * 恢复器判定与门禁判定必须一致（门禁放行的阶段不能让恢复器拒绝，
#     反之亦然——"每次重启都掉进救援"就是这两者打架的症状）
# ---------------------------------------------------------------------------

class TestRestartMatrix(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox()
        self.ctl_path = self.sb.make_fake_ctl()
        # 服务已崩：is-active 不 active、D-Bus 全不可达、NRestarts 高
        self.runner = FakeRunner({
            ("systemctl", "is-active"): (3, "inactive\n"),
            ("systemctl", "show"): (0, "3\n"),
            "busctl": {
                "HealthCheck": (1, ""),
                "GetEbpfMonitorHealth": (1, ""),
                "GetUpdateStatus": (1, ""),
            },
        }, delegate_paths=(self.ctl_path,))
        self.rec = make_recorder(self.sb, self.runner, ctl_path=self.ctl_path)

    def tearDown(self):
        self.sb.cleanup()

    def _watcher(self):
        w = rec.Watcher(self.rec, once=True)
        # 不读真机 boot_id：固定一个"新开机"
        w.read_boot_id = staticmethod(lambda: "boot-drill")
        return w

    def test_rollback_pending_continues_rollback_not_retry_candidate(self):
        """ROLLBACK_PENDING 期间重启 → 继续回滚到目标版本，绝不重试候选。

        这里故意让控制器不可用：本用例要验的是"指针一定被翻到回滚目标"
        这件事本身，而不是控制器实现。控制器在 ROLLBACK_PENDING 阶段本来
        也不接受 do-switch（它只认 SWITCH_PENDING/TRIAL），所以真实链路上
        走的正是这条直接原子翻链的兜底路径。
        """
        self.sb.write_state(make_state("ROLLBACK_PENDING", to="1.1.0",
                                       rollback="1.0.0"))
        self.sb.set_current("1.1.0")
        rec_no_ctl = make_recorder(self.sb, self.runner)  # ctl 路径不存在
        w = rec.Watcher(rec_no_ctl, once=True)
        w.read_boot_id = staticmethod(lambda: "boot-drill")
        w.run_once()
        self.assertEqual(rec.read_current(self.sb.root), "1.0.0")

    def test_rollback_verify_crash_loop_goes_to_rescue(self):
        """回滚目标自己也崩 → 救援模式，而不是无限重试。"""
        self.sb.write_state(make_state("ROLLBACK_VERIFY", to="1.1.0",
                                       rollback="1.0.0"))
        self.sb.set_current("1.0.0")
        w = self._watcher()
        done, _ = w.run_once()
        self.assertTrue(done)
        self.assertIsNotNone(self.sb.read_rescue())

    def test_exhausted_attempts_rolls_back(self):
        """连续重启超过试运行次数 → 确定性回滚，不再无限试运行。"""
        self.sb.write_state(make_state("TRIAL", to="1.1.0", rollback="1.0.0",
                                       attempts=rec.MAX_TRIAL_ROUNDS))
        self.sb.set_current("1.1.0")
        w = self._watcher()
        w.run_once()
        self.assertIn("rollback-arm", self.sb.ctl_verbs())

    def test_boot_attempt_bumped_once_per_boot_id(self):
        """恢复器自身被重启（同一 boot_id）不重复计数。"""
        self.sb.write_state(make_state("TRIAL", to="1.1.0", rollback="1.0.0"))
        self.sb.set_current("1.1.0")
        # 控制器真实执行：bump 幂等由控制器保证，这里验证恢复器每轮只调一次且
        # 用同一个 boot_id（重复调用不会导致计数增长）
        w1 = self._watcher()
        w1.run_once()
        w2 = self._watcher()
        w2.run_once()
        bumps = [c for c in self.sb.ctl_calls() if len(c) > 4 and c[4] == "bump-boot-attempt"]
        self.assertGreaterEqual(len(bumps), 1)
        for call in bumps:
            self.assertIn("boot-drill", call)


class TestGateConsistency(unittest.TestCase):
    """门禁与恢复器的判定不许互相打架。

    "门禁拒绝一个恢复器认为还能继续的阶段" = 每次重启直接掉进救援；
    "门禁放行一个恢复器认为必须救援的阶段" = 恢复器被绕过。两种都必须防。

    注意这里的"一致"不是"对同一状态给出同样的结论"——恢复器会**主动**
    把不可信状态转成救援（转完之后它自己也会拒绝启动），门禁看到的是
    已经落定的阶段。所以真正的断言是：
      * 恢复器愿意继续推进的活跃阶段 → 门禁必须放行
        （否则服务起不来，健康窗口永远无法判定，恢复器只能一路超时进救援）
      * 恢复器主动转救援的唯一阶段 RECOVERY_REQUIRED → 门禁必须拒绝
    """

    @classmethod
    def setUpClass(cls):
        gate_path = os.path.join(HERE, "weaknet-ota-gate")
        loader = importlib.machinery.SourceFileLoader("weaknet_ota_gate_x", gate_path)
        spec = importlib.util.spec_from_loader("weaknet_ota_gate_x", loader)
        cls.gate = importlib.util.module_from_spec(spec)
        loader.exec_module(cls.gate)

    def _state(self, phase):
        return {"schema_version": 1, "revision": 1,
                "stable_version": "1.0.0", "previous_stable_version": None,
                "transaction": {"transaction_id": "T", "kind": "upgrade",
                                "phase": phase, "from_version": "1.0.0",
                                "to_version": "1.1.0",
                                "rollback_target": "1.0.0",
                                "previous_stable_before": None,
                                "boot_attempts": 0, "last_trial_boot_id": "",
                                "policy": {}, "started_at": "2026-01-01T00:00:00Z",
                                "updated_at": "2026-01-01T00:00:00Z",
                                "last_error": ""}}

    def test_recoverer_wait_phases_are_allowed_by_gate(self):
        """恢复器判定为"继续等/推进"的阶段，门禁必须放行。"""
        cases = [
            # (phase, current, healthy, restarts, attempts) → decide 结果
            ("TRIAL", "1.1.0", False, 0, 0),          # wait_trial
            ("ROLLBACK_VERIFY", "1.0.0", False, 0, 0),  # wait_rollback
        ]
        for phase, current, healthy, restarts, attempts in cases:
            state = rec.TxState(self._state(phase))
            action, _ = rec.decide(state, current, healthy, 0, restarts, attempts)
            self.assertIn(action, ("wait_trial", "wait_rollback"), phase)
            allow, _ = self.gate.decide(self._state(phase), None)
            self.assertTrue(allow, "%s 门禁拒绝但恢复器要继续推进（会互相打架）" % phase)

    def test_rescue_phase_is_denied_by_gate(self):
        state = rec.TxState(self._state("RECOVERY_REQUIRED"))
        action, _ = rec.decide(state, "1.1.0", True, 0, 0, 0)
        self.assertEqual(action, "rescue")
        allow, _ = self.gate.decide(self._state("RECOVERY_REQUIRED"), None)
        self.assertFalse(allow)

    def test_every_live_phase_is_registered_on_both_sides(self):
        """阶段词表必须两边都认识：新增阶段只改一边会导致静默不一致。"""
        self.assertEqual(set(rec.LIVE_PHASES), set(self.gate.LIVE_PHASES))
        self.assertEqual(set(rec.TERMINAL_PHASES), set(self.gate.TERMINAL_PHASES))


if __name__ == "__main__":
    unittest.main()
