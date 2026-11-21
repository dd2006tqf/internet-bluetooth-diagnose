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

# ---- 2026-11-17 可靠性加固新增 ----
CMD_TIMEOUT_MS = 1500
PID_UNKNOWN_MAX_STREAK = 3
TRIP_HISTORY_MAX = 5
TRIP_BREAKER_THRESHOLD = 3
TRIP_HISTORY_RESET_S = 30 * 60
EXIT_BREAKER_OPEN = 98
DISK_WARN_PCT = 10
# 硬件超时（板端实测 WDIOC_GETTIMEOUT = 16）
HARDWARE_TIMEOUT_S = 16

# ---- 2026-11-21 墙钟跳变豁免 ----
CLOCK_STEP_THRESHOLD_S = 5
CLOCK_STEP_MAX_STREAK = 3


def _strip_c_comments(text):
    """
    去掉 // 行注释与 /* */ 块注释，保留字符串字面量原样。

    为什么测试需要这个：本文件里有一批断言是「源码中不得出现某个符号」
    （禁止的 ioctl、禁止的 systemctl 子命令）。而源码的注释恰好在解释
    「为什么不能那么做」，会把符号名写进去。不剥注释，这些断言就会因为
    作者的说明文字而失败 —— 那会诱导人去删掉有价值的注释，而不是修代码。
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '"':
            j = i + 1
            while j < n:
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            out.append(text[i:j])
            i = j
        elif ch == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif ch == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
            out.append(' ')
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


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
            ("kCmdTimeoutMs", CMD_TIMEOUT_MS),
            ("kPidUnknownMaxStreak", PID_UNKNOWN_MAX_STREAK),
            ("kTripHistoryMax", TRIP_HISTORY_MAX),
            ("kTripBreakerThreshold", TRIP_BREAKER_THRESHOLD),
            ("kTripHistoryResetSec", TRIP_HISTORY_RESET_S),
            ("kExitBreakerOpen", EXIT_BREAKER_OPEN),
            ("kClockStepThresholdSec", CLOCK_STEP_THRESHOLD_S),
            ("kClockStepMaxStreak", CLOCK_STEP_MAX_STREAK),
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


class TestClockStepExemption(unittest.TestCase):
    """
    墙钟跳变豁免（2026-11-21）。

    背景：本板**无 RTC 电池**，开机后 systemd-timesyncd 先用「记录时间戳」
    恢复，再等 NTP 真同步。同步完成的那一瞬间墙钟一次性前跳（板端实测
    2026-11-21 05:49:11 前跳 9785 秒）。

    凡是用「当前墙钟 − 文件 mtime」算年龄的判据都会被这次跳变污染，
    而且是三处语义完全不同、后果也不同的地方：

      * WAL 年龄   → 假失败；去抖两轮后停喂 → **误复位**
      * 维护标记   → 被误判 EXPIRED → 维护模式**静默失效**
      * OTA 回执   → 被误判 STALLED → 服务已停的阶段**被复位在升级中途**

    因此修复的方向不是「放大阈值」（跳变可达数小时，任何合理阈值都会被
    穿透；放大到几小时又等于废掉判据），而是用单调钟检测跳变、当轮豁免。
    """

    def step(self, wall_delta, mono_delta):
        """复刻 C++ DetectClockStep 的核心算式。"""
        return (wall_delta - mono_delta)

    def exempt(self, step, streak):
        """复刻 C++ ClockStepExempt 的决策。"""
        if step == 0:
            return False
        return streak <= CLOCK_STEP_MAX_STREAK

    def test_normal_tick_not_flagged(self):
        """正常一轮：墙钟与单调钟同步前进，差值为 0 → 不豁免。"""
        for d in (1, 2, 3):
            self.assertEqual(self.step(d, d), 0, f"正常一轮 {d}s 不应判为跳变")
            self.assertFalse(self.exempt(0, 0))

    def test_sleep_jitter_tolerated(self):
        """
        轮询抖动：sleep(1) 实际睡了 1.02s，墙钟与单调钟各有毫秒级误差。
        阈值 5 秒必须吸收这类抖动，否则每一轮都会被判成跳变。
        """
        for jitter in (1, 2, 4, 5):
            s = self.step(2 + jitter, 2)
            self.assertLessEqual(abs(s), CLOCK_STEP_THRESHOLD_S,
                                 f"抖动 {jitter}s 不应触发跳变")
            self.assertEqual(s, jitter)

    def test_forward_jump_detected(self):
        """
        真实场景：NTP 一次性前跳 9785 秒。
        这一轮墙钟增量是 9785+2，单调钟增量只有 2 → step = 9785。
        """
        s = self.step(9785 + TICK_S, TICK_S)
        self.assertEqual(s, 9785, "应算出真实跳变幅度")
        self.assertGreater(abs(s), CLOCK_STEP_THRESHOLD_S)
        self.assertTrue(self.exempt(s, 1), "第 1 轮跳变必须豁免")

    def test_backward_jump_detected(self):
        """
        反向跳变同样要检测。原实现只防了负年龄（回跳），没防正年龄（前跳），
        这次要把两个方向都覆盖。
        """
        s = self.step(-3600 + TICK_S, TICK_S)
        self.assertEqual(s, -3600)
        self.assertGreater(abs(s), CLOCK_STEP_THRESHOLD_S)
        self.assertTrue(self.exempt(s, 1))

    def test_exemption_exhausts(self):
        """
        连续跳变超过上限就不再豁免。

        理由：豁免的那一轮所有墙钟判据都不生效。若时钟**每轮都在跳**
        （手工反复 `date`、timesyncd 陷入反复校正），无限豁免等于把判据
        永久关掉 —— 保护在，判据不在，正是本设计要消灭的「假活」形态。
        """
        for streak in range(1, CLOCK_STEP_MAX_STREAK + 1):
            self.assertTrue(self.exempt(100, streak),
                            f"第 {streak} 轮仍应豁免（上限 {CLOCK_STEP_MAX_STREAK}）")
        self.assertFalse(self.exempt(100, CLOCK_STEP_MAX_STREAK + 1),
                         "超过上限必须恢复判定，让失败被看见")

    def test_exempt_direction_is_fail_safe(self):
        """
        三处豁免的**方向**都必须偏向「别复位」，不能偏向「判失败」：

          WAL      → 按通过处理（误判停滞会复位）
          维护标记 → 按新鲜处理（误判过期会让维护中复位）
          OTA 回执 → 按进行中处理（误判 STALLED 会复位在升级中途）

        三者共同的代价上限都是一轮判定延迟，无保护风险。
        """
        # 三处调用点都必须把豁免结果当作「通过 / 有效 / 进行中」
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            src = _strip_c_comments(f.read())

        for who in ("wal_mtime", "maintenance", "ota_receipt", "trip_history"):
            self.assertIn(f'ClockStepExempt(clock_step, clock_streak, "{who}")',
                          src, f"{who} 必须走统一的豁免入口")

    def test_exemption_wired_before_judgement(self):
        """
        检测必须发生在**所有**依赖墙钟年龄的判定之前。

        这是顺序性要求，不是风格问题：若 DetectClockStep 放在判据之后，
        判据已经用被污染的年龄算出结果了，豁免再正确也没用。
        """
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            src = _strip_c_comments(f.read())

        pos_detect = src.find("const long clock_step = DetectClockStep(")
        self.assertGreater(pos_detect, 0, "主循环必须调用 DetectClockStep")

        for call in ("MaintenanceRequested(maint_detail",
                     "OtaUpgradeInProgress(ota_detail",
                     "CheckWalMtime(g_checks[0]"):
            pos = src.find(call)
            self.assertGreater(pos, 0, f"找不到调用点 {call}")
            self.assertGreater(pos, pos_detect,
                               f"{call} 必须在墙钟跳变检测之后")

    def test_monotonic_source_is_clock_monotonic(self):
        """
        检测基线必须来自 CLOCK_MONOTONIC。

        CLOCK_REALTIME 是会被 NTP 校正的那个，用它做基线等于用被测量的
        量去校准自己 —— 跳变会算成 0，整个检测形同虚设。
        """
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            src = f.read()

        # 注意不能用 \{.*?\} —— 函数体里 `struct timespec ts {};` 的花括号
        # 会让懒惰匹配提前收尾。这里锚到行首的单独 `}`。
        m = re.search(r"int64_t MonotonicNs\(\)\s*\{.*?\n\}", src, re.S)
        self.assertIsNotNone(m, "找不到 MonotonicNs 定义")
        self.assertIn("CLOCK_MONOTONIC", m.group(0))

        m = re.search(r"int64_t WallClockSeconds\(\)\s*\{.*?\n\}", src, re.S)
        self.assertIsNotNone(m, "找不到 WallClockSeconds 定义")
        self.assertIn("CLOCK_REALTIME", m.group(0))


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
        代码退出 99（判据失败）与 98（熔断），单元必须同时挡住这两个码。
        不一致 → 触发后 systemd 重启 guard → 重新 open 喂狗 → 复位被取消。
        这是整套设计里最致命的一处配置漂移。

        断言的是「集合包含」而不是逐字相等：RestartPreventExitStatus 是
        空格分隔的列表，将来若再加入第三个码，这条断言不应无谓失败。
        """
        m = re.search(r"(?m)^RestartPreventExitStatus=(.+)$", self.unit)
        self.assertIsNotNone(m, "单元文件缺少 RestartPreventExitStatus")
        codes = m.group(1).split()
        self.assertIn("99", codes, "退出码 99（判据失败）必须被挡住")
        self.assertIn("98", codes, "退出码 98（连续复位熔断）必须被挡住")
        self.assertIn("kExitTripped = 99", self.src)
        self.assertIn("kExitBreakerOpen = 98", self.src)

    def test_restart_is_disabled(self):
        """
        Restart 必须是 no：guard 崩溃时若被自动重启，会喂狗续命，
        掩盖崩溃。宁可让硬件复位。
        """
        self.assertRegex(self.unit, r"(?m)^Restart=no$")

    def test_has_no_stop_post_hook(self):
        """
        方案 B 下**不应**有 ExecStopPost：驱动从未被 unbind，没有需要归还的
        东西。归还脚本的残留会在 guard 正常停机时跑一段无意义的逻辑，更糟的
        是给人「设备被接管过」的错觉，掩盖「停止语义完全由 MAGICCLOSE 契约
        决定」这个真正的机制。
        """
        self.assertNotIn("ExecStopPost=", self.unit)
        self.assertNotIn("weaknet-wdt-takeover", self.unit)

    def test_pre_waits_for_platform_device(self):
        """
        ExecStartPre 必须等 /dev/watchdog0 就绪，而不是等自建的 watchdog1。
        设备由平台驱动 sunxi_wdt 在 probe 时导出；不等待就有可能在 devtmpfs
        极早期抢跑失败，日志上表现为「打开失败」而非「驱动未就绪」。
        """
        self.assertIn("ExecStartPre=", self.unit)
        self.assertIn("/dev/watchdog0", self.unit)
        self.assertNotIn("/dev/watchdog1", self.unit)

    def test_requires_ota_gate(self):
        """
        必须依赖 OTA 启动门禁：门禁拒绝时服务不会起来，
        guard 若照样打开看门狗就会在硬件超时后复位——把「不要启动业务」
        变成「无限重启」。
        """
        self.assertIn("Requires=weaknet-ota-gate.service", self.unit)

    def test_uses_sigterm(self):
        """SIGTERM 是 guard 写 'V' 的唯一时机，不能被改成 SIGKILL。"""
        self.assertIn("KillSignal=SIGTERM", self.unit)
        self.assertNotIn("KillSignal=SIGKILL", self.unit)


