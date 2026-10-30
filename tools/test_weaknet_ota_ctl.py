#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""weaknet-ota-ctl 测试套件（P0-A）

覆盖方案要求的三层：
  1. 纯逻辑：schema 校验、状态转换、幂等、非法输入
  2. 崩溃点矩阵：第 6 节 13 个切点的 reconcile 判定
  3. 文件系统语义：原子写、软链翻转、投影重建、GC 保守性

真掉电测试无法在自动化测试里做（需要拔电），本文件用「构造持久事实 +
调用 reconcile」的方式验证判定逻辑；持久化本身的断电语义由 fsync 顺序保证，
需真机验收，测试里只断言顺序调用的正确性。

运行：python3 -m pytest tools/test_weaknet_ota_ctl.py -v
或：  python3 tools/test_weaknet_ota_ctl.py
"""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from importlib.machinery import SourceFileLoader

HERE = os.path.dirname(os.path.abspath(__file__))
CTL_PATH = os.path.join(HERE, "weaknet-ota-ctl")

# 控制器没有 .py 后缀（它是要直接执行的命令），spec_from_file_location 无法
# 从扩展名推断 loader，必须显式给 SourceFileLoader。
_loader = SourceFileLoader("weaknet_ota_ctl", CTL_PATH)
_spec = importlib.util.spec_from_loader("weaknet_ota_ctl", _loader)
ctl = importlib.util.module_from_spec(_spec)
_loader.exec_module(ctl)


class Sandbox(object):
    """构造一个假的 weaknet 根目录 + 状态目录。"""

    def __init__(self, versions=("1.0.0", "1.1.0"), current="1.0.0"):
        self.tmp = tempfile.mkdtemp(prefix="wn-ota-test-")
        self.root = os.path.join(self.tmp, "weaknet")
        self.state_dir = os.path.join(self.tmp, "ota-state")
        os.makedirs(os.path.join(self.root, "releases"))
        for ver in versions:
            self.add_release(ver)
        if current:
            os.symlink("releases/" + current, os.path.join(self.root, "current"))

    def add_release(self, version, committed=False):
        path = os.path.join(self.root, "releases", version)
        os.makedirs(os.path.join(path, "server", "bin"), exist_ok=True)
        with open(os.path.join(path, "VERSION"), "w") as fh:
            fh.write(version + "\n")
        if committed:
            open(os.path.join(path, ".committed"), "w").close()

    def archive_tx(self, tx):
        """把一笔事务写进 history/（模拟 archive_transaction 的产物）。"""
        hist_dir = os.path.join(self.state_dir, "history")
        os.makedirs(hist_dir, exist_ok=True)
        name = "%s-%s.json" % (tx["transaction_id"], tx["phase"])
        with open(os.path.join(hist_dir, name), "w") as fh:
            json.dump(tx, fh)

    def cleanup(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ---- 便捷操作（直接调库，便于断言状态） ----
    def load(self):
        return ctl.load_state(self.state_dir)

    def save(self, state):
        return ctl.save_state(self.state_dir, state)

    def run(self, *argv):
        return ctl.main(["--root", self.root, "--state-dir", self.state_dir] + list(argv))


class TestSchema(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox()

    def tearDown(self):
        self.sb.cleanup()

    def test_missing_state_returns_empty(self):
        state = self.sb.load()
        self.assertEqual(state["schema_version"], 1)
        self.assertIsNone(state["transaction"])
        self.assertEqual(state["revision"], 0)

    def test_reject_unknown_schema_version(self):
        os.makedirs(self.sb.state_dir, exist_ok=True)
        with open(ctl.state_path(self.sb.state_dir), "w") as fh:
            json.dump({"schema_version": 99, "revision": 1,
                       "stable_version": None, "previous_stable_version": None,
                       "transaction": None}, fh)
        with self.assertRaises(ctl.SchemaError):
            self.sb.load()

    def test_reject_corrupt_json(self):
        os.makedirs(self.sb.state_dir, exist_ok=True)
        with open(ctl.state_path(self.sb.state_dir), "w") as fh:
            fh.write("{not json")
        with self.assertRaises(ctl.SchemaError):
            self.sb.load()

    def test_reject_unknown_phase(self):
        state = ctl.empty_state()
        state["transaction"] = {
            "transaction_id": "t1", "kind": "upgrade", "phase": "MADE_UP",
            "from_version": "1.0.0", "to_version": "1.1.0",
            "rollback_target": "1.0.0", "previous_stable_before": None,
            "boot_attempts": 0,
        }
        self.sb.save(state)
        with self.assertRaises(ctl.SchemaError):
            self.sb.load()

    def test_reject_path_traversal_version(self):
        for bad in ("../../etc", "1.0", "1.0.0/x", "v1.0.0", "1.0.0\n"):
            with self.assertRaises(ctl.OtaError):
                ctl.check_version(bad)

    def test_revision_increments(self):
        state = self.sb.load()
        self.sb.save(state)
        self.assertEqual(self.sb.load()["revision"], 1)
        state = self.sb.load()
        self.sb.save(state)
        self.assertEqual(self.sb.load()["revision"], 2)


class TestStateMachine(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.0.0")

    def tearDown(self):
        self.sb.cleanup()

    def _begin(self, to="1.1.0", frm="1.0.0", rollback="1.0.0"):
        rc = self.sb.run("begin", "--from", frm, "--to", to, "--rollback-target", rollback)
        self.assertEqual(rc, ctl.EXIT_OK)
        return self.sb.load()

    def test_happy_path_commit(self):
        self._begin()
        self.assertEqual(self.sb.load()["transaction"]["phase"], ctl.PHASE_PREPARED)

        self.assertEqual(self.sb.run("arm-switch"), ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["transaction"]["phase"], ctl.PHASE_SWITCH_PENDING)
        # 关键：arm-switch 之后指针**还没翻**
        self.assertEqual(ctl.read_current(self.sb.root), "1.0.0")

        self.assertEqual(self.sb.run("do-switch"), ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["transaction"]["phase"], ctl.PHASE_TRIAL)
        self.assertEqual(ctl.read_current(self.sb.root), "1.1.0")

        self.assertEqual(self.sb.run("commit"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["transaction"]["phase"], ctl.PHASE_COMMITTED)
        self.assertEqual(state["stable_version"], "1.1.0")
        self.assertEqual(state["previous_stable_version"], "1.0.0")
        # 兼容投影
        self.assertTrue(os.path.exists(
            os.path.join(self.sb.root, "releases", "1.1.0", ".committed")))
        with open(os.path.join(self.sb.root, "releases", ".previous")) as fh:
            self.assertEqual(fh.read().strip(), "1.0.0")

    def test_do_switch_idempotent(self):
        self._begin()
        self.sb.run("arm-switch")
        self.assertEqual(self.sb.run("do-switch"), ctl.EXIT_OK)
        rev_after_first = self.sb.load()["revision"]
        self.assertEqual(self.sb.run("do-switch"), ctl.EXIT_OK)
        # 重复调用不改 phase、不重复翻指针
        self.assertEqual(self.sb.load()["transaction"]["phase"], ctl.PHASE_TRIAL)
        self.assertEqual(ctl.read_current(self.sb.root), "1.1.0")
        self.assertEqual(self.sb.load()["revision"], rev_after_first)

    def test_cannot_begin_while_live_transaction(self):
        self._begin()
        rc = self.sb.run("begin", "--from", "1.0.0", "--to", "1.1.0",
                         "--rollback-target", "1.0.0")
        self.assertEqual(rc, ctl.EXIT_CONFLICT)

    def test_rollback_target_cannot_equal_target(self):
        rc = self.sb.run("begin", "--from", "1.0.0", "--to", "1.1.0",
                         "--rollback-target", "1.1.0")
        self.assertEqual(rc, ctl.EXIT_USAGE)

    def test_commit_requires_trial(self):
        self._begin()
        # PREPARED 阶段不能直接 commit（非 forced）
        self.assertEqual(self.sb.run("commit"), ctl.EXIT_CONFLICT)

    def test_commit_rejects_when_current_is_not_candidate(self):
        self._begin()
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        # 外力把指针改回旧版
        ctl.atomic_symlink("releases/1.0.0", os.path.join(self.sb.root, "current"))
        self.assertEqual(self.sb.run("commit"), ctl.EXIT_CONFLICT)

    def test_abort_from_prepared(self):
        self._begin()
        self.assertEqual(self.sb.run("abort", "--reason", "test"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["transaction"]["phase"], ctl.PHASE_ABORTED)
        # 稳定版本元数据不受影响
        self.assertEqual(ctl.read_current(self.sb.root), "1.0.0")

    def test_abort_rejected_if_current_moved(self):
        self._begin()
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        # 已是 TRIAL，abort 不接受
        self.assertEqual(self.sb.run("abort"), ctl.EXIT_CONFLICT)

    def test_first_install_without_current(self):
        """全新设备：没有 current 软链，from == to == 目标版本。

        「指针必须等于原版本」的前提在首次安装下不成立（根本没有原版本），
        控制器必须放行，否则新设备永远装不上第一个版本。
        """
        fresh = Sandbox(versions=(), current=None)
        try:
            staged = os.path.join(fresh.tmp, "staging")
            os.makedirs(os.path.join(staged, "server", "bin"), exist_ok=True)
            with open(os.path.join(staged, "VERSION"), "w") as fh:
                fh.write("2.0.0\n")
            rc = fresh.run("begin", "--from", "2.0.0", "--to", "2.0.0",
                           "--rollback-target", "2.0.0", "--staged", staged)
            self.assertEqual(rc, ctl.EXIT_OK)
            self.assertEqual(fresh.run("materialize"), ctl.EXIT_OK)
            self.assertEqual(fresh.run("arm-switch"), ctl.EXIT_OK)
            self.assertEqual(fresh.run("do-switch"), ctl.EXIT_OK)
            self.assertEqual(ctl.read_current(fresh.root), "2.0.0")
            self.assertEqual(fresh.run("commit"), ctl.EXIT_OK)
            state = fresh.load()
            self.assertEqual(state["stable_version"], "2.0.0")
            self.assertIsNone(state["previous_stable_version"])
        finally:
            fresh.cleanup()

    def test_arm_switch_rejects_when_current_is_third_version(self):
        """current 指向既非 from 也非 to 的版本时，必须拒绝推进。"""
        self.sb.add_release("1.2.0")
        ctl.atomic_symlink("releases/1.2.0", os.path.join(self.sb.root, "current"))
        self._begin()
        self.assertEqual(self.sb.run("arm-switch"), ctl.EXIT_CONFLICT)

    def test_forced_commit(self):
        self._begin()
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        self.assertEqual(self.sb.run("commit", "--forced"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["transaction"]["phase"], ctl.PHASE_FORCED_COMMITTED)
        self.assertEqual(state["stable_version"], "1.1.0")

    def test_same_version_commit_keeps_previous_unchanged(self):
        """同版本幂等重装不得把 previous 定成 current 自己。

        若 previous == current，之后的无参回滚会以「目标与当前相同」失败，
        设备就少了一条自动退路。
        """
        self.sb.add_release("1.0.0", committed=True)
        rc = self.sb.run("begin", "--from", "1.0.0", "--to", "1.0.0",
                         "--rollback-target", "1.0.0")
        self.assertEqual(rc, ctl.EXIT_OK)
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        self.assertEqual(self.sb.run("commit"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["stable_version"], "1.0.0")
        self.assertNotEqual(state["previous_stable_version"], "1.0.0")
        # .previous 投影也不得指向 current
        prev_file = os.path.join(self.sb.root, "releases", ".previous")
        if os.path.exists(prev_file):
            with open(prev_file) as fh:
                self.assertNotEqual(fh.read().strip(), "1.0.0")

    def test_rollback_flow(self):
        self._begin()
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        self.assertEqual(ctl.read_current(self.sb.root), "1.1.0")

        self.assertEqual(self.sb.run("rollback-arm", "--reason", "unhealthy"), ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["transaction"]["phase"], ctl.PHASE_ROLLBACK_PENDING)
        # 意图落盘后、翻链前，指针仍是失败候选
        self.assertEqual(ctl.read_current(self.sb.root), "1.1.0")

        self.assertEqual(self.sb.run("rollback-do"), ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["transaction"]["phase"], ctl.PHASE_ROLLBACK_VERIFY)
        self.assertEqual(ctl.read_current(self.sb.root), "1.0.0")

        self.assertEqual(self.sb.run("rolled-back"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["transaction"]["phase"], ctl.PHASE_ROLLED_BACK)
        self.assertEqual(state["stable_version"], "1.0.0")
        # 关键不变量：失败候选 1.1.0 绝不能成为 previous_stable
        self.assertNotEqual(state["previous_stable_version"], "1.1.0")

    def test_rolled_back_requires_target_in_place(self):
        self._begin()
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        self.sb.run("rollback-arm")
        # 未执行 rollback-do 就标记 ROLLED_BACK，必须拒绝
        self.assertEqual(self.sb.run("rolled-back"), ctl.EXIT_CONFLICT)

    def test_bump_boot_attempt_is_idempotent_per_boot_id(self):
        """同一次开机内重复计数不重复加（恢复服务自身重启不算新尝试）。"""
        self._begin()
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        self.assertEqual(self.sb.run("bump-boot-attempt", "--boot-id", "boot-A"),
                         ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["transaction"]["boot_attempts"], 1)
        # 同 boot_id 再来一次 → 不加
        self.assertEqual(self.sb.run("bump-boot-attempt", "--boot-id", "boot-A"),
                         ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["transaction"]["boot_attempts"], 1)
        # 换了 boot_id（设备真的重启过）→ 加
        self.assertEqual(self.sb.run("bump-boot-attempt", "--boot-id", "boot-B"),
                         ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["transaction"]["boot_attempts"], 2)

    def test_bump_boot_attempt_requires_trial(self):
        """非 TRIAL 阶段不允许计数（那是"试运行尝试"的计数）。"""
        self._begin()
        self.assertEqual(self.sb.run("bump-boot-attempt", "--boot-id", "boot-A"),
                         ctl.EXIT_CONFLICT)

    def test_previous_stable_survives_rollback_without_explicit_flag(self):
        """回归：begin 未传 --previous-stable-before 时，必须沿用状态里的值。

        演练里发现的真实缺陷：升级脚本漏传该参数 → rolled-back 把
        previous_stable_version 写成 None，"上一稳定版本"这个事实被丢掉。
        """
        # 先建立稳定基线：stable=1.1.0, previous=1.0.0
        state = self.sb.load()
        state["stable_version"] = "1.1.0"
        state["previous_stable_version"] = "1.0.0"
        self.sb.save(state)
        self.sb.add_release("1.2.0")
        ctl.atomic_symlink("releases/1.1.0", os.path.join(self.sb.root, "current"))

        # 注意：不传 --previous-stable-before
        self.assertEqual(self.sb.run("begin", "--from", "1.1.0", "--to", "1.2.0",
                                     "--rollback-target", "1.1.0"), ctl.EXIT_OK)
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        self.sb.run("rollback-arm")
        self.sb.run("rollback-do")
        self.assertEqual(self.sb.run("rolled-back"), ctl.EXIT_OK)

        state = self.sb.load()
        self.assertEqual(state["stable_version"], "1.1.0")
        self.assertEqual(state["previous_stable_version"], "1.0.0",
                         "回滚后 previous_stable 被清空——上一稳定版本事实丢失")

    def test_recovery_required_blocks_begin(self):
        self._begin()
        self.assertEqual(self.sb.run("require-recovery", "--reason", "x"), ctl.EXIT_OK)
        rc = self.sb.run("begin", "--from", "1.0.0", "--to", "1.1.0",
                         "--rollback-target", "1.0.0")
        self.assertEqual(rc, ctl.EXIT_CONFLICT)

    def test_recover_accept(self):
        self._begin()
        self.sb.run("require-recovery", "--reason", "disk corrupt")
        self.assertEqual(self.sb.run("recover", "--accept", "1.0.0"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["transaction"]["phase"], ctl.PHASE_COMMITTED)
        self.assertEqual(state["stable_version"], "1.0.0")

    def test_recover_accept_keeps_previous_stable(self):
        """人工恢复不得丢掉「上一稳定版本」这个事实。

        回归：板端演练发现 recover --accept 把 previous_stable_version 写成
        None（事务里 previous_stable_before 为空时），设备就少了一条自动
        退路，且 .previous 投影被删。
        """
        state = self.sb.load()
        state["stable_version"] = "1.1.0"
        state["previous_stable_version"] = "1.0.0"
        self.sb.save(state)
        self._begin()
        self.sb.run("require-recovery", "--reason", "disk corrupt")
        # 认定 1.1.0 为已知良好版本（当前 stable 就是它）
        self.assertEqual(self.sb.run("recover", "--accept", "1.1.0"), ctl.EXIT_OK)
        after = self.sb.load()
        self.assertEqual(after["stable_version"], "1.1.0")
        self.assertEqual(after["previous_stable_version"], "1.0.0")

    def test_recover_accept_drops_previous_equal_to_accepted(self):
        """认定版本 == 上一稳定版本时，previous 必须清空（否则没有退路）。"""
        state = self.sb.load()
        state["stable_version"] = "1.1.0"
        state["previous_stable_version"] = "1.0.0"
        self.sb.save(state)
        self._begin()
        self.sb.run("require-recovery", "--reason", "disk corrupt")
        self.assertEqual(self.sb.run("recover", "--accept", "1.0.0"), ctl.EXIT_OK)
        after = self.sb.load()
        self.assertEqual(after["stable_version"], "1.0.0")
        self.assertIsNone(after["previous_stable_version"])

    def test_recover_requires_explicit_choice(self):
        self._begin()
        self.sb.run("require-recovery")
        self.assertEqual(self.sb.run("recover"), ctl.EXIT_USAGE)

    def test_terminal_transaction_archived_before_next(self):
        self._begin()
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        self.sb.run("commit")
        first_id = self.sb.load()["transaction"]["transaction_id"]
        # 开第二笔事务，第一笔应被归档。
        # 注意 --to 与 --rollback-target 必须是不同版本（失败版本不得成为
        # 回滚目标是硬约束），所以第二笔用 1.2.0 -> 回滚到 1.1.0。
        self.sb.add_release("1.2.0")
        self.assertEqual(self.sb.run("begin", "--from", "1.1.0", "--to", "1.2.0",
                                     "--rollback-target", "1.1.0"), ctl.EXIT_OK)
        hist = os.path.join(self.sb.state_dir, "history")
        self.assertTrue(os.path.isdir(hist))
        names = os.listdir(hist)
        self.assertTrue(any(first_id in n for n in names),
                        "终态事务未归档：%r" % names)


class TestRescueFlag(unittest.TestCase):
    """rescue.flag 是权威状态的**派生投影**，给 systemd 启动门禁用。

    两条硬约束：
      1. 标记与权威状态**永远一致**（进入救援 → 有；离开救援 → 无）
      2. 不一致时的失败方向是**过度保护**（标记在、状态说不在），
         绝不是"标记缺失、状态说在"那种不安全方向
    """

    def setUp(self):
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.0.0")

    def tearDown(self):
        self.sb.cleanup()

    def _flag(self):
        return ctl.rescue_flag_path(self.sb.state_dir)

    def _begin(self, to="1.1.0", frm="1.0.0", rollback="1.0.0"):
        return self.sb.run("begin", "--from", frm, "--to", to,
                            "--rollback-target", rollback)

    def test_require_recovery_creates_flag(self):
        self._begin()
        self.assertFalse(os.path.exists(self._flag()))
        self.assertEqual(self.sb.run("require-recovery", "--reason", "disk"),
                         ctl.EXIT_OK)
        self.assertTrue(os.path.exists(self._flag()))
        # 标记内容是 JSON，带原因
        with open(self._flag(), encoding="utf-8") as fh:
            payload = json.load(fh)
        self.assertEqual(payload["phase"], ctl.PHASE_RECOVERY_REQUIRED)
        self.assertEqual(payload["reason"], "disk")

    def test_recover_accept_clears_flag(self):
        self._begin()
        self.sb.run("require-recovery", "--reason", "disk")
        self.assertTrue(os.path.exists(self._flag()))
        self.assertEqual(self.sb.run("recover", "--accept", "1.0.0"),
                         ctl.EXIT_OK)
        self.assertFalse(os.path.exists(self._flag()))

    def test_recover_abort_clears_flag(self):
        self._begin()
        self.sb.run("require-recovery", "--reason", "disk")
        self.assertTrue(os.path.exists(self._flag()))
        self.assertEqual(self.sb.run("recover", "--abort"), ctl.EXIT_OK)
        self.assertFalse(os.path.exists(self._flag()))

    def test_enter_rescue_creates_flag(self):
        self._begin()
        self.assertEqual(self.sb.run("enter-rescue", "--reason", "untrusted"),
                         ctl.EXIT_OK)
        self.assertTrue(os.path.exists(self._flag()))
        self.assertEqual(self.sb.load()["transaction"]["phase"],
                         ctl.PHASE_RECOVERY_REQUIRED)

    # ---- RESCUE.md 也是救援态的派生视图（板端演练暴露的陈旧现场问题）----

    def _report(self):
        return ctl.rescue_report_path(self.sb.state_dir)

    def test_recover_clears_stale_rescue_report(self):
        """离开救援态必须连现场报告一起清掉。

        否则下一次真进救援时，日志里重放的是一份"上次"的 RESCUE.md：
        现场信息与触发原因全是旧的，会把运维引向错误的版本与错误的判断。
        """
        self._begin()
        self.sb.run("require-recovery", "--reason", "disk")
        with open(self._report(), "w", encoding="utf-8") as fh:
            fh.write("# 旧现场\n")
        self.assertEqual(self.sb.run("recover", "--accept", "1.0.0"),
                         ctl.EXIT_OK)
        self.assertFalse(os.path.exists(self._report()))

    def test_sync_rescue_flag_clears_stale_report(self):
        self._begin()
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        self.sb.run("commit")
        with open(self._report(), "w", encoding="utf-8") as fh:
            fh.write("# 旧现场\n")
        self.assertEqual(self.sb.run("sync-rescue-flag"), ctl.EXIT_OK)
        self.assertFalse(os.path.exists(self._report()))

    def test_sync_keeps_report_while_still_in_rescue(self):
        """仍在救援态时报告是**当前现场**，不能删。"""
        self._begin()
        self.sb.run("require-recovery", "--reason", "disk")
        with open(self._report(), "w", encoding="utf-8") as fh:
            fh.write("# 当前现场\n")
        self.assertEqual(self.sb.run("sync-rescue-flag"), ctl.EXIT_OK)
        self.assertTrue(os.path.exists(self._report()))

    def test_sync_rescue_flag_creates_missing_flag(self):
        """状态在救援态但标记缺失 → 补写（过度保护方向）。"""
        self._begin()
        self.sb.run("require-recovery", "--reason", "disk")
        os.unlink(self._flag())
        self.assertEqual(self.sb.run("sync-rescue-flag"), ctl.EXIT_OK)
        self.assertTrue(os.path.exists(self._flag()))

    def test_sync_rescue_flag_removes_stale_flag(self):
        """状态不在救援态但标记存在 → 删除（陈旧标记）。"""
        self._begin()
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        self.sb.run("commit")
        # 人工塞一个陈旧标记
        with open(self._flag(), "w", encoding="utf-8") as fh:
            fh.write("{}")
        self.assertEqual(self.sb.run("sync-rescue-flag"), ctl.EXIT_OK)
        self.assertFalse(os.path.exists(self._flag()))

    def test_sync_rescue_flag_dry_run_does_not_write(self):
        self._begin()
        self.sb.run("require-recovery", "--reason", "disk")
        os.unlink(self._flag())
        self.assertEqual(self.sb.run("sync-rescue-flag", "--dry-run"),
                         ctl.EXIT_OK)
        self.assertFalse(os.path.exists(self._flag()))

    def test_commit_clears_stale_flag(self):
        """转正时若标记残留（陈旧），必须清掉——否则业务服务永远起不来。"""
        self._begin()
        self.sb.run("arm-switch")
        self.sb.run("do-switch")
        with open(self._flag(), "w", encoding="utf-8") as fh:
            fh.write("{}")
        self.assertEqual(self.sb.run("commit"), ctl.EXIT_OK)
        self.assertFalse(os.path.exists(self._flag()))

    def test_abort_clears_stale_flag(self):
        self._begin()
        with open(self._flag(), "w", encoding="utf-8") as fh:
            fh.write("{}")
        self.assertEqual(self.sb.run("abort", "--reason", "x"), ctl.EXIT_OK)
        self.assertFalse(os.path.exists(self._flag()))


class TestReconcileMatrix(unittest.TestCase):
    """第 6 节崩溃点矩阵：按持久事实断言 reconcile 的判定。"""

    def setUp(self):
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.0.0")

    def tearDown(self):
        self.sb.cleanup()

    def _state_with(self, phase, cur, to="1.1.0", frm="1.0.0", rollback="1.0.0"):
        state = ctl.empty_state()
        state["transaction"] = ctl.new_transaction("upgrade", frm, to, rollback, None)
        state["transaction"]["phase"] = phase
        ctl.atomic_symlink("releases/" + cur, os.path.join(self.sb.root, "current"))
        return state

    def _verdict(self, phase, cur):
        state = self._state_with(phase, cur)
        return ctl.reconcile(self.sb.root, state)

    # ① 写 PREPARED 临时文件中 → 磁盘上是上一份有效状态（无事务）
    def test_01_no_transaction(self):
        v = ctl.reconcile(self.sb.root, ctl.empty_state())
        self.assertEqual(v["action"], "none")

    # ② PREPARED 落盘之后、未切换
    def test_02_prepared_old_version(self):
        self.assertEqual(self._verdict(ctl.PHASE_PREPARED, "1.0.0")["action"], "abort")

    # ③ SWITCH_PENDING 落盘、翻指针之前
    def test_03_switch_pending_before_flip(self):
        self.assertEqual(self._verdict(ctl.PHASE_SWITCH_PENDING, "1.0.0")["action"], "abort")

    # ④ 翻 current 期间（已翻到候选）
    def test_04a_switch_pending_after_flip(self):
        self.assertEqual(self._verdict(ctl.PHASE_SWITCH_PENDING, "1.1.0")["action"], "mark_trial")

    # ④' current 指向第三版本 → 不可判定
    def test_04b_third_version(self):
        self.sb.add_release("0.9.0")
        v = self._verdict(ctl.PHASE_SWITCH_PENDING, "0.9.0")
        self.assertEqual(v["action"], "recovery_required")

    # ⑤ current 已为候选、TRIAL 未落盘 —— 与 ④a 同形，通过 mark_trial 收敛
    def test_05_current_candidate_trial_not_persisted(self):
        self.assertEqual(self._verdict(ctl.PHASE_SWITCH_PENDING, "1.1.0")["action"], "mark_trial")

    # ⑥ TRIAL 期间
    def test_06_trial(self):
        self.assertEqual(self._verdict(ctl.PHASE_TRIAL, "1.1.0")["action"], "continue_trial")

    # ⑦ TRIAL 中整机掉电 —— 状态与 ⑥ 相同，由 boot_attempts 限次（P0-B 执行）
    def test_07_trial_after_power_loss(self):
        v = self._verdict(ctl.PHASE_TRIAL, "1.1.0")
        self.assertEqual(v["action"], "continue_trial")

    # ⑧ COMMITTED 已落盘、.committed 未补写
    def test_08_committed_marker_missing(self):
        state = self._state_with(ctl.PHASE_COMMITTED, "1.1.0")
        state["stable_version"] = "1.1.0"
        v = ctl.reconcile(self.sb.root, state)
        self.assertEqual(v["action"], "project_views")

    # ⑧' COMMITTED 且投影已在 → 无事可做
    def test_08b_committed_marker_present(self):
        state = self._state_with(ctl.PHASE_COMMITTED, "1.1.0")
        state["stable_version"] = "1.1.0"
        open(os.path.join(self.sb.root, "releases", "1.1.0", ".committed"), "w").close()
        self.assertEqual(ctl.reconcile(self.sb.root, state)["action"], "none")

    # ⑨ ROLLBACK_PENDING 落盘、尚未翻回
    def test_09_rollback_pending_not_flipped(self):
        v = self._verdict(ctl.PHASE_ROLLBACK_PENDING, "1.1.0")
        self.assertEqual(v["action"], "continue_rollback")

    # ⑩ 回滚翻链时掉电（已翻回）
    def test_10_rollback_flipped(self):
        v = self._verdict(ctl.PHASE_ROLLBACK_PENDING, "1.0.0")
        self.assertEqual(v["action"], "continue_rollback")

    # ⑪ ROLLBACK_VERIFY 中掉电
    def test_11_rollback_verify(self):
        v = self._verdict(ctl.PHASE_ROLLBACK_VERIFY, "1.0.0")
        self.assertEqual(v["action"], "continue_trial")

    # ⑫ 已记 ROLLED_BACK，投影未刷新
    def test_12_rolled_back(self):
        v = self._verdict(ctl.PHASE_ROLLED_BACK, "1.0.0")
        self.assertEqual(v["action"], "none")

    # ⑬ 不可信状态
    def test_13_recovery_required(self):
        v = self._verdict(ctl.PHASE_RECOVERY_REQUIRED, "1.1.0")
        self.assertEqual(v["action"], "recovery_required")

    def test_13b_trial_but_current_moved(self):
        v = self._verdict(ctl.PHASE_TRIAL, "1.0.0")
        self.assertEqual(v["action"], "recovery_required")

    def test_dry_run_never_mutates(self):
        self._state_with(ctl.PHASE_SWITCH_PENDING, "1.0.0")
        state = self._state_with(ctl.PHASE_SWITCH_PENDING, "1.0.0")
        self.sb.save(state)
        before_rev = self.sb.load()["revision"]
        before_cur = ctl.read_current(self.sb.root)
        rc = self.sb.run("reconcile", "--dry-run")
        self.assertEqual(rc, ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["revision"], before_rev)
        self.assertEqual(ctl.read_current(self.sb.root), before_cur)


class TestGcConservatism(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox(versions=("1.0.0", "1.1.0", "1.2.0", "1.3.0", "1.4.0"),
                          current="1.4.0")

    def tearDown(self):
        self.sb.cleanup()

    def test_skip_when_transaction_live(self):
        state = ctl.empty_state()
        state["transaction"] = ctl.new_transaction("upgrade", "1.3.0", "1.4.0", "1.3.0", None)
        state["transaction"]["phase"] = ctl.PHASE_TRIAL
        self.sb.save(state)
        removed, reason = ctl.collect_garbage(self.sb.root, self.sb.load(), 2)
        self.assertEqual(removed, [])
        self.assertIsNotNone(reason)
        # 所有版本仍在
        self.assertEqual(len(ctl.list_versions(self.sb.root)), 5)

    def test_skip_when_recovery_required(self):
        state = ctl.empty_state()
        state["transaction"] = ctl.new_transaction("upgrade", "1.3.0", "1.4.0", "1.3.0", None)
        state["transaction"]["phase"] = ctl.PHASE_RECOVERY_REQUIRED
        self.sb.save(state)
        removed, reason = ctl.collect_garbage(self.sb.root, self.sb.load(), 2)
        self.assertEqual(removed, [])
        self.assertIn("RECOVERY_REQUIRED", reason)

    def test_protects_current_and_stable(self):
        state = ctl.empty_state()
        state["stable_version"] = "1.4.0"
        state["previous_stable_version"] = "1.3.0"
        self.sb.save(state)
        removed, _ = ctl.collect_garbage(self.sb.root, self.sb.load(), 1)
        self.assertNotIn("1.4.0", removed)
        self.assertNotIn("1.3.0", removed)
        # 空闲时确实回收了陈旧版本
        self.assertTrue(removed)

    def test_keeps_max_keep_unprotected_versions(self):
        """max_keep 是「额外保留的未受保护版本数」，不是「总数」。

        受保护版本（current/stable/previous）始终留着，所以磁盘上可能超过
        max_keep 个版本——那是刻意的：误删回滚目标的代价远大于多占几 MB。
        """
        state = ctl.empty_state()
        state["stable_version"] = "1.4.0"
        state["previous_stable_version"] = "1.3.0"
        self.sb.save(state)
        removed, _ = ctl.collect_garbage(self.sb.root, self.sb.load(), 2)
        remaining = ctl.list_versions(self.sb.root)
        # 1.4.0/1.3.0 受保护；未受保护的 1.2.0/1.1.0/1.0.0 保留最新 2 个
        self.assertIn("1.4.0", remaining)
        self.assertIn("1.3.0", remaining)
        self.assertIn("1.2.0", remaining)
        self.assertIn("1.1.0", remaining)
        self.assertNotIn("1.0.0", remaining)
        self.assertEqual(removed, ["1.0.0"])


class TestProjections(unittest.TestCase):
    def setUp(self):
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")

    def tearDown(self):
        self.sb.cleanup()

    def test_project_committed_and_previous(self):
        state = ctl.empty_state()
        state["stable_version"] = "1.1.0"
        state["previous_stable_version"] = "1.0.0"
        warnings = ctl.project_views(self.sb.root, state)
        self.assertEqual(warnings, [])
        self.assertTrue(os.path.exists(
            os.path.join(self.sb.root, "releases", "1.1.0", ".committed")))
        with open(os.path.join(self.sb.root, "releases", ".previous")) as fh:
            self.assertEqual(fh.read().strip(), "1.0.0")

    def test_project_removes_stale_previous(self):
        with open(os.path.join(self.sb.root, "releases", ".previous"), "w") as fh:
            fh.write("1.0.0\n")
        state = ctl.empty_state()
        state["stable_version"] = "1.1.0"
        state["previous_stable_version"] = None
        ctl.project_views(self.sb.root, state)
        self.assertFalse(os.path.exists(os.path.join(self.sb.root, "releases", ".previous")))

    def test_symlink_keeps_relative_form(self):
        ctl.switch_current(self.sb.root, "1.0.0")
        target = os.readlink(os.path.join(self.sb.root, "current"))
        self.assertEqual(target, "releases/1.0.0")
        self.assertFalse(os.path.isabs(target))


class TestBootstrap(unittest.TestCase):
    def tearDown(self):
        if hasattr(self, "sb"):
            self.sb.cleanup()

    def test_bootstrap_from_committed(self):
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")
        self.sb.add_release("1.1.0", committed=True)
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["stable_version"], "1.1.0")

    def test_bootstrap_refuses_without_committed(self):
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_RECOVERY)

    def test_bootstrap_accept_stable_override(self):
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")
        self.assertEqual(self.sb.run("bootstrap", "--accept-stable", "1.1.0"), ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["stable_version"], "1.1.0")

    def test_bootstrap_refuses_overwrite(self):
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")
        self.sb.add_release("1.1.0", committed=True)
        self.sb.run("bootstrap")
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_CONFLICT)

    def test_bootstrap_prefers_committed_fact_over_previous_file(self):
        """上一稳定版本取「带 .committed 的最高版本」，不是旧 .previous 文件。

        旧布局的 .previous 语义是「上一次操作之前的版本」，可能等于 current。
        照抄它会把「回滚目标」定成当前版本 —— 那是砖化路径。
        """
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")
        self.sb.add_release("1.1.0", committed=True)
        self.sb.add_release("1.0.0", committed=True)
        # 旧脚本留下的 .previous：语义错误地指向 current 自己
        prev_file = os.path.join(self.sb.root, "releases", ".previous")
        with open(prev_file, "w") as fh:
            fh.write("1.1.0\n")
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["stable_version"], "1.1.0")
        self.assertEqual(state["previous_stable_version"], "1.0.0")

    def test_bootstrap_falls_back_to_previous_file_when_no_other_committed(self):
        """没有其他带 .committed 的版本时，才用 .previous 文件兜底。"""
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")
        self.sb.add_release("1.1.0", committed=True)
        prev_file = os.path.join(self.sb.root, "releases", ".previous")
        with open(prev_file, "w") as fh:
            fh.write("1.0.0\n")
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["previous_stable_version"], "1.0.0")

    def test_bootstrap_rejects_previous_equal_to_current(self):
        """兜底路径也不能把 current 自己定成上一稳定版本。"""
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")
        self.sb.add_release("1.1.0", committed=True)
        prev_file = os.path.join(self.sb.root, "releases", ".previous")
        with open(prev_file, "w") as fh:
            fh.write("1.1.0\n")
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_OK)
        self.assertIsNone(self.sb.load()["previous_stable_version"])

    # ---- 多个 .committed 版本时的歧义（用户指出的场景）----

    def _tx(self, tid, phase, frm, to, rollback=None, psb=None):
        tx = ctl.new_transaction("upgrade", frm, to, rollback or frm, psb)
        tx["transaction_id"] = tid
        tx["phase"] = phase
        return tx

    def test_bootstrap_prefers_transaction_history_over_highest_committed(self):
        """人工回滚后：1.1.0 与 1.2.0 都带 .committed，current 指向 1.1.0。

        「版本号最高的已转正版本」是 1.2.0，但那不是当前稳定版本。
        事务历史才说得清：1.1.0 转正 → 1.2.0 转正 → 人工回滚到 1.1.0，
        所以 stable=1.1.0、previous=1.2.0。
        """
        self.sb = Sandbox(versions=("1.1.0", "1.2.0"), current="1.1.0")
        self.sb.add_release("1.1.0", committed=True)
        self.sb.add_release("1.2.0", committed=True)
        # 归档历史：先 1.1.0 转正，再 1.2.0 转正，最后人工回滚到 1.1.0
        self.sb.archive_tx(self._tx("20260101T000000Z-1", ctl.PHASE_COMMITTED,
                                     "1.0.0", "1.1.0"))
        self.sb.archive_tx(self._tx("20260102T000000Z-2", ctl.PHASE_COMMITTED,
                                     "1.1.0", "1.2.0"))
        self.sb.archive_tx(self._tx("20260103T000000Z-3", ctl.PHASE_COMMITTED,
                                     "1.2.0", "1.1.0"))
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["stable_version"], "1.1.0")
        self.assertEqual(state["previous_stable_version"], "1.2.0")
        self.assertEqual(state["stable_source"], "transaction_history")

    def test_bootstrap_rolled_back_history_keeps_failed_candidate_out_of_previous(self):
        """历史末笔是自动回滚：失败候选绝不能成为上一稳定版本。"""
        self.sb = Sandbox(versions=("1.0.0", "1.1.0", "1.2.0"), current="1.0.0")
        for v in ("1.0.0", "1.1.0", "1.2.0"):
            self.sb.add_release(v, committed=True)
        self.sb.archive_tx(self._tx("20260101T000000Z-1", ctl.PHASE_COMMITTED,
                                     "0.9.0", "1.0.0"))
        # 1.2.0 试运行失败 → 回滚到 1.0.0（失败候选 1.2.0 已转正过，
        # 但在事务语义里它是失败的那个）
        self.sb.archive_tx(self._tx("20260102T000000Z-2", ctl.PHASE_ROLLED_BACK,
                                     "1.0.0", "1.2.0", rollback="1.0.0", psb=None))
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["stable_version"], "1.0.0")
        self.assertNotEqual(state["previous_stable_version"], "1.2.0")

    def test_bootstrap_history_conflicting_with_current_returns_unknown(self):
        """历史与 current 冲突时明确返回未知，不猜。"""
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")
        self.sb.add_release("1.0.0", committed=True)
        self.sb.add_release("1.1.0", committed=True)
        # 历史说最后落定的是 1.0.0，但 current 指向 1.1.0 —— 冲突
        self.sb.archive_tx(self._tx("20260101T000000Z-1", ctl.PHASE_COMMITTED,
                                     "0.9.0", "1.0.0"))
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_OK)
        state = self.sb.load()
        # 回落到 current 确认（1.1.0 带 .committed），但 previous 来源要标清楚
        self.assertEqual(state["stable_version"], "1.1.0")
        self.assertEqual(state["stable_source"], "current")
        # 1.0.0 带 .committed 但没有事务历史佐证 → 次优推断，来源明确标注
        self.assertEqual(state["previous_stable_version"], "1.0.0")
        self.assertEqual(state["previous_source"], "committed_mtime")

    def test_bootstrap_history_for_deleted_release_falls_back(self):
        """历史指向的版本目录已被 GC/删除时，不据历史断言，回落到 current。"""
        self.sb = Sandbox(versions=("1.1.0",), current="1.1.0")
        self.sb.add_release("1.1.0", committed=True)
        self.sb.archive_tx(self._tx("20260101T000000Z-1", ctl.PHASE_COMMITTED,
                                     "0.9.0", "9.9.9"))
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["stable_version"], "1.1.0")
        self.assertEqual(state["stable_source"], "current")

    def test_bootstrap_unknown_when_no_evidence(self):
        """既无历史也无其他已转正版本 → previous 明确未知。"""
        self.sb = Sandbox(versions=("1.1.0",), current="1.1.0")
        self.sb.add_release("1.1.0", committed=True)
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_OK)
        state = self.sb.load()
        self.assertIsNone(state["previous_stable_version"])
        self.assertEqual(state["previous_source"], "unknown")

    def test_bootstrap_accept_previous_override(self):
        """人工指定上一稳定版本。"""
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")
        self.sb.add_release("1.1.0", committed=True)
        self.sb.add_release("1.0.0", committed=True)
        self.assertEqual(self.sb.run("bootstrap", "--accept-previous", "1.0.0"),
                         ctl.EXIT_OK)
        state = self.sb.load()
        self.assertEqual(state["previous_stable_version"], "1.0.0")
        self.assertEqual(state["previous_source"], "manual")

    def test_bootstrap_corrupt_history_file_is_skipped(self):
        """单条历史损坏不该让整个 bootstrap 失败。"""
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.1.0")
        self.sb.add_release("1.1.0", committed=True)
        self.sb.add_release("1.0.0", committed=True)
        hist_dir = os.path.join(self.sb.state_dir, "history")
        os.makedirs(hist_dir, exist_ok=True)
        with open(os.path.join(hist_dir, "20260101T000000Z-1-COMMITTED.json"), "w") as fh:
            fh.write("{ broken")
        self.assertEqual(self.sb.run("bootstrap"), ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["stable_version"], "1.1.0")


class TestMaterialize(unittest.TestCase):
    """目录落位：跨版本 rename；同版本重装必须原子交换。"""

    def setUp(self):
        self.sb = Sandbox(versions=("1.0.0",), current="1.0.0")

    def tearDown(self):
        self.sb.cleanup()

    def _staging(self, version, marker):
        path = os.path.join(self.sb.tmp, "staging-" + version)
        os.makedirs(os.path.join(path, "server", "bin"), exist_ok=True)
        with open(os.path.join(path, "VERSION"), "w") as fh:
            fh.write(version + "\n")
        with open(os.path.join(path, marker), "w") as fh:
            fh.write(marker)
        return path

    def test_cross_version_rename(self):
        staged = self._staging("1.1.0", "NEW")
        rc = self.sb.run("begin", "--from", "1.0.0", "--to", "1.1.0",
                         "--rollback-target", "1.0.0", "--staged", staged)
        self.assertEqual(rc, ctl.EXIT_OK)
        self.assertEqual(self.sb.run("materialize"), ctl.EXIT_OK)
        self.assertTrue(os.path.isdir(os.path.join(self.sb.root, "releases", "1.1.0")))
        # 跨版本不改指针
        self.assertEqual(ctl.read_current(self.sb.root), "1.0.0")

    def test_same_version_atomic_exchange(self):
        self.sb.add_release("1.0.0", committed=True)
        old_content = os.path.join(self.sb.root, "releases", "1.0.0", "OLD_MARKER")
        open(old_content, "w").write("old")
        staged = self._staging("1.0.0", "NEW_MARKER")

        rc = self.sb.run("begin", "--from", "1.0.0", "--to", "1.0.0",
                         "--rollback-target", "1.0.0", "--staged", staged)
        self.assertEqual(rc, ctl.EXIT_OK)
        self.assertEqual(self.sb.run("materialize"), ctl.EXIT_OK)

        target = os.path.join(self.sb.root, "releases", "1.0.0")
        # 新内容就位
        self.assertTrue(os.path.exists(os.path.join(target, "NEW_MARKER")))
        self.assertFalse(os.path.exists(os.path.join(target, "OLD_MARKER")))
        # 旧内容被换到 stage 路径（供失败时恢复）
        self.assertTrue(os.path.exists(os.path.join(staged, "OLD_MARKER")))
        # current 始终有效（原子交换，不存在缺失窗口）
        self.assertEqual(ctl.read_current(self.sb.root), "1.0.0")
        self.assertTrue(os.path.isdir(os.path.join(self.sb.root, "current")))

    def test_materialize_idempotent(self):
        staged = self._staging("1.1.0", "NEW")
        self.sb.run("begin", "--from", "1.0.0", "--to", "1.1.0",
                    "--rollback-target", "1.0.0", "--staged", staged)
        self.assertEqual(self.sb.run("materialize"), ctl.EXIT_OK)
        rev = self.sb.load()["revision"]
        self.assertEqual(self.sb.run("materialize"), ctl.EXIT_OK)
        self.assertEqual(self.sb.load()["revision"], rev)

    def test_begin_requires_staged_when_target_missing(self):
        rc = self.sb.run("begin", "--from", "1.0.0", "--to", "9.9.9",
                         "--rollback-target", "1.0.0")
        self.assertEqual(rc, ctl.EXIT_USAGE)


class TestCliProcess(unittest.TestCase):
    """以子进程方式验证真实 CLI 行为（权限、退出码、JSON 输出）。"""

    def setUp(self):
        self.sb = Sandbox(versions=("1.0.0", "1.1.0"), current="1.0.0")

    def tearDown(self):
        self.sb.cleanup()

    def _exec(self, *argv):
        return subprocess.run(
            [sys.executable, CTL_PATH, "--root", self.sb.root,
             "--state-dir", self.sb.state_dir] + list(argv),
            capture_output=True, text=True)

    def test_status_json(self):
        out = self._exec("status")
        self.assertEqual(out.returncode, 0, out.stderr)
        payload = json.loads(out.stdout)
        self.assertEqual(payload["current"], "1.0.0")
        self.assertIsNone(payload["phase"])

    def test_begin_through_commit_via_cli(self):
        self.assertEqual(self._exec("begin", "--from", "1.0.0", "--to", "1.1.0",
                                    "--rollback-target", "1.0.0").returncode, 0)
        self.assertEqual(self._exec("arm-switch").returncode, 0)
        self.assertEqual(self._exec("do-switch").returncode, 0)
        self.assertEqual(self._exec("commit").returncode, 0)
        payload = json.loads(self._exec("status").stdout)
        self.assertEqual(payload["stable_version"], "1.1.0")
        self.assertEqual(payload["current"], "1.1.0")

    def test_concurrent_lock_rejected(self):
        # 手工持锁，另一个进程必须失败
        os.makedirs(self.sb.state_dir, exist_ok=True)
        import fcntl
        lock_path = os.path.join(self.sb.state_dir, "lock")
        fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            out = self._exec("begin", "--from", "1.0.0", "--to", "1.1.0",
                             "--rollback-target", "1.0.0")
            self.assertEqual(out.returncode, ctl.EXIT_CONFLICT, out.stderr)
        finally:
            os.close(fd)


if __name__ == "__main__":
    unittest.main(verbosity=2)
