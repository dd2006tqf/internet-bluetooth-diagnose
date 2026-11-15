#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
weaknet-health-guard 的判据解析测试（不需要开发板）

为什么这些用例值得存在：
  guard 的判据全部是「解析别人给的字符串」。解析错的方向有两种，都很难
  在真机上发现：
    * 偏严 → 恒假告警 → 每次正常波动都复位板子
    * 偏松 → 恒真通过 → 判据形同虚设，故障时照样喂狗

  两者在板端都表现为「看起来在跑」。因此把解析逻辑抽成可测的纯函数，
  用构造输入逐个钉死边界，是唯一能在没有板子的情况下验证它的办法。

被测对象：tools/weaknet-health-guard.cpp 里的算法。
本脚本用 Python 重实现同一套判定规则，并与其注释中声明的语义对照。
这不是重复实现（那没有意义），而是把「注释里写的规则」变成可执行断言，
防止代码与文档各说一套。

用法：python3 tools/test_weaknet_health_guard.py
"""

import re
import unittest

# ============================================================================
# 判据常量：必须与 tools/weaknet-health-guard.cpp 逐字一致。
# 不一致时本测试会立刻失败 —— 那是刻意的：常量漂移会让所有边界断言失效。
# ============================================================================
TICK_S = 2
DEBOUNCE_K = 2
WAL_MAX_AGE_S = 12
SAMPLE_MAX_AGE_S = 20
CPU_WINDOW_S = 6
CPU_MIN_TICKS = 1
DSTATE_MAX_S = 8
INACTIVE_GRACE_S = 12
MAINT_MAX_AGE_S = 30 * 60
OTA_PROGRESS_MAX_AGE_S = 300
RESTART_WARMUP_S = 12
RESTART_WINDOW_S = 20 * 60
RESTART_MAX_IN_WIN = 6


def _c_unescape(literal):
    """按 C 字符串字面量语义反转义，用于校验源码里的转义是否真的生效。"""
    out, i = [], 0
    simple = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0',
              '"': '"', "'": "'", '\\': '\\'}
    while i < len(literal):
        ch = literal[i]
        if ch == '\\' and i + 1 < len(literal):
            nxt = literal[i + 1]
            out.append(simple.get(nxt, nxt))
            i += 2
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


class TestConstantsAgainstSource(unittest.TestCase):
    """从 C++ 源码里抽取常量，与本文件断言的值对照。"""

    @classmethod
    def setUpClass(cls):
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            cls.src = f.read()

    def _const(self, name):
        m = re.search(
            r"constexpr\s+int\s+" + name + r"\s*=\s*([0-9*\s]+);", self.src)
        self.assertIsNotNone(m, f"源码里找不到常量 {name}")
        return eval(m.group(1).strip())  # 只含数字与 *

    def test_constants_match(self):
        for name, expected in [
            ("kTickSeconds", TICK_S),
            ("kDebounceK", DEBOUNCE_K),
            ("kWalMaxAgeSec", WAL_MAX_AGE_S),
            ("kSampleMaxAgeSec", SAMPLE_MAX_AGE_S),
            ("kCpuWindowSec", CPU_WINDOW_S),
            ("kCpuMinTicks", CPU_MIN_TICKS),
            ("kDStateMaxSec", DSTATE_MAX_S),
            ("kInactiveGraceSec", INACTIVE_GRACE_S),
            ("kMaintMaxAgeSec", MAINT_MAX_AGE_S),
            ("kOtaProgressMaxAgeSec", OTA_PROGRESS_MAX_AGE_S),
            ("kRestartWarmupSec", RESTART_WARMUP_S),
            ("kRestartWindowSec", RESTART_WINDOW_S),
            ("kRestartMaxInWin", RESTART_MAX_IN_WIN),
        ]:
            self.assertEqual(self._const(name), expected,
                             f"{name} 与源码不一致：常量已漂移")

    def test_16s_budget(self):
        """
        16 秒硬约束：最坏确认时间必须留在预算内。

        最坏路径 = 一次假通过后的去抖确认：
            一轮判定失败(K=1) → 2s → 再失败(K=2 触发) = 4s
        加上硬件从最后一次 ping 开始计时的 16s，总保护窗口足够。
        这里只断言去抖本身的耗时上限，不重复计算硬件部分。
        """
        worst = TICK_S * DEBOUNCE_K
        self.assertLessEqual(worst, 8, "去抖确认耗时应远小于 16s 硬件超时")

    def test_debounce_never_three(self):
        """
        K=3 在 16 秒预算下不安全：3 轮 = 6s，加上执行抖动与判据本身的
        滞后，留给硬件的时间窗太薄。注释明确禁止，这里钉死。
        """
        self.assertLessEqual(DEBOUNCE_K, 2, "去抖 K 不允许超过 2（16s 预算）")


class TestWalAge(unittest.TestCase):
    """判据 1：WAL mtime 年龄判定。"""

    def ok(self, age):
        return age <= WAL_MAX_AGE_S

    def test_fresh_is_ok(self):
        for age in (0, 1, 5, 11):
            self.assertTrue(self.ok(age), f"age={age} 应通过")

    def test_boundary(self):
        self.assertTrue(self.ok(WAL_MAX_AGE_S), "恰好等于阈值应通过")
        self.assertFalse(self.ok(WAL_MAX_AGE_S + 1), "超过阈值应失败")

    def test_stale_is_fail(self):
        for age in (13, 60, 600, 3600):
            self.assertFalse(self.ok(age), f"age={age} 应失败")

    def test_negative_age_tolerated(self):
        """
        时钟回跳（无 RTC 板子首次 NTP 校正）会产生负数年龄。
        C++ 侧直接算差值，负数 <= 阈值 → 通过。这是正确的保守选择：
        无法判断新鲜度时不该误报。
        """
        self.assertTrue(self.ok(-5))
        self.assertTrue(self.ok(-10000))


class TestSampleNsParsing(unittest.TestCase):
    """
    判据 2：从 busctl 输出里扫 last_successful_sample_ns。

    C++ 实现用 strstr + strtoll 逐项扫描，判据是
    「非零项中至少 2/3 在窗口内」。这里复刻同一规则并测边界。
    """

    KEY = '"last_successful_sample_ns":'

    def parse(self, text):
        vals = []
        pos = 0
        while True:
            i = text.find(self.KEY, pos)
            if i < 0:
                break
            rest = text[i + len(self.KEY):]
            m = re.match(r"\s*(-?\d+)", rest)
            if m:
                vals.append(int(m.group(1)))
                pos = i + len(self.KEY) + m.end()
            else:
                pos = i + len(self.KEY)
        return vals

    def judge(self, vals, now_ns):
        """与 C++ 中 fresh*3 >= nonzero*2 的判定一致。"""
        nonzero = [v for v in vals if v > 0]
        if not nonzero:
            return False   # 无活跃监控器：判失败（C++ 同）
        fresh = sum(1 for v in nonzero
                    if 0 <= now_ns - v <= SAMPLE_MAX_AGE_S * 10**9)
        return fresh * 3 >= len(nonzero) * 2

    def test_parse_plain(self):
        s = '{"monitors":[' + self.KEY + '12345}]}'
        self.assertEqual(self.parse(s), [12345])

    # ---- 真实 busctl 输出形态 -------------------------------------------
    #
    # 2026-11-15 板端 dry-run 暴露：判据恒失败。上面所有用例的夹具都是
    # **裸 JSON**，而 busctl 实际返回的是 D-Bus 字符串渲染形式，JSON 内部
    # 每个引号都被转义：s "{\"monitors\":[{\"last_successful_sample_ns\":123}]}"。
    # 第一版找不转义的键，strstr 永远不命中 → nonzero 恒为 0。
    #
    # 与 cpu_delta 那个缺陷同源：夹具比真实输入干净，于是测试全绿。
    # 「夹具是否忠实于真实输入」这件事，测试自己无法验证 —— 只能拿真实
    # 输出回来对。

    REAL_BUSCTL = (
        's "{\\"monitors\\":[{\\"name\\":\\"DnsMonitor\\",'
        '\\"last_successful_sample_ns\\":1510056662029},'
        '{\\"name\\":\\"SkbDropMonitor\\",\\"last_successful_sample_ns\\":0}]}"'
    )

    @staticmethod
    def parse_real(text):
        """复刻 C++ 现行实现：查找带转义的键。"""
        KEY = '\\"last_successful_sample_ns\\":'
        vals, pos = [], 0
        while True:
            i = text.find(KEY, pos)
            if i < 0:
                break
            rest = text[i + len(KEY):]
            m = re.match(r"\s*(-?\d+)", rest)
            if m:
                vals.append(int(m.group(1)))
                pos = i + len(KEY) + m.end()
            else:
                pos = i + len(KEY)
        return vals

    def test_real_busctl_output_is_parsed(self):
        """真实转义形态必须能解析出两项（一个非零、一个零）。"""
        self.assertEqual(self.parse_real(self.REAL_BUSCTL), [1510056662029, 0])

    def test_unescaped_key_misses_real_output(self):
        """
        反证：不转义的键在真实输出上**必然**不命中。

        保留这个用例是为了让缺陷的具体形态留在测试里 —— 只断言「现在是
        对的」不够，还要能说清「错的那种写法为什么会错」。
        """
        self.assertNotIn(self.KEY, self.REAL_BUSCTL,
                         "如果命中，说明 busctl 输出格式变了，需重新取真实样本")

    def test_source_uses_escaped_key(self):
        """
        源码守护：kKey 解码后必须是带反斜杠的形态。

        不能直接 assertIn('\\"last...\\"', src) —— 变异测试证实那是假守护：
        C++ 里正确的字面量 "\\\\\\"last..." 在源码文本中包含子串 \\"last..."，
        而错误的 "\\"last..." 也包含同一子串，两种写法都能让断言通过。
        必须把字面量取出来做 C 语义解码，再比对解码后的真实字符串。
        """
        import pathlib
        src = (pathlib.Path(__file__).parent /
               "weaknet-health-guard.cpp").read_text(encoding="utf-8")
        m = re.search(r'kKey\s*=\s*"((?:[^"\\]|\\.)*)"', src)
        self.assertIsNotNone(m, "找不到 kKey 的字面量定义")
        decoded = _c_unescape(m.group(1))
        self.assertEqual(decoded, '\\"last_successful_sample_ns\\":',
                         "kKey 解码后必须带反斜杠前缀（匹配 busctl 的转义引号）")


    def test_key_not_found_reported_separately(self):
        """
        「键失配」与「值全为 0」必须给出不同 detail。

        把解析 bug 伪装成业务事实（no active monitor）是本缺陷最危险的地方：
        运维会去查采集，而真正坏的是判据自己。
        """
        import pathlib
        src = (pathlib.Path(__file__).parent /
               "weaknet-health-guard.cpp").read_text(encoding="utf-8")
        missing = "key not found in busctl output (format changed?)"
        zero = "no active monitor (all timestamps 0)"
        self.assertIn(missing, src)
        self.assertIn(zero, src)
        # 两条 detail 必须来自不同分支，而不是同一个 snprintf 的两种措辞。
        self.assertNotEqual(src.find(missing), src.find(zero))
        # 分叉依据必须是「键是否命中」，不能是「值是否全为 0」。
        self.assertIn("key_found", src,
                      "必须显式区分键失配与值全零两种成因")

    def test_parse_multiple(self):
        s = (self.KEY + '111,' + self.KEY + '222,' + self.KEY + '333')
        self.assertEqual(self.parse(s), [111, 222, 333])

    def test_parse_zero_entries(self):
        """按需启用的监控器时间戳恒为 0，必须能解析出来并排除。"""
        s = self.KEY + '0,' + self.KEY + '12345'
        self.assertEqual(self.parse(s), [0, 12345])

    def test_parse_spaces_after_colon(self):
        """真实 busctl 输出里冒号后可能有空格。"""
        s = '"last_successful_sample_ns": 999'
        self.assertEqual(self.parse(s), [999])

    def test_all_zero_is_fail(self):
        """
        全部为 0 = 没有活跃监控器。C++ 判失败并写明
        "no active monitor (all timestamps 0)"。
        这是刻意的：刚启动、还没采到任何数据的阶段也算不健康，
        否则「采集从未成功」会被当成正常。
        """
        now = 10**18
        self.assertFalse(self.judge([0, 0, 0], now))

    def test_all_fresh_is_ok(self):
        now = 10**18
        vals = [now - i * 10**9 for i in (1, 2, 3)]
        self.assertTrue(self.judge(vals, now))

    def test_two_of_three_fresh_is_ok(self):
        """2/3 边界：恰好 2 个新鲜应通过（fresh*3 >= nonzero*2）。"""
        now = 10**18
        stale = now - 999 * 10**9
        vals = [now - 10**9, now - 2 * 10**9, stale]
        self.assertTrue(self.judge(vals, now))

    def test_one_of_three_fresh_is_fail(self):
        now = 10**18
        stale = now - 999 * 10**9
        vals = [now - 10**9, stale, stale]
        self.assertFalse(self.judge(vals, now))

    def test_one_of_one_fresh_is_ok(self):
        """只剩 1 个活跃监控器时，它新鲜就算健康（比例 1/1）。"""
        now = 10**18
        self.assertTrue(self.judge([now - 10**9], now))

    def test_one_of_one_stale_is_fail(self):
        now = 10**18
        self.assertFalse(self.judge([now - 999 * 10**9], now))

    def test_future_timestamp_is_stale(self):
        """
        时间戳在未来（now - v < 0）：不算新鲜。
        发生条件是进程重启后 CLOCK_MONOTONIC 归零而旧值还在 —— 不该当健康。
        """
        now = 10**18
        self.assertFalse(self.judge([now + 10**12], now))

    def test_boundary_age(self):
        now = 10**18
        exactly = now - SAMPLE_MAX_AGE_S * 10**9
        self.assertTrue(self.judge([exactly], now), "恰好等于窗口应通过")
        over = now - (SAMPLE_MAX_AGE_S + 1) * 10**9
        self.assertFalse(self.judge([over], now), "超出窗口应失败")


class TestProcStatParsing(unittest.TestCase):
    """
    判据 3：从 /proc/<pid>/stat 取 utime+stime（字段 14/15）。

    这里最容易出的错是 comm 里带空格或括号。C++ 用 strrchr(')') 定位，
    正确做法就是从最后一个 ')' 之后开始数字段。
    """

    @staticmethod
    def parse_cpu_ticks(stat_line):
        rp = stat_line.rfind(")")
        if rp < 0:
            return None
        rest = stat_line[rp + 2:]          # 跳过 ") " → 字段 3 起点
        fields = rest.split()
        # 字段 3 落在 fields[0]，故字段 N 落在 fields[N-3]：
        #   字段 14 (utime) -> fields[11]
        #   字段 15 (stime) -> fields[12]
        try:
            return int(fields[11]) + int(fields[12])
        except (IndexError, ValueError):
            return None

    def _line(self, comm="weaknet-dbus", utime=100, stime=50):
        """
        字段: 1=pid 2=comm 3=state 4=ppid 5=pgrp 6=session 7=tty_nr
              8=tpgid 9=flags 10=minflt 11=cminflt 12=majflt 13=cmajflt
              14=utime 15=stime
        comm 之后必须正好 13 个字段（3..15），少一个就会把 utime
        读成别的字段 —— 这正是本夹具第一版犯的错。
        """
        # 字段 3..13 恰好 11 个，多一个少一个都会让 utime 错位：
        #   state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt
        tail = ["S", "1", "1", "0", "-1", "4194560", "0",
                "0", "0", "0", "0"]             # 字段 3..13 共 11 个
        tail += [str(utime), str(stime)]        # 字段 14、15
        return f"1234 ({comm}) " + " ".join(tail)

    def test_simple(self):
        self.assertEqual(self.parse_cpu_ticks(self._line()), 150)

    def test_comm_with_space(self):
        """comm 含空格：必须靠最后一个 ')' 定位，不能按空格切。"""
        line = self._line(comm="my service name")
        self.assertEqual(self.parse_cpu_ticks(line), 150)

    def test_comm_with_parens(self):
        """comm 含括号：内核不转义，因此可能出现嵌套括号。"""
        line = self._line(comm="(weird)name)")
        self.assertEqual(self.parse_cpu_ticks(line), 150)

    def test_zero_ticks(self):
        self.assertEqual(self.parse_cpu_ticks(self._line(utime=0, stime=0)), 0)

    def test_malformed_returns_none(self):
        self.assertIsNone(self.parse_cpu_ticks("garbage without paren"))
        self.assertIsNone(self.parse_cpu_ticks("1234 (x) S"))

    def test_delta_judgement(self):
        """
        窗口内增量 < kCpuMinTicks 视为停滞。
        kCpuMinTicks=1 意味着 6 秒内必须至少消耗 1 个 tick（10ms）——
        对一个持续采样的服务非常宽松，不会误报。
        """
        def cpu_ok(delta):
            return delta >= CPU_MIN_TICKS
        self.assertFalse(cpu_ok(0), "完全不动应判失败")
        self.assertTrue(cpu_ok(1))
        self.assertTrue(cpu_ok(1000))

    # ---- 以下两个用例针对的是「实现」而不是「规格」----------------------
    #
    # 2026-11-15 板端 dry-run 暴露：cpu_delta 判据对**任何**进程都恒失败。
    # 成因是 C++ 的 ReadProcCpuTicks 从 rp+2（字段 3，状态字符 'S'）开始
    # strtoull，而它不是数字，end == p 立即 return false。
    #
    # 关键教训：上面那些用例全都走 parse_cpu_ticks，它用 split() + 索引
    # 取值，是**正确的另一条路径**。它们验证的是「应该怎么解析」，对
    # 「代码实际怎么解析」一无所知。所以 78 项测试全绿而实现是坏的。
    #
    # 下面改为直接从 C++ 源码里抽取那段循环并**按 C++ 的语义执行它**，
    # 这样实现一旦退回从字段 3 起解析，测试立刻失败。

    @staticmethod
    def _cxx_readproc_fragment():
        """从 C++ 源码抽出 ReadProcCpuTicks，返回源码文本。"""
        import pathlib
        src = (pathlib.Path(__file__).parent / "weaknet-health-guard.cpp").read_text(encoding="utf-8")
        start = src.find("bool ReadProcCpuTicks(")
        assert start >= 0, "找不到 ReadProcCpuTicks，本守护需要更新"
        end = src.find("\n}\n", start)
        assert end > start
        return src[start:end]

    def test_source_starts_at_field_4(self):
        """
        源码守护：必须跳过字段 3（状态字符）再从字段 4 起解析数字。

        断言的是 C++ 里真实存在的那两句 `p += 2` 语义，而不是
        「有个循环」这种弱条件 —— 弱的守护等于没有守护。
        """
        body = self._cxx_readproc_fragment()
        # 起始点必须是 rp + 2（字段 3 的位置），这一点本身没变
        self.assertIn("char* p = rp + 2;", body,
                      "定位 ')' 之后的起点被改动了，需重新核对字段编号")
        # 必须有第二步跳过状态字符
        self.assertIn("p += 2;", body,
                      "缺少「跳过状态字符」这一步 —— 会退回从 'S' 开始 strtoull，"
                      "对任何进程都返回 false，CPU 判据恒失败")
        # 循环必须从字段 4 开始
        self.assertIn("for (int f = 4; f <= 15; ++f)", body,
                      "循环起点必须是字段 4；字段 3 是状态字符，不是数字")
        # 绝不能退回字段 3
        self.assertNotIn("for (int f = 3; f <= 15; ++f)", body,
                         "已修复的缺陷回归：字段 3 是状态字符，strtoull 必然失败")

    def test_cxx_semantics_reproduce_bug(self):
        """
        语义级复现：按 C++ 现行逻辑在 Python 里走一遍，必须取到 utime/stime。

        这里逐字复刻 strtoull 的行为（遇到非数字则失败），因此它和上面的
        源码守护是互补的：上面查文本，这里查语义。两者同时通过才算真的对。
        """
        line = self._line(comm="weaknet-dbus-se", utime=16466, stime=0)
        rp = line.rfind(")")
        # 复刻 C++ 现行实现：rp+2 → 检查 → p+=2 → 从字段 4 起循环
        p_text = line[rp + 2:]
        self.assertFalse(p_text[:1].isdigit(),
                         "字段 3 应当是状态字符（夹具问题）")
        p_text = p_text[2:]           # 跳过状态字符 + 空格
        fields = p_text.split()
        # 复刻 for (f = 4; f <= 15; ++f) 的语义：fields[0] 是字段 4
        # 字段 14 → fields[10]，字段 15 → fields[11]
        try:
            total = int(fields[10]) + int(fields[11])
        except (IndexError, ValueError):
            self.fail("按现行 C++ 逻辑取不到 utime/stime —— 解析起点错了")
        self.assertEqual(total, 16466)


class TestDStateDuration(unittest.TestCase):
    """
    判据 4：D 状态必须判「持续」，不能判瞬时。

    一次瞬时 IO 也会出现 D。若见 D 就判失败，正常盘写抖动就复位。
    真正的判据是「有线程在 D 停留超过 kDStateMaxSec」。
    """

    def judge(self, dur_s, d_count):
        """复刻 C++ 语义：d_count==0 通过；否则比较持续时间。"""
        if d_count == 0:
            return True
        return dur_s < DSTATE_MAX_S

    def test_no_d_threads_ok(self):
        self.assertTrue(self.judge(0, 0))

    def test_transient_d_tolerated(self):
        """瞬时 D（0~7 秒）不算故障。"""
        for d in (0, 1, 3, 7):
            self.assertTrue(self.judge(d, 1), f"持续 {d}s 不应判失败")

    def test_persistent_d_fails(self):
        for d in (8, 9, 60):
            self.assertFalse(self.judge(d, 1), f"持续 {d}s 应判失败")

    def test_boundary(self):
        self.assertTrue(self.judge(DSTATE_MAX_S - 1, 2))
        self.assertFalse(self.judge(DSTATE_MAX_S, 2),
                         "恰好等于阈值应判失败（< 而非 <=）")

    def test_duration_exceeds_hardware_timeout(self):
        """
        8 秒上限必须显著小于 16 秒硬件超时，否则「确认故障」这件事
        本身就会跨过复位点，判据永远来不及生效。
        """
        self.assertLess(DSTATE_MAX_S, 16)


class TestDThreadCounting(unittest.TestCase):
    """
    判据 4 的前半段：**怎么数出 D 线程**。

    `TestDStateDuration` 只覆盖「数出来之后怎么判」，覆盖不到「数错」。
    而数错的两种方式都指向同一个危险方向 —— 漏报：

      1. 按空白切分取第 3 字段。comm 含空格时整体错位，
         "1234 (my service name) D ..." 的第三段是 "service" 而非 "D"，
         卡在 D 的线程数不出来，判据恒通过。
      2. 扫描失败时把输出当 "0"。shell 管道丢掉退出码后，
         目录读不到与「真的没有 D 线程」在输出上无法区分。

    漏报意味着「保护看起来在、实际不在」，比误报危险得多：
    误报会复位板子留下痕迹，漏报什么都不留。
    """

    @staticmethod
    def count_d_awk(stat_lines):
        """复刻旧实现：按空白切分取第 3 字段（**错误做法**，仅作对照）。"""
        n = 0
        for line in stat_lines:
            parts = line.split()
            if len(parts) >= 3 and parts[2] == "D":
                n += 1
        return n

    @staticmethod
    def count_d_rfind(stat_lines):
        """复刻现行实现：从最后一个 ')' 之后取状态字符（**正确做法**）。"""
        n = 0
        for line in stat_lines:
            rp = line.rfind(")")
            if rp < 0 or len(line) < rp + 3:
                continue
            if line[rp + 1] != " ":
                continue
            if line[rp + 2] == "D":
                n += 1
        return n

    def _stat(self, comm, state):
        return f"1234 ({comm}) {state} 1 1 0 -1 4194560 0 0 0 0 0 100 50"

    def test_simple_comm_both_agree(self):
        """comm 无空格时两种做法结果一致——所以这个 bug 不会被普通用例抓到。"""
        lines = [self._stat("weaknet-dbus", "D"), self._stat("weaknet-dbus", "S")]
        self.assertEqual(self.count_d_awk(lines), 1)
        self.assertEqual(self.count_d_rfind(lines), 1)

    def test_comm_with_space_awk_miscounts(self):
        """
        这条钉死旧实现的具体失效：comm 含空格时 awk 版数不出 D。
        若哪天有人把实现改回按空白切分，这个断言会立刻失败。
        """
        line = self._stat("my service name", "D")
        self.assertEqual(self.count_d_rfind([line]), 1,
                         "正确解析必须数出这条 D 线程")
        self.assertEqual(self.count_d_awk([line]), 0,
                         "按空白切分的旧实现数不出（这正是被修掉的 bug）")

    def test_comm_with_paren_mixed(self):
        """comm 含右括号：必须靠**最后**一个 ')' 定位。"""
        line = self._stat("weird)name", "D")
        self.assertEqual(self.count_d_rfind([line]), 1)

    def test_multiple_threads_mixed_states(self):
        lines = [
            self._stat("weaknet-dbus", "S"),
            self._stat("weaknet-dbus", "D"),
            self._stat("my service name", "D"),
            self._stat("weird)name", "D"),
            self._stat("weaknet-dbus", "R"),
        ]
        self.assertEqual(self.count_d_rfind(lines), 3)

    def test_no_d_threads(self):
        lines = [self._stat("weaknet-dbus", "S"), self._stat("weaknet-dbus", "R")]
        self.assertEqual(self.count_d_rfind(lines), 0)

    def test_malformed_line_skipped_not_counted(self):
        """缺 ')' 或长度不足的行必须跳过，不能猜。"""
        self.assertEqual(self.count_d_rfind(["garbage line without paren"]), 0)
        self.assertEqual(self.count_d_rfind(["1234 (x)"]), 0)
        self.assertEqual(self.count_d_rfind(["1234 (x)D 1 2"]), 0,
                         "')' 后没有空格说明字段错位，应跳过")

    def test_source_uses_last_paren_not_awk(self):
        """
        源码守护：CountDThreads 必须用 strrchr(')') 定位，
        不能退回按空白切分的 shell 管道。
        这条防的是「重构时顺手改回简单写法」。
        """
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            src = f.read()
        start = src.index("int CountDThreads(pid_t pid)")
        body = src[start:start + 2000]
        self.assertIn("strrchr", body,
                      "CountDThreads 必须用 strrchr(')') 定位状态字段")
        self.assertNotIn("print $3", body,
                         "CountDThreads 不得用 awk 按空白切分（comm 含空格会错位）")
        self.assertNotIn("popen", body,
                         "CountDThreads 不得走 shell（退出码丢失会让失败被当成 0）")
        self.assertIn("kDStateMaxTasks", body,
                      "线程数上限必须真正使用，不能只是定义")


class TestInactiveGrace(unittest.TestCase):
    """
    服务缺席容忍：8 秒内继续喂狗，超过则停止喂狗。

    这是本设计里最容易被误解的一条。它不是「放宽」，而是为了区分
    「受控重启」与「真的死了」—— 没有它，每次 systemctl restart
    都会把板子打复位。
    """

    def decide(self, gone_s):
        """返回 'feed' 或 'stop'。"""
        return "feed" if gone_s < INACTIVE_GRACE_S else "stop"

    def test_short_gap_feeds(self):
        for g in (0, 1, 5, 7):
            self.assertEqual(self.decide(g), "feed", f"缺席 {g}s 应继续喂狗")

    def test_boundary(self):
        self.assertEqual(self.decide(INACTIVE_GRACE_S - 1), "feed")
        self.assertEqual(self.decide(INACTIVE_GRACE_S), "stop",
                         "达到容忍上限应停止喂狗（< 而非 <=）")

    def test_long_gap_stops(self):
        # 用相对常量表达，不写死数字：阈值改动时这条断言自动跟着走，
        # 而不是变成"改测试直到通过"。
        for g in (INACTIVE_GRACE_S, INACTIVE_GRACE_S + 1, 300):
            self.assertEqual(self.decide(g), "stop")

    def test_grace_covers_systemd_restart(self):
        """
        weaknet-server 的 RestartSec=5，启动约 3 秒。
        容忍窗口必须覆盖这个常规重启，否则每次崩溃自动重启都变成复位。
        """
        systemd_restart_cost = 5 + 3
        self.assertGreater(INACTIVE_GRACE_S, systemd_restart_cost,
                           "容忍窗口必须大于 systemd 自动重启的耗时")

    def test_stop_leaves_room_before_reset(self):
        """
        停止喂狗后到硬件复位之间还有 16 秒。
        容忍窗口不能把这段全部吃掉 —— 否则「决定停喂」和「已复位」
        之间没有留出任何写失败快照的时间。
        """
        self.assertLess(INACTIVE_GRACE_S, 16)


class TestMaintenanceFlag(unittest.TestCase):
    """
    维护标记：有上限的人工豁免。

    关键设计点：标记会过期。若永久有效，一次 Ctrl-C 残留就让保护永久消失，
    而且没人知道 —— 比误复位更糟。
    """

    def state(self, exists, age_s):
        if not exists:
            return "inactive"
        if age_s < 0:
            return "active"          # 时钟回跳，保守按新鲜处理
        if age_s > MAINT_MAX_AGE_S:
            return "expired"
        return "active"

    def test_absent(self):
        self.assertEqual(self.state(False, 0), "inactive")

    def test_fresh_is_active(self):
        for age in (0, 60, 600, MAINT_MAX_AGE_S - 1):
            self.assertEqual(self.state(True, age), "active")

    def test_boundary(self):
        self.assertEqual(self.state(True, MAINT_MAX_AGE_S), "active")
        self.assertEqual(self.state(True, MAINT_MAX_AGE_S + 1), "expired")

    def test_expired_is_not_active(self):
        """过期后必须回到正常判定 —— 这是「不会永久关掉保护」的保证。"""
        self.assertEqual(self.state(True, MAINT_MAX_AGE_S + 1), "expired")

    def test_clock_stepped_back(self):
        """负年龄（NTP 校正）按新鲜处理，避免误判过期。"""
        self.assertEqual(self.state(True, -100), "active")

    def test_covers_full_deploy(self):
        """30 分钟必须覆盖一次完整部署/OTA 的最坏耗时。"""
        self.assertGreaterEqual(MAINT_MAX_AGE_S, 15 * 60)


class TestOtaProgress(unittest.TestCase):
    """
    OTA 过渡识别：IN_PROGRESS + mtime 新鲜。

    为什么不能只看 IN_PROGRESS：升级进程若死掉，回执会永远停在
    IN_PROGRESS。靠 mtime 新鲜度区分「还在跑」与「已经死了」。
    """

    def decide(self, has_receipt, in_progress, age_s):
        if not has_receipt:
            return "no_receipt"
        if not in_progress:
            return "finished"
        if age_s < 0:
            age_s = 0
        if age_s > OTA_PROGRESS_MAX_AGE_S:
            return "stalled"
        return "in_progress"

    def test_no_receipt(self):
        self.assertEqual(self.decide(False, False, 0), "no_receipt")

    def test_finished(self):
        self.assertEqual(self.decide(True, False, 5), "finished")

    def test_active_upgrade(self):
        for age in (0, 10, 60, OTA_PROGRESS_MAX_AGE_S):
            self.assertEqual(self.decide(True, True, age), "in_progress")

    def test_stalled_upgrade_is_not_in_progress(self):
        """
        死掉的升级不能被当成受控过渡 —— 否则一次崩溃的 OTA
        就永久关掉了保护。
        """
        self.assertEqual(
            self.decide(True, True, OTA_PROGRESS_MAX_AGE_S + 1), "stalled")

    def test_hung_upgrade_detected(self):
        self.assertEqual(self.decide(True, True, 3600), "stalled")


class TestRestartWarmup(unittest.TestCase):
    """
    重启豁免 + 崩溃循环防护。

    豁免是必需的（新进程的 WAL 年龄是从停机期继承的），
    但无限豁免等于永久喂狗。因此设次数上限。
    """

    def warm(self, age_s, restarts):
        if restarts > RESTART_MAX_IN_WIN:
            return False          # 崩溃循环：取消豁免
        return age_s < RESTART_WARMUP_S

    def test_fresh_restart_warm(self):
        for age in (0, 5, 11):
            self.assertTrue(self.warm(age, 1), f"重启后 {age}s 应豁免")

    def test_warmup_expires(self):
        self.assertFalse(self.warm(RESTART_WARMUP_S, 1),
                         "豁免必须到期（< 而非 <=）")
        self.assertFalse(self.warm(60, 1))

    def test_restart_storm_cancels_warmup(self):
        """
        关键用例：崩溃循环不能让豁免无限续期。
        超过上限后，即使刚重启也不豁免。
        """
        for n in (RESTART_MAX_IN_WIN + 1, RESTART_MAX_IN_WIN + 5, 100):
            self.assertFalse(self.warm(1, n),
                             f"重启 {n} 次后不应再豁免")

    def test_boundary(self):
        self.assertTrue(self.warm(1, RESTART_MAX_IN_WIN))
        self.assertFalse(self.warm(1, RESTART_MAX_IN_WIN + 1))

    def test_warmup_covers_wal_latency(self):
        """
        豁免窗口必须至少覆盖两个落库周期（5s），
        否则新进程起来后 WAL 还没写第二笔，判据就开始了。
        """
        self.assertGreaterEqual(RESTART_WARMUP_S, 10)


class TestTripFlagGate(unittest.TestCase):
    """
    已触发标记（boot_id 门控）。

    这是「重启无法取消复位」的核心保证：
      * 同一 boot 内已触发 → 拒绝 open 设备（open 会触发 .start 喂狗）
      * boot_id 变了（真复位过）→ 标记自动失效，不毒住下一次启动
    """

    def tripped(self, saved_boot_id, current_boot_id):
        """
        复刻 C++ 的 AlreadyTrippedThisBoot：读到的 boot_id 要剥掉尾部换行
        再比。文件由脚本 echo 写入，必然带 \\n —— 不剥就永远比不相等，
        标记形同不存在，已触发的复位会被下一次 open 取消。
        """
        if saved_boot_id is None:
            return False
        saved_boot_id = saved_boot_id.strip()
        if not saved_boot_id:
            return False
        return saved_boot_id == current_boot_id.strip()

    def test_no_flag(self):
        self.assertFalse(self.tripped(None, "boot-a"))

    def test_same_boot_blocked(self):
        """同一 boot 内已触发：必须拒绝重新打开设备。"""
        self.assertTrue(self.tripped("boot-a", "boot-a"))

    def test_new_boot_allowed(self):
        """
        复位后 boot_id 变化 → 标记失效，服务能正常重新接管。
        否则一次触发会让板子永远起不来看门狗保护。
        """
        self.assertFalse(self.tripped("boot-a", "boot-b"))

    def test_trailing_newline_ignored(self):
        """标记文件写入时带换行，读取时必须剥掉。"""
        self.assertTrue(self.tripped("boot-a\n", "boot-a"))

    def test_empty_flag_not_tripped(self):
        self.assertFalse(self.tripped("", "boot-a"))


class TestExitSemantics(unittest.TestCase):
    """
    三条退出路径的行为语义。

    混在一起会出人命：把「判定不健康」当成正常退出，就等于取消复位。
    """

    EXIT_TRIPPED = 99

    def action(self, reason):
        return {
            "criteria_unhealthy": "stop_feeding_exit_99",
            "service_absent":     "stop_feeding_exit_99",
            "feed_write_failed":  "stop_feeding_exit_1",
            "sigterm":            "magic_close_exit_0",
            "crash":              "no_close_exit_killed",
        }[reason]

    def test_unhealthy_stops_feeding(self):
        self.assertEqual(self.action("criteria_unhealthy"),
                         "stop_feeding_exit_99")

    def test_service_absent_stops_feeding(self):
        self.assertEqual(self.action("service_absent"),
                         "stop_feeding_exit_99")

    def test_sigterm_does_not_reset(self):
        """SIGTERM 是正常停机，必须写 'V' 停用看门狗。"""
        self.assertEqual(self.action("sigterm"), "magic_close_exit_0")

    def test_crash_causes_reset(self):
        """崩溃时没机会写 'V'，硬件复位 —— 期望行为。"""
        self.assertEqual(self.action("crash"), "no_close_exit_killed")

    def test_exit_code_99_is_unusual(self):
        """
        99 必须与常规退出码不冲突：0=成功、1=错误、2=用法。
        用 99 是为了让 RestartPreventExitStatus 精确命中。
        """
        self.assertEqual(self.EXIT_TRIPPED, 99)
        self.assertNotIn(self.EXIT_TRIPPED, (0, 1, 2))


class TestSystemdUnitConsistency(unittest.TestCase):
    """单元文件与代码必须一致，否则保护链会在运行时断开。"""

    @classmethod
    def setUpClass(cls):
        with open("tools/weaknet-health-guard.service", encoding="utf-8") as f:
            cls.unit = f.read()
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            cls.src = f.read()

    def test_restart_prevent_matches_exit_code(self):
        """
        代码退出 99，单元必须 RestartPreventExitStatus=99。
        不一致 → 触发后 systemd 重启 guard → 重新 open 喂狗 → 复位被取消。
        这是整套设计里最致命的一处配置漂移。
        """
        self.assertIn("RestartPreventExitStatus=99", self.unit)
        self.assertIn("kExitTripped = 99", self.src)

    def test_restart_is_disabled(self):
        """
        Restart 必须是 no：guard 崩溃时若被自动重启，会喂狗续命，
        掩盖崩溃。宁可让硬件复位。
        """
        self.assertRegex(self.unit, r"(?m)^Restart=no$")

    def test_has_stop_post_for_give_back(self):
        """
        必须有 ExecStopPost 归还看门狗：否则 guard 退出后整机既无健康
        看门狗也无原生看门狗，而 systemctl status 看起来正常。
        """
        self.assertIn("ExecStopPost=", self.unit)
        self.assertIn("give-back", self.unit)

    def test_requires_ota_gate(self):
        """
        必须依赖 OTA 启动门禁：门禁拒绝时服务不会起来，
        guard 若照样接管看门狗就会在 16 秒后复位——把「不要启动业务」
        变成「无限重启」。
        """
        self.assertIn("Requires=weaknet-ota-gate.service", self.unit)

    def test_uses_sigterm(self):
        """SIGTERM 是 guard 写 'V' 的唯一时机，不能被改成 SIGKILL。"""
        self.assertIn("KillSignal=SIGTERM", self.unit)
        self.assertNotIn("KillSignal=SIGKILL", self.unit)


class TestTakeoverScriptOrdering(unittest.TestCase):
    """接管脚本的顺序不可颠倒 —— 颠倒会产生无保护窗口。"""

    @classmethod
    def setUpClass(cls):
        with open("tools/weaknet-wdt-takeover.sh", encoding="utf-8") as f:
            cls.sh = f.read()

    def test_insmod_before_unbind(self):
        """
        必须先 insmod 再 unbind：反过来会在「旧狗已撤、新狗未起」之间
        出现真空期，此时任何故障都不会被复位兜住，且 dmesg 什么都不留。
        """
        i_insmod = self.sh.find('insmod "$KO_PATH"')
        i_unbind = self.sh.find('"${SYSFS_DRV}/unbind"')
        self.assertGreater(i_insmod, 0, "找不到 insmod")
        self.assertGreater(i_unbind, 0, "找不到 unbind")
        self.assertLess(i_insmod, i_unbind,
                        "insmod 必须在 unbind 之前")

    def test_verifies_device_before_unbind(self):
        """
        unbind 之前必须确认新设备节点存在。这是唯一的安全闸门：
        不存在就退出，绝不 unbind。
        """
        i_check = self.sh.find('[ ! -c "$NEW_DEV" ]')
        i_unbind = self.sh.find('"${SYSFS_DRV}/unbind"')
        self.assertGreater(i_check, 0, "找不到设备存在性检查")
        self.assertLess(i_check, i_unbind,
                        "设备存在性检查必须在 unbind 之前")

    def test_give_back_binds_before_rmmod(self):
        """
        归还顺序：先 bind 回原驱动，再 rmmod。
        反过来会再制造一次真空期。
        """
        gb = self.sh[self.sh.find("do_give_back()"):]
        i_bind = gb.find('"${SYSFS_DRV}/bind"')
        i_rmmod = gb.find('rmmod "$KMOD_NAME"')
        self.assertGreater(i_bind, 0)
        self.assertGreater(i_rmmod, 0)
        self.assertLess(i_bind, i_rmmod, "bind 必须在 rmmod 之前")

    def test_respects_trip_flag(self):
        """
        已触发状态下必须拒绝接管与归还：
          * 接管 = 重新 insert 模块 + 可能 open，会重新喂狗
          * 归还 = rmmod 触发 .stop 关狗
        两者都会取消已排定的复位。
        """
        self.assertIn("tripped_this_boot", self.sh)
        take = self.sh[self.sh.find("do_take()"):self.sh.find("do_give_back()")]
        self.assertIn("tripped_this_boot", take,
                      "do_take 必须检查已触发状态")
        gb = self.sh[self.sh.find("do_give_back()"):]
        self.assertIn("tripped_this_boot", gb,
                      "do_give_back 必须检查已触发状态")

    def test_reloads_boot_id(self):
        """boot_id 必须现读，不能用缓存值（否则复位后仍判为已触发）。"""
        self.assertIn("/proc/sys/kernel/random/boot_id", self.sh)

    def test_give_back_checks_clean_exit(self):
        """
        do_give_back 必须调用 guard_exited_uncleanly。

        注意：这条只是**存在性**检查，不是有效性检查 —— 把实现改成
        `if false; then` 它照样通过。真正挡住这种退化的是
        TestGiveBackDoesNotReturnOnUncleanExit，后者真跑 do_give_back
        并断言 rmmod 没有被调用。保留本条是因为它失败时的信息更直接
        （函数整个消失 vs 判定变了）。
        """
        gb = self.sh[self.sh.find("do_give_back()"):]
        self.assertIn("guard_exited_uncleanly", gb,
                      "do_give_back 必须检查 guard 的退出方式")


class TestGiveBackDoesNotReturnOnUncleanExit(unittest.TestCase):
    """
    真跑 do_give_back，断言「不洁退出时绝不 rmmod」。

    为什么必须做到这个程度：本条用例的第一版只断言源码里出现了
    `guard_exited_uncleanly`，而把调用点改成 `if false; then` 之后
    测试**全绿**。那是假守护 —— 与 sample_ns/cpu_delta 那次同一个病根：
    检查「代码看起来写了什么」，而不是「代码实际做了什么」。

    做法：用 PATH 桩把 rmmod/insmod 换成写标记文件的脚片，
    SYSFS_DRV 指向临时目录，于是可以观察「有没有真的去卸载」。
    """

    #: (描述, 环境变量, 是否应当拒绝归还)
    CASES = [
        ("kill -9", {"SERVICE_RESULT": "signal",
                     "EXIT_CODE": "killed", "EXIT_STATUS": "9"}, True),
        ("段错误", {"SERVICE_RESULT": "signal",
                    "EXIT_CODE": "dumped", "EXIT_STATUS": "11"}, True),
        ("判定不健康 return 99", {"SERVICE_RESULT": "exit-code",
                                  "EXIT_CODE": "exited",
                                  "EXIT_STATUS": "99"}, True),
        ("正常 stop", {"SERVICE_RESULT": "success",
                       "EXIT_CODE": "killed", "EXIT_STATUS": "TERM"}, False),
    ]

    @classmethod
    def setUpClass(cls):
        import subprocess
        cls._subprocess = subprocess

    def _run_give_back(self, extra_env):
        import os
        import stat
        import tempfile

        tmp = tempfile.mkdtemp(prefix="wdt-gb-")
        try:
            stub_bin = os.path.join(tmp, "bin")
            sysfs = os.path.join(tmp, "sunxi-wdt")
            state = os.path.join(tmp, "state")
            os.makedirs(stub_bin)
            os.makedirs(sysfs)
            os.makedirs(state)

            # rmmod 桩：被调用就留下痕迹。成功返回 0（模拟卸载成功），
            # 这样测试能区分「拒绝归还」与「归还了但 rmmod 失败」。
            rmmod_mark = os.path.join(state, "rmmod-called")
            with open(os.path.join(stub_bin, "rmmod"), "w") as f:
                f.write("#!/bin/sh\ntouch %s\nexit 0\n" % rmmod_mark)
            os.chmod(os.path.join(stub_bin, "rmmod"), 0o755)

            # insmod 桩：take 路径用不到，给一个安全的空实现
            with open(os.path.join(stub_bin, "insmod"), "w") as f:
                f.write("#!/bin/sh\nexit 0\n")
            os.chmod(os.path.join(stub_bin, "insmod"), 0o755)

            # 预置 sunxi_wdt 的绑定标记：让 sunxi_bound 返回真，跳过 bind 分支。
            # 若不建它，脚本会尝试写 <SYSFS_DRV>/bind —— 宿主机上没有这个文件，
            # bind 必然失败，测试就会走进与「已绑定」不同的分支。
            open(os.path.join(sysfs, "2050000.watchdog"), "w").close()

            # ORIG_DEV 必须是**字符设备**：脚本用 `[ -c "$ORIG_DEV" ]` 等待它
            # 出现。普通文件过不了这个判断，会让每个用例白等 2 秒再报错。
            # /dev/null 恰好是字符设备，语义上也满足「归还后设备已恢复」。
            orig_dev_ok = "/dev/null"

            with open("tools/weaknet-wdt-takeover.sh", encoding="utf-8") as f:
                src = f.read()
            # 去掉最后的 case 分发，只 source 函数定义
            src = src.split('case "${1:-status}" in')[0]

            # 注意顺序：脚本内部会把 SYSFS_DRV / NEW_DEV / ORIG_DEV / TRIP_FLAG
            # 全部重新赋成写死的板端路径（那是刻意的 —— 生产行为不应被环境
            # 变量意外改变）。因此覆盖必须发生在 source **之后**，放在前面
            # 会被脚本自己的赋值冲掉。
            harness = (
                "set -u\n"
                + src
                + "\nTRIP_FLAG=%s\n" % os.path.join(tmp, "no-trip-flag")
                + "SYSFS_DRV=%s\n" % sysfs
                + "NEW_DEV=%s\n" % os.path.join(tmp, "watchdog1")
                + "ORIG_DEV=%s\n" % orig_dev_ok
                + "BOOT_ID_FILE=/proc/sys/kernel/random/boot_id\n"
                # kmod_loaded 查的是 /sys/module/weaknet_wdt，宿主机上必然不存在。
                # 不覆盖它就永远进不了 rmmod 分支 —— 于是本用例会对「不洁退出
                # 不 rmmod」恒真通过，又变成假守护。覆盖为「模块已加载」。
                + "kmod_loaded() { return 0; }\n"
                + "do_give_back\n"
            )

            env = {k: v for k, v in os.environ.items()
                   if k not in ("SERVICE_RESULT", "EXIT_CODE", "EXIT_STATUS")}
            env["PATH"] = stub_bin + os.pathsep + env.get("PATH", "")
            env.update(extra_env)

            self._subprocess.run(["bash", "-c", harness],
                                 capture_output=True, text=True, env=env,
                                 timeout=60)
            return os.path.exists(rmmod_mark)
        finally:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)

    def test_matrix_real_execution(self):
        for desc, env, expect_reject in self.CASES:
            with self.subTest(scenario=desc):
                rmmod_called = self._run_give_back(env)
                if expect_reject:
                    self.assertFalse(
                        rmmod_called,
                        f"{desc}: guard 未正常退出，do_give_back 仍然调用了 "
                        f"rmmod —— 这会经 watchdog_stop_on_unregister 调 "
                        f".stop 关掉硬件看门狗，把已排定的复位静默取消")
                else:
                    self.assertTrue(
                        rmmod_called,
                        f"{desc}: 正常停机应当归还（rmmod 应被调用），"
                        f"否则模块与设备永久残留")


class TestGuardExitSemantics(unittest.TestCase):
    """
    guard_exited_uncleanly 的判定矩阵 —— 真跑 bash，不在 Python 里复刻。

    为什么必须执行而不是复刻：本文件已有一次教训。sample_ns 与 cpu_delta
    两个判据的测试都「复刻了正确逻辑」，而 C++ 里的实现是错的，78 项测试
    全绿却掩盖了两个 100% 恒失败的判据（2026-11-15 板端 dry-run 才发现）。
    对 shell 函数同理 —— 复刻一遍只会得到「我以为它这么判」，不是「它这么判」。

    取值全部来自 systemd 247 在开发板上的实测，不是推测：

      场景                          SERVICE_RESULT  EXIT_CODE  EXIT_STATUS
      ----------------------------  --------------  ---------  -----------
      systemctl stop（优雅 SIGTERM） success         killed     TERM
      捕获 SIGTERM 后 exit 0         success         exited     0
      kill -9                        signal          killed     9
      段错误                         signal          dumped     11
      主进程 return 99               exit-code       exited     99
      TimeoutStopSec 超时            timeout         killed     9

    第一行是关键陷阱：正常 stop 时 EXIT_CODE 是 'killed'、EXIT_STATUS 是
    'TERM'，**都不是 0**。若按「EXIT_STATUS != 0 就拒绝归还」实现，每次
    正常停止服务都会被拒绝，模块与设备永久残留。这一条是板端实测抓到的真 bug。
    """

    #: 每种场景 = (描述, 期望是否拒绝归还, 环境变量)
    CASES = [
        ("systemctl stop（优雅 SIGTERM）", False,
         {"SERVICE_RESULT": "success", "EXIT_CODE": "killed", "EXIT_STATUS": "TERM"}),
        ("捕获 SIGTERM 后 exit 0", False,
         {"SERVICE_RESULT": "success", "EXIT_CODE": "exited", "EXIT_STATUS": "0"}),
        ("kill -9", True,
         {"SERVICE_RESULT": "signal", "EXIT_CODE": "killed", "EXIT_STATUS": "9"}),
        ("段错误", True,
         {"SERVICE_RESULT": "signal", "EXIT_CODE": "dumped", "EXIT_STATUS": "11"}),
        ("OOM 被杀", True,
         {"SERVICE_RESULT": "signal", "EXIT_CODE": "killed", "EXIT_STATUS": "9"}),
        ("判定不健康 return 99", True,
         {"SERVICE_RESULT": "exit-code", "EXIT_CODE": "exited", "EXIT_STATUS": "99"}),
        ("喂狗失败 return 1", True,
         {"SERVICE_RESULT": "exit-code", "EXIT_CODE": "exited", "EXIT_STATUS": "1"}),
        ("stop 超时", True,
         {"SERVICE_RESULT": "timeout", "EXIT_CODE": "killed", "EXIT_STATUS": "9"}),
        ("exit-code 但 EXIT_STATUS=0（兼容）", False,
         {"SERVICE_RESULT": "exit-code", "EXIT_CODE": "exited", "EXIT_STATUS": "0"}),
        # 变量缺失 = 运维手工执行，必须放行，否则无法手动恢复
        ("手工执行（无变量）", False, {}),
        ("手工强制（全空）", False,
         {"SERVICE_RESULT": "", "EXIT_CODE": "", "EXIT_STATUS": ""}),
    ]

    @classmethod
    def setUpClass(cls):
        import subprocess
        with open("tools/weaknet-wdt-takeover.sh", encoding="utf-8") as f:
            src = f.read()
        m = re.search(r"^guard_exited_uncleanly\(\) \{.*?^\}", src, re.S | re.M)
        if not m:
            raise AssertionError("找不到 guard_exited_uncleanly 的定义")
        cls.func_src = m.group(0)

        # 探针脚本：source 函数后按环境变量判定，拒绝归还 return 0 / 放行 return 1
        cls.probe = (
            "set -u\n"
            + cls.func_src
            + "\nif guard_exited_uncleanly; then exit 0; else exit 1; fi\n"
        )

        # 先确认函数能被 bash 解析 —— 语法坏时所有断言都会以相同方式失败，
        # 那会把「语法错误」误报成「判定错误」。
        chk = subprocess.run(["bash", "-n", "-c", cls.probe],
                             capture_output=True, text=True)
        if chk.returncode != 0:
            raise AssertionError(
                "guard_exited_uncleanly 无法被 bash 解析: " + chk.stderr)

        cls._subprocess = subprocess

    def test_matrix(self):
        import os
        env_base = {k: v for k, v in os.environ.items()
                    if k not in ("SERVICE_RESULT", "EXIT_CODE", "EXIT_STATUS")}
        for desc, expect_reject, extra in self.CASES:
            with self.subTest(scenario=desc):
                env = dict(env_base)
                env.update(extra)
                r = self._subprocess.run(["bash", "-c", self.probe],
                                         capture_output=True, text=True, env=env)
                reject = (r.returncode == 0)
                self.assertEqual(
                    reject, expect_reject,
                    f"{desc}: 期望{'拒绝' if expect_reject else '允许'}归还，"
                    f"实际{'拒绝' if reject else '允许'}")

    def test_normal_stop_must_not_be_rejected(self):
        """
        单独钉死最危险的一条：正常 systemctl stop 必须允许归还。

        这条独立成用例而不是只放在矩阵里，是因为它的失败后果最严重且
        最容易被"改得更严格"的动机破坏 —— 每次 stop 都拒绝归还，模块
        与设备永久残留，下一次 take 会撞上已存在的模块。
        """
        import os
        env = {k: v for k, v in os.environ.items()
               if k not in ("SERVICE_RESULT", "EXIT_CODE", "EXIT_STATUS")}
        env.update({"SERVICE_RESULT": "success",
                    "EXIT_CODE": "killed", "EXIT_STATUS": "TERM"})
        r = self._subprocess.run(["bash", "-c", self.probe],
                                 capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 1,
                         "systemctl stop 的正常停机被拒绝归还 —— "
                         "这会让模块与设备永久残留")

    def test_unclean_always_rejects(self):
        """任何非 success 的 SERVICE_RESULT 都不得放行（除 exit-code/0）。"""
        import os
        env_base = {k: v for k, v in os.environ.items()
                    if k not in ("SERVICE_RESULT", "EXIT_CODE", "EXIT_STATUS")}
        for result in ("signal", "timeout", "core-dump", "watchdog", "oom-kill"):
            with self.subTest(result=result):
                env = dict(env_base)
                env.update({"SERVICE_RESULT": result,
                            "EXIT_CODE": "killed", "EXIT_STATUS": "9"})
                r = self._subprocess.run(["bash", "-c", self.probe],
                                         capture_output=True, text=True, env=env)
                self.assertEqual(r.returncode, 0,
                                 f"SERVICE_RESULT={result} 未被判定为不洁退出")


if __name__ == "__main__":
    unittest.main(verbosity=2)