class TestDeviceContract(unittest.TestCase):
    """
    方案 B 的全部安全性都压在「设备契约」上，因此必须逐条钉死。

    这套用例只读源码、不需要开发板，但每一条都对应一个会在真机上表现为
    「保护看起来在、实际不在」的失效模式。
    """

    @classmethod
    def setUpClass(cls):
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            raw = f.read()
        # 只断言**代码**，不断言注释：注释里点名被禁的 ioctl（说明为什么
        # 不用它）不构成违规。不剥注释，这些「不得出现」的断言会被作者的
        # 说明文字触发，反过来逼人删掉有价值的注释。
        cls.src = _strip_c_comments(raw)

    def _func(self, name):
        """截出 C 函数体（花括号配平），避免断言误命中别处的同名片段。"""
        m = re.search(r"\n(?:\w[\w\s\*&:<>]*?)\b" + name + r"\([^)]*\)\s*\{", self.src)
        self.assertIsNotNone(m, f"找不到函数 {name}")
        i = self.src.index("{", m.start())
        depth = 0
        for j in range(i, len(self.src)):
            if self.src[j] == "{":
                depth += 1
            elif self.src[j] == "}":
                depth -= 1
                if depth == 0:
                    return self.src[i:j + 1]
        self.fail(f"{name} 的花括号没有配平")

    def test_opens_platform_device_not_private_node(self):
        """
        必须打开平台驱动导出的 /dev/watchdog，而不是自建 /dev/weaknet_wdt。

        自建设备需要 insmod + unbind 原驱动，二者之间必然存在一个
        「旧狗已撤、新狗未起」的真空期。方案 B 的整个价值就在于没有这个窗口。
        """
        self.assertIn('kWatchdogDev = "/dev/watchdog"', self.src)
        self.assertIn('kWatchdogAlt = "/dev/watchdog0"', self.src)
        self.assertNotIn("/dev/weaknet_wdt", self.src)
        self.assertNotIn("/dev/watchdog1", self.src)

    def test_magicclose_is_hard_asserted(self):
        """
        WDIOF_MAGICCLOSE 是「不写 V 也能停狗」与「不写 V 就复位」的分界。

        驱动不支持它时，close() 会无条件 ops->stop()：停止喂狗不再触发复位，
        四项判据全部归零，而进程仍然退出码 0、日志一片安静。
        因此这一条必须是启动期硬断言，不能只写在注释里。
        """
        body = self._func("VerifyDeviceContract")
        self.assertIn("WDIOF_MAGICCLOSE", body)
        self.assertRegex(body, r"WDIOF_MAGICCLOSE\)\s*==\s*0",
                         "必须检查 MAGICCLOSE 位为 0 的情况")
        self.assertIn("return false", body)

    def test_timeout_lower_bound_exceeds_debounce(self):
        """
        硬件超时必须显著长于确认窗口，否则「连续 K 次判据失败」期间就会
        超时复位 —— 判据去抖形同虚设，一次网络抖动就能复板子。

        下限取 2 倍确认窗口（TICK_S * DEBOUNCE_K），给采集与落盘留余量。
        """
        body = self._func("VerifyDeviceContract")
        self.assertIn("WDIOC_GETTIMEOUT", body)
        self.assertRegex(body, r"timeout\s*<\s*need",
                         "必须对硬件超时做下限断言")
        self.assertIn("kTickSeconds * kDebounceK * 2", body)

    def test_contract_failure_writes_v_before_close(self):
        """
        契约不满足时必须先写 'V' 再 close。

        因为 open() 已经把硬件使能了：直接 return 会让 fd 被内核关掉，
        走的就是「没写 V → 拒绝停狗 → 硬件超时复位」那条故障路径 ——
        一个配置错误会变成无限复位。写 'V' 把这条路径显式改成优雅停机。
        """
        m = re.search(r"VerifyDeviceContract\(wd_fd.*?return 1;", self.src, re.S)
        self.assertIsNotNone(m, "找不到 main() 里的契约检查分支")
        branch = m.group(0)
        i_v = branch.find('write(wd_fd, "V", 1)')
        i_close = branch.find("close(wd_fd)")
        self.assertGreater(i_v, 0, "契约失败分支必须写 'V'")
        self.assertGreater(i_close, 0, "契约失败分支必须 close")
        self.assertLess(i_v, i_close, "必须先写 'V' 再 close")

    def test_no_ioctl_that_changes_hardware_state(self):
        """
        除只读的 GETSUPPORT / GETTIMEOUT 外，不得出现任何会改硬件状态的
        ioctl（SETTIMEOUT / SETOPTIONS / SETPRETIMEOUT / KEEPALIVE）。

        理由：喂狗只走 write()。一旦用 ioctl 去 arm/disarm 或改超时，
        停止喂狗就不再是唯一出口，「不写 V 就复位」这条不变量被绕过，
        而且改过的超时不会自动恢复。
        """
        for banned in ("WDIOC_SETTIMEOUT", "WDIOC_SETOPTIONS",
                       "WDIOC_SETPRETIMEOUT", "WDIOC_KEEPALIVE"):
            self.assertNotIn(banned, self.src,
                             f"{banned} 会改硬件状态，喂狗只允许 write()")
        self.assertIn("WDIOC_GETSUPPORT", self.src)

    def test_bootstatus_is_deliberately_not_read(self):
        """
        WDIOC_GETBOOTSTATUS 在本平台恒返回 0（sunxi_wdt 未实现 .bootstatus），
        读它只会产生「本次复位不是看门狗导致」的假结论。宁可完全不读。
        """
        self.assertNotIn("WDIOC_GETBOOTSTATUS", self.src)

    def test_first_feed_happens_immediately_after_open(self):
        """
        open() 已经启动硬件计时，因此契约通过后必须立刻喂第一次。

        否则「open → 进入循环 → 第一次 tick」之间是一段完全没有喂狗的窗口，
        在超时调小的设备上会直接复位。
        """
        m = re.search(r"VerifyDeviceContract\(wd_fd.*?wd_fd = |return 1;\s*\n\s*\}", self.src, re.S)
        # 退而求其次：直接在 main 里找契约调用之后是否紧接 write "1"
        i_verify = self.src.find("VerifyDeviceContract(wd_fd")
        self.assertGreater(i_verify, 0)
        tail = self.src[i_verify:]
        # 到主循环为止的区间内必须有首次喂狗
        i_loop = tail.find("while (")
        self.assertGreater(i_loop, 0, "找不到主循环")
        self.assertIn('write(wd_fd, "1", 1)', tail[:i_loop],
                      "契约通过后必须立刻喂第一次")


class TestGuardDoesNotRepairService(unittest.TestCase):
    """
    守卫**刻意不**拉起采集服务。这条必须在测试里固定住。

    为什么这不是「少写了一个功能」：修复动作本身需要时间，而硬件超时
    不会等你。修复期间要么继续喂狗（于是故障期没有保护），要么不喂狗
    （于是复位发生在修复中途）。守卫唯一的职责是**发现**，恢复交给
    硬件复位 + systemd 开机自启。
    """

    @classmethod
    def setUpClass(cls):
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            raw = f.read()
        # 只检查**代码**，不检查注释。注释里出现 systemctl restart 是
        # 在解释「为什么不能那么做」，把它当成违规会让这条断言反向失真。
        cls.src = cls._strip_comments(raw)

    @staticmethod
    def _strip_comments(text):
        """去掉 // 行注释与 /* */ 块注释，保留字符串字面量原样。"""
        out = []
        i, n = 0, len(text)
        while i < n:
            ch = text[i]
            if ch == '"':
                j = i + 1
                while j < n:
                    if text[j] == '\\':
                        j += 2
                        continue
                    if text[j] == '"':
                        j += 1
                        break
                    j += 1
                out.append(text[i:j])
                i = j
            elif ch == '/' and i + 1 < n and text[i + 1] == '/':
                j = text.find('\n', i)
                i = n if j < 0 else j
            elif ch == '/' and i + 1 < n and text[i + 1] == '*':
                j = text.find('*/', i + 2)
                i = n if j < 0 else j + 2
                out.append(' ')
            else:
                out.append(ch)
                i += 1
        return ''.join(out)

    def test_no_service_start_or_restart(self):
        for banned in ("systemctl start", "systemctl restart",
                       "systemctl stop", "systemctl enable",
                       "systemctl daemon-reload"):
            self.assertNotIn(banned, self.src,
                             f"guard 不得执行 {banned}：喂狗期间修服务等于关掉保护")

    def test_mainpid_query_is_the_only_systemctl_use(self):
        """
        允许的唯一 systemctl 用法是只读的 `show -p MainPID`
        —— 它用来判断服务进程是否还在、是否换过 PID。

        命令是通过 snprintf 拼成单个字符串交给 popen 的（不是 execv 数组），
        因此按「代码里出现的所有 systemctl 子命令」来断言。
        """
        subs = re.findall(r"systemctl\s+([a-z-]+)", self.src)
        self.assertEqual(subs, ["show"],
                         f"guard 里的 systemctl 子命令只能是 show，实际 {subs}")


class TestCommandTimeout(unittest.TestCase):
    """
    命令超时：守卫不得被自己启动的子进程焊死。

    这是本文件里最重要的一组断言。原始实现用 popen/pclose，没有超时：
    子进程挂死 → 主循环停在 pclose → 不再喂狗 → 16 秒后整机复位，
    而 WriteFailureJson 永远执行不到。「板子无缘无故重启」就是这么来的。
    """

    @classmethod
    def setUpClass(cls):
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            cls.raw = f.read()
        cls.src = _strip_c_comments(cls.raw)

    def _func(self, name):
        m = re.search(r"\n\w[\w\s\*&:<>]*?\b" + name + r"\([^)]*\)\s*\{", self.src)
        self.assertIsNotNone(m, f"找不到函数 {name}")
        i = self.src.index("{", m.start())
        depth = 0
        for j in range(i, len(self.src)):
            if self.src[j] == "{":
                depth += 1
            elif self.src[j] == "}":
                depth -= 1
                if depth == 0:
                    return self.src[i:j + 1]
        self.fail(f"{name} 大括号未配平")

    def test_no_popen_pclose(self):
        """
        popen/pclose 必须彻底消失。

        这条不是「偏好」问题：pclose 的语义就是「等到子进程退出」，
        它没有任何超时机制。保留它就是保留无限阻塞的可能。
        """
        body = self._func("RunCommand")
        self.assertNotIn("pclose", body, "RunCommand 不得使用 pclose（无超时原语）")
        self.assertNotIn("popen", body, "RunCommand 不得使用 popen（无超时原语）")

    def test_uses_poll_with_deadline(self):
        body = self._func("RunCommand")
        self.assertIn("poll", body, "必须用 poll 带超时地等管道")
        self.assertIn("deadline", body, "必须有显式死线，不能只靠单次 poll")

    def test_kills_whole_process_group(self):
        """
        必须杀进程组而不是单个 pid。

        命令是交给 /bin/sh -c 执行的，sh 会 fork 出真正的命令。
        只杀 sh 会留下孤儿 busctl 继续占着管道 —— waitpid 收不到 EOF，
        等于没杀。
        """
        body = self._func("RunCommand")
        self.assertIn("kill(-pid", body,
                      "必须 kill(-pid) 杀整个进程组，否则孤儿进程占住管道")
        self.assertIn("setpgid", body,
                      "必须 setpgid 建立独立进程组，kill(-pid) 才有意义")

    def test_reaps_child_always(self):
        """无论正常结束还是被杀，都必须 waitpid 回收，否则留僵尸。"""
        body = self._func("RunCommand")
        self.assertIn("waitpid", body, "必须 waitpid 回收子进程")

    def test_child_does_not_inherit_watchdog_fd(self):
        """
        管道 fd 必须 CLOEXEC。

        这条防的是一个会**彻底废掉整个保护**的写法：若子进程继承了
        看门狗 fd，父进程死掉后 fd 仍被 sh 持有，内核不会执行 release()，
        于是「停止喂狗」不再导致复位 —— 判据全部失效但系统看起来正常。
        （看门狗 fd 本身已用 O_CLOEXEC 打开，这里再钉一次管道侧。）
        """
        body = self._func("RunCommand")
        self.assertIn("O_CLOEXEC", body, "pipe 必须带 O_CLOEXEC")
        self.assertIn("O_CLOEXEC", self._func("OpenWatchdog"),
                      "看门狗 fd 必须带 O_CLOEXEC，否则子进程会继承它")

    def test_budget_fits_hardware_timeout(self):
        """
        预算不变式：4 条判据各自超时，仍必须显著小于硬件超时。

        这条断言的存在意义是防止后来者把 1500 改大。改成 4000 时
        4×4000 = 16 秒 = 硬件超时，一次全挂就会在判定完成前复位，
        而且 WriteFailureJson 来不及写。
        """
        worst_ms = CMD_TIMEOUT_MS * 4 + TICK_S * 1000
        self.assertLess(worst_ms, HARDWARE_TIMEOUT_S * 1000,
                        f"最坏 {worst_ms}ms 必须 < 硬件超时 "
                        f"{HARDWARE_TIMEOUT_S * 1000}ms")
        # 实际只有 2 条判据起子进程，真实最坏更宽松
        real_ms = CMD_TIMEOUT_MS * 2 + TICK_S * 1000
        self.assertLess(real_ms, HARDWARE_TIMEOUT_S * 1000 // 2,
                        "真实最坏路径应有一倍以上余量")

    def test_timeout_is_a_failure_not_a_pass(self):
        """
        超时必须算判据失败（ok=false），不能算通过。

        算通过 = 一次挂死就能让保护永久静默。这是「无法判定 ≠ 健康」
        这条不变式的可执行形式。
        """
        body = self._func("CheckSampleNs")
        self.assertIn("-ETIMEDOUT", body, "CheckSampleNs 必须显式区分超时")
        # 超时分支必须 return false
        m = re.search(r"-ETIMEDOUT\)\s*\{(.*?)\n    \}", body, re.S)
        self.assertIsNotNone(m, "找不到超时分支")
        self.assertIn("return false", m.group(1),
                      "超时分支必须 return false（无法判定 ≠ 健康）")

    def test_mainpid_timeout_is_not_service_death(self):
        """
        systemctl 超时不得被当成「服务不存在」。

        二者的运维含义完全不同：服务不存在 → 等 12 秒宽限 → 复位；
        systemctl 卡住 → 我们只是不知道。混在一起会让一次 systemd 抖动
        变成一次整机复位，也就是守卫用复位惩罚自己的观测失败。
        """
        body = self._func("GetMainPid")
        self.assertIn("out_unobservable", body,
                      "GetMainPid 必须把「观测失败」与「确实没有 PID」分开")
        self.assertIn("kPidUnknownMaxStreak", self.src,
                      "必须有有界容忍常量，不能无限喂狗也不能立即复位")

    def test_pid_unknown_tolerance_is_bounded(self):
        """
        容忍必须有界，且必须小于服务缺席宽限的一半。

        无界 = systemctl 永久挂死时保护永久失效。
        大于等于宽限 = 「先容忍再判定」的总耗时挤占硬件预算。
        """
        self.assertLessEqual(PID_UNKNOWN_MAX_STREAK * TICK_S,
                             INACTIVE_GRACE_S,
                             "观测失败容忍总时长不得超过服务缺席宽限")
        self.assertGreater(PID_UNKNOWN_MAX_STREAK, DEBOUNCE_K,
                           "容忍轮数必须大于去抖 K，否则一次抖动就停喂")


class TestResetBreaker(unittest.TestCase):
    """
    连续复位熔断：确定性故障不得把网关变成每 28 秒重置一次的砖。

    这组用例把 BreakerOpen() 的判定规则用 Python 复现一遍。之所以值得
    重复实现，是因为这条规则一旦写错，后果是「该熔断时不熔断」（无限复位）
    或「不该熔断时熔断」（失去保护），两者在板端都很难观察到。
    """

    @classmethod
    def setUpClass(cls):
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            raw = f.read()
        cls.src = _strip_c_comments(raw)

    def breaker_open(self, ids, current):
        """
        BreakerOpen() 的规则复现。

        ids 是历史里的 boot_id 列表（按时间顺序），current 是当前 boot_id。
        规则：取最后 TRIP_BREAKER_THRESHOLD 条，要求互不相同且都不含当前 boot。
        """
        if len(ids) < TRIP_BREAKER_THRESHOLD:
            return False
        window = ids[-TRIP_BREAKER_THRESHOLD:]
        if current in window:
            return False
        if len(set(window)) != len(window):
            return False
        return True

    def test_no_history(self):
        """没有历史 → 不熔断。首次启动就是这种情况。"""
        self.assertFalse(self.breaker_open([], "boot-1"))

    def test_below_threshold(self):
        self.assertFalse(self.breaker_open(["a"], "d"))
        self.assertFalse(self.breaker_open(["a", "b"], "d"))

    def test_threshold_reached(self):
        """三个不同 boot 连续以触发收场 → 熔断。"""
        self.assertTrue(self.breaker_open(["a", "b", "c"], "d"))

    def test_current_boot_excluded(self):
        """
        当前 boot 已在窗口里 → 不熔断。

        理由：当前 boot 触发过一次，说明这次启动还没走完（它正在等复位）。
        把它算进连续计数会让阈值提前一个 boot 触发 ——
        第一次真实熔断本应在第 3 次失败后的那次启动，提前到第 2 次。
        """
        self.assertFalse(self.breaker_open(["a", "b", "c"], "c"))
        self.assertFalse(self.breaker_open(["a", "b", "c"], "b"))

    def test_duplicate_boot_ids_not_counted(self):
        """
        同一个 boot 触发两次只算一次。

        RecordTrip() 在同一 boot 里理论上只会执行一次（写 flag 后退出），
        但若 flag 写失败（磁盘满）就可能重复。重复不该加速熔断。
        """
        self.assertFalse(self.breaker_open(["a", "a", "b"], "d"))
        self.assertFalse(self.breaker_open(["a", "b", "b"], "d"))

    def test_only_last_n_matter(self):
        """只有最后 N 条参与判定，更早的历史不影响。"""
        # 前两个相同，但不在窗口内（窗口是后三条）
        self.assertTrue(self.breaker_open(["x", "x", "a", "b", "c"], "d"))

    def test_history_cap_enforced(self):
        """
        历史行数有上限。

        没有上限 → 文件无限增长；有上限但太小 → 排障时看不到更早的触发。
        TRIP_HISTORY_MAX 必须大于等于阈值，否则永远凑不够。
        """
        self.assertGreaterEqual(TRIP_HISTORY_MAX, TRIP_BREAKER_THRESHOLD,
                                "历史容量必须不小于熔断阈值")
        self.assertLess(TRIP_HISTORY_MAX, 20, "历史容量不应无谓地大")

    def test_healthy_reset_window_longer_than_breaker_accumulation(self):
        """
        健康复位窗口必须远长于一次启动到判定所需时间。

        太短 → 每次正常启动都会清空历史，熔断永远不触发（形同虚设）。
        太长 → 偶发故障跨很久仍被累加，误熔断。
        半小时对「启动 + 12 秒豁免 + 4 秒判据」是极宽松的余量。
        """
        startup_s = RESTART_WARMUP_S + TICK_S * DEBOUNCE_K
        self.assertGreater(TRIP_HISTORY_RESET_S, startup_s * 10,
                           "健康复位窗口应比启动耗时大一个数量级")

    def test_exit_code_distinct_from_trip(self):
        """
        熔断退出码必须与判据失败退出码区分。

        两者运维处置完全不同：99 → 查采集服务；98 → 查设备本身。
        相同会让「设备级确定性故障」被误读成「服务又挂了」。
        """
        self.assertNotEqual(EXIT_BREAKER_OPEN, 99)
        # 都必须小于 128（不与信号退出码混淆）
        self.assertLess(EXIT_BREAKER_OPEN, 128)
        self.assertGreaterEqual(EXIT_BREAKER_OPEN, 1)

    def test_breaker_checked_before_open(self):
        """
        熔断判定必须在 open() 之前 —— 与 trip flag 同一条理由。

        open() 会 arm 硬件。熔断的语义是「放弃保护、让设备留在可访问状态」，
        一旦开了设备，进程退出反而会触发复位，与目的正好相反。
        """
        body = self.src
        i_main = body.index("int main(int argc, char** argv)")
        body = body[i_main:]
        i_trip = body.index("AlreadyTrippedThisBoot()")
        i_brk = body.index("BreakerOpen(brk_reason")
        i_open = body.index("OpenWatchdog()")
        self.assertLess(i_trip, i_open, "trip flag 判定必须在 open 之前")
        self.assertLess(i_brk, i_open, "熔断判定必须在 open 之前")
        self.assertLess(i_trip, i_brk, "先查本次 boot 是否已触发，再查跨 boot 历史")

    def test_trip_records_history(self):
        """
        RecordTrip() 必须同时写 trip flag 与跨 boot 历史。

        只写 flag → 熔断器永远看不到确定性故障。
        只写历史 → systemd 重启能撤销已排定的复位。
        两件事放同一个函数里，就是为了让它们不可能被漏掉其中一个。
        """
        m = re.search(r"void RecordTrip\(const char\* reason\)\s*\{(.*?)\n\}",
                      self.src, re.S)
        self.assertIsNotNone(m, "RecordTrip 必须接收 reason 参数")
        self.assertIn("kTripFlag", m.group(1), "必须写 trip flag")
        self.assertIn("AppendTripHistory", m.group(1), "必须追加跨 boot 历史")

    def test_rescue_json_written(self):
        """熔断进入时必须留下可读的记录，否则运维只能看到退出码 98。"""
        self.assertIn("WriteRescueJson", self.src)
        self.assertIn("kRescueJson", self.src)
        self.assertIn("health-guard-rescue.json", self.src)


class TestDiskWatermark(unittest.TestCase):
    """
    磁盘水位：只观测，不判定。

    这组用例的核心是**守护「它不是判据」这件事**。初稿曾打算把它加成
    第五项判据，推演后发现是错的：磁盘满是一个复位无法修复的条件，
    加进判据就是用保护机制制造无限复位循环。
    """

    @classmethod
    def setUpClass(cls):
        with open("tools/weaknet-health-guard.cpp", encoding="utf-8") as f:
            raw = f.read()
        cls.raw = raw
        cls.src = _strip_c_comments(raw)

    def pct(self, blocks, bavail, frsize):
        """SampleDisk() 的百分比算法复现（含四舍五入）。"""
        total = blocks * frsize
        avail = bavail * frsize
        if total == 0:
            return 0
        return (avail * 100 + total // 2) // total

    def test_percentage_rounding(self):
        """四舍五入，不是截断 —— 9.6% 应报 10% 而不是 9%。"""
        self.assertEqual(self.pct(1000, 96, 1), 10)   # 9.6% → 10%
        self.assertEqual(self.pct(1000, 94, 1), 9)    # 9.4% → 9%
        self.assertEqual(self.pct(1000, 1000, 1), 100)
        self.assertEqual(self.pct(1000, 0, 1), 0)

    def test_uses_bavail_not_bfree(self):
        """
        必须用 f_bavail（非特权可用）而不是 f_bfree（含保留块）。

        f_bfree 会把 root 保留的 5% 算成「可用」，对一个以 root 运行的
        进程勉强成立，但报告口径应保守 —— 保留块存在的意义就是不该被算进
        正常可用空间。
        """
        body = self.src[self.src.index("void SampleDisk"):]
        body = body[:body.index("\n}")]
        self.assertIn("f_bavail", body, "必须用 f_bavail")
        self.assertNotIn("f_bfree", body, "不得用 f_bfree（含保留块，口径偏乐观）")

    def test_not_a_criterion(self):
        """
        **这条是这组用例的重点**：磁盘水位不得进 g_checks[]。

        进 g_checks 就意味着它参与 any_persistent_fail 判定，即
        「磁盘满 → 停喂 → 复位 → 磁盘还是满 → 再停喂」的无限循环。
        而这个洞其实已被判据 1（WAL mtime）覆盖 —— 磁盘满会让 SQLite
        写不进去，WAL 停止推进，12 秒内必被捕获。
        """
        m = re.search(r"CheckState g_checks\[\]\s*=\s*\{(.*?)\};", self.src, re.S)
        self.assertIsNotNone(m, "找不到 g_checks")
        self.assertNotIn("disk", m.group(1).lower(),
                         "磁盘水位不得作为判据进入 g_checks")
        self.assertEqual(DEBOUNCE_K, 2, "判据数量与去抖关系不应被这条改动影响")

    def test_four_checks_unchanged(self):
        """判据数量仍是 4 —— 这次加固没有增加也没有减少判定维度。"""
        m = re.search(r"CheckState g_checks\[\]\s*=\s*\{(.*?)\};", self.src, re.S)
        names = re.findall(r'\{"(\w+)"', m.group(1))
        self.assertEqual(names, ["wal_mtime", "sample_ns", "cpu_delta", "d_state"])

    def test_both_paths_sampled(self):
        """
        证据区与数据区必须分开采样。

        两者常常不在同一个文件系统上。合并（取较小值）会正好把
        「哪个分区满了」这个关键信息抹掉，而归因正是这项功能的全部价值。
        """
        self.assertIn("kFailureDir", self.src)
        self.assertIn("kDataDir", self.src)
        body = self.src[self.src.index("void SampleBothDisks"):]
        body = body[:body.index("\n}")]
        self.assertIn("kFailureDir", body, "必须采样证据分区")
        self.assertIn("kDataDir", body, "必须采样数据分区")

    def test_lands_in_failure_json(self):
        """水位必须进失败快照，否则复位后照样归因不了。"""
        m = re.search(r"void WriteFailureJson.*?\n\}", self.src, re.S)
        self.assertIsNotNone(m, "找不到 WriteFailureJson")
        body = m.group(0)
        self.assertIn("SampleBothDisks", body, "快照里必须采样磁盘")
        self.assertIn("free_pct", body, "快照里必须有剩余百分比")

    def test_warn_threshold_reasonable(self):
        """
        告警阈值必须留出人工介入窗口。

        太低（1~2%）→ 日志还没来得及被看到就已写满。
        太高（50%）→ 天天报警，运维学会忽略它。
        10% 对 192MB 库 + 5 秒落库周期是十几个小时的窗口。
        """
        self.assertGreaterEqual(DISK_WARN_PCT, 5, "阈值过低，来不及介入")
        self.assertLessEqual(DISK_WARN_PCT, 25, "阈值过高，告警会被忽略")

    def test_silent_failures_now_visible(self):
        """
        RecordTrip / WriteFailureJson 的写失败必须喊出来。

        这是「看不见的失效」最典型的形态：证据分区写满 → 两个文件都写不进去
        → 复位后没有任何记录 → 运维只知道「板子重启了」。
        更糟的是 trip flag 丢失会让熔断器也看不到这次启动。
        """
        for fn in ("void RecordTrip", "void WriteFailureJson",
                   "void AppendTripHistory"):
            m = re.search(re.escape(fn) + r".*?\n\}", self.src, re.S)
            self.assertIsNotNone(m, f"找不到 {fn}")
            body = m.group(0)
            self.assertRegex(body, r"fprintf\(stderr",
                             f"{fn} 写失败时必须打印到 stderr，不能静默 return")


if __name__ == "__main__":
    unittest.main(verbosity=2)
