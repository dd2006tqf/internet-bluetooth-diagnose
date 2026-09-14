#!/usr/bin/env python3
"""
WeakNet Assurance v1 长稳证据审计器 (Soak Analyzer)

输入：soak_monitor.py 产出的 JSONL（如 soak_24h.jsonl）
输出：结构化审计报告 + 明确的 PASS/FAIL 判定

设计原则：
  1. **判定标准先于数据**。每个指标都有写死的阈值与方向，不是"看完了再解释"。
  2. **趋势用线性回归斜率**表达，而不是"首末两点相减"——后者会被单点噪声主导。
  3. **缺失数据如实报告为 SKIP**，绝不把"没采集到"当作"通过"。
  4. 只读分析，不修改被分析的数据文件。

审计维度（对应 v1 长稳的 8 项要求）：
  A. 内存趋势        RSS 线性回归斜率应接近 0（无持续增长）
  B. FD / socket    文件描述符与套接字数量斜率应接近 0（无泄漏）
  C. BPF map 水位   dns_self_endpoints 必须始终 <= LRU 上限(256)，且无单调爬升
  D. 事件丢失       emit_fail / perf_lost 增量应为 0（或比率低于 v1 门禁 2%）
  E. Tracker        unmatched / late 不得持续增长
  F. 状态抖动       状态跃迁（transitions）应在预算内
  G. 探测调度       Portal oracle 轮次间隔应稳定在配置的 30s 附近
  H. 数据库         体积应线性增长（记录历史），不得爆炸式增长
  I. D-Bus 信号     每分钟信号数应平稳，无风暴

用法：
  python3 tools/analyze_soak.py --input soak_24h.jsonl
  python3 tools/analyze_soak.py --input soak_24h.jsonl --min-hours 0.1   # 提前自检用
"""

import argparse
import json
import os
import sys


# ── 审计阈值（v1 冻结判据，改动需 reopen freeze）────────────────────────────
RSS_SLOPE_MB_PER_H = 0.5       # RSS 每小时增长上限（数量级 ~12MB，斜率有意义）
FD_SLOPE_PER_H = 1.0           # FD 每小时增长上限（数量级 ~150，长窗口斜率有意义）
FD_SPAN_LIMIT = 5              # 短窗口下 FD 允许的波动幅度（防 ±1 抖动误判）
DB_GROWTH_MB_PER_H_LIMIT = 60.0

# 小整数指标（个位数）用「波动幅度」判定，不用回归斜率：
# 短窗口下 ±1 的抖动会被斜率放大成看似很大的漂移（实测 0.13h 得 -0.657/h）。
SOCK_SPAN_LIMIT = 4            # socket 个数允许的波动幅度
THREAD_SPAN_LIMIT = 3          # 线程数允许的波动幅度

BPF_ENDPOINT_CAP = 256         # dns_self_endpoints LRU 上限（内核强制，与代码一致）
EMIT_FAIL_RATIO_LIMIT = 0.02   # v1 观测质量门禁（与 evaluator 一致）
PERF_LOSS_RATIO_LIMIT = 0.02
TRACKER_GROWTH_LIMIT = 100     # unmatched/late 全程允许净增上限
TRANSITION_BUDGET_PER_DAY = 20 # 状态跃迁预算
PROBE_INTERVAL_TARGET = 30.0   # 探测间隔（配置值）
PROBE_INTERVAL_TOL = 8.0       # 允许漂移容差
# 一轮探测的实际耗时（实测 3~8s）：3 个 capability 目标 + 4 个 oracle 端点，
# 每个都要走 DNS/TCP/TLS 阶段。周期语义是 interval + 轮耗时。
ROUND_DURATION_MIN_S = 30.0    # interval + 最小轮耗时
ROUND_DURATION_MAX_S = 38.0    # interval + 最大轮耗时


PASS, FAIL, SKIP = [], [], []


def record(kind, name, detail):
    (PASS if kind == "PASS" else FAIL if kind == "FAIL" else SKIP).append((name, detail))


# ── 线性回归 ────────────────────────────────────────────────────────────────

def slope_per_hour(rows, key_fn):
    """
    对 (hours, value) 做最小二乘线性回归，返回斜率（单位/小时）。
    比"首末相减"稳健：单点噪声不会主导结论。

    时间轴用归一化后的 `_t_h`（见 normalize_timeline），而不是原始
    uptime_sec —— 后者在分段合并后会回绕，导致趋势结论完全错误。
    """
    pts = []
    for r in rows:
        v = key_fn(r)
        if v is None:
            continue
        pts.append((r["_t_h"], float(v)))
    n = len(pts)
    if n < 3:
        return None
    sx = sum(p[0] for p in pts)
    sy = sum(p[1] for p in pts)
    sxx = sum(p[0] * p[0] for p in pts)
    sxy = sum(p[0] * p[1] for p in pts)
    denom = n * sxx - sx * sx
    if abs(denom) < 1e-9:
        return None
    return (n * sxy - sx * sy) / denom


def normalize_timeline(rows):
    """
    为所有样本建立单调递增的累计时间轴 `_t_h`（单位：小时），并返回总时长。

    为什么必须做：分段证据（如断电前的段1 + 重启后的段2）合并后，
    每段的 uptime_sec 都从 0 重新计数。若直接沿用，时间轴会回绕，
    回归斜率与"后半程"判定全部失去意义。

    做法：同一段内用该段的 uptime_sec；段与段之间按真实墙钟时间戳
    （timestamp，UTC epoch 秒）衔接，因此断点期间的停机时长也被正确计入
    累计观测跨度（这正是"分段累计 24h"想要的口径）。
    """
    if not rows:
        return 0.0
    rows.sort(key=lambda r: r.get("timestamp", 0.0))
    t0 = rows[0].get("timestamp", 0.0)
    for r in rows:
        ts = r.get("timestamp")
        if ts is None:
            r["_t_h"] = 0.0
        else:
            r["_t_h"] = (ts - t0) / 3600.0
    return rows[-1]["_t_h"]


def seg_slopes(rows, key_fn):
    """
    对每个分段**独立**计算回归斜率，返回 (最差斜率, 分段明细)。

    为什么必须分段：进程重启后，进程级指标的**基线会跳变**（新进程的
    FD/RSS 起点与旧进程末值不同）。若跨段混算回归，"基线跳变"会被误读成
    "持续增长趋势"。

    实测（2026-09-14）：段1 末值 FD=148、段2 全段恒定 151（3.16h 无一变化），
    混算得到 +0.643/h 的"增长"假象，而两段独立斜率分别是 +0.020 与 -0.016，
    即真实情况是**零增长**。

    返回最差（绝对值最大）斜率作为判据，保守取值。
    """
    worst = None
    detail = []
    for s in sorted({r["_segment"] for r in rows}):
        seg = [r for r in rows if r["_segment"] == s]
        if len(seg) < 3:
            continue
        sl = slope_per_hour(seg, key_fn)
        if sl is None:
            continue
        detail.append((os.path.basename(s)[:22], sl, len(seg)))
        if worst is None or abs(sl) > abs(worst):
            worst = sl
    return worst, detail


def sawtooth_stats(vals, window=60):
    """
    判定时间序列是否呈「锯齿」（即有升有降，说明存在回收机制）。

    为什么不能逐样本比较：采样间隔 10s，而水位在相邻样本间几乎不变，
    逐样本统计"回落次数"会把缓慢下降误算成不回落（实测 2130 次观测
    只数出 18 次回落 = 0.8%，得出"疑似单调爬升"的错误结论）。

    正确做法：先按窗口取中位数降采样，消除逐样本噪声，再统计窗口之间
    的升降段数。返回 (up, down, samples)，供调用方判断是否既有升也有降。
    """
    import statistics
    if not vals:
        return 0, 0, []
    win = []
    for i in range(0, len(vals), window):
        chunk = vals[i:i + window]
        if chunk:
            win.append(statistics.median(chunk))
    up = sum(1 for a, b in zip(win, win[1:]) if b > a)
    down = sum(1 for a, b in zip(win, win[1:]) if b < a)
    return up, down, [int(x) for x in win]


def rng(rows, key_fn):
    vals = [key_fn(r) for r in rows if key_fn(r) is not None]
    if not vals:
        return None, None
    return min(vals), max(vals)


def main():
    ap = argparse.ArgumentParser(description="WeakNet v1 Soak Analyzer")
    ap.add_argument("--input", required=True, action="append",
                    help="可多次指定（分段证据合并审计），例如 "
                         "--input soak_3h69_prepowerloss.jsonl --input soak_24h.jsonl")
    ap.add_argument("--min-hours", type=float, default=0.0,
                    help="低于该累计时长只做趋势自检，不给出最终结论")
    args = ap.parse_args()

    rows = []
    for path in args.input:
        seg = []
        try:
            with open(path) as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        seg.append(json.loads(line))
                    except Exception:
                        pass
        except FileNotFoundError:
            print(f"[!] 找不到输入文件: {path}", file=sys.stderr)
            return 2
        if not seg:
            print(f"[!] {path} 无有效样本", file=sys.stderr)
            continue
        # 标注分段来源，便于报告里显示断点
        for r in seg:
            r["_segment"] = path
        rows.extend(seg)
        print(f"[*] {path}: {len(seg)} 条，"
              f"PID={sorted({r.get('pid') for r in seg})}，"
              f"时长 {seg[-1]['uptime_sec']/3600:.2f}h")

    if len(rows) < 10:
        print(f"[!] 样本过少（{len(rows)} 条），无法分析。", file=sys.stderr)
        return 2

    # 建立跨分段的单调时间轴（原始 uptime_sec 在分段间会回绕）
    span_h = normalize_timeline(rows)

    # 实际在跑的观测时长（各段 uptime 之和），用于区分"跨度"与"有效观测"
    active_h = 0.0
    for seg_path in {r["_segment"] for r in rows}:
        seg_rows = [r for r in rows if r["_segment"] == seg_path]
        active_h += seg_rows[-1]["uptime_sec"] / 3600.0

    print()
    print("=" * 72)
    print("WeakNet Assurance v1 — 长稳证据审计报告")
    print("=" * 72)
    print(f"采样条数   : {len(rows)}")
    print(f"累计观测   : {active_h:.2f} 小时（各段有效运行之和）")
    print(f"时间跨度   : {span_h:.2f} 小时（含段间停机）")
    start_ts = rows[0].get("timestamp")
    end_ts = rows[-1].get("timestamp")
    if start_ts and end_ts:
        import datetime as _dt
        print(f"起始时间   : {_dt.datetime.fromtimestamp(start_ts, _dt.timezone.utc):%Y-%m-%d %H:%M:%S} UTC")
        print(f"结束时间   : {_dt.datetime.fromtimestamp(end_ts, _dt.timezone.utc):%Y-%m-%d %H:%M:%S} UTC")

    segs = sorted({r["_segment"] for r in rows})
    pids = sorted({r.get("pid") for r in rows})
    print(f"分段数     : {len(segs)}")
    for s in segs:
        seg_rows = [r for r in rows if r["_segment"] == s]
        import datetime as _dt
        a = _dt.datetime.fromtimestamp(seg_rows[0]["timestamp"], _dt.timezone.utc)
        b = _dt.datetime.fromtimestamp(seg_rows[-1]["timestamp"], _dt.timezone.utc)
        print(f"   - {os.path.basename(s)}")
        print(f"       {a:%m-%d %H:%M:%S} → {b:%m-%d %H:%M:%S} UTC  "
              f"{seg_rows[-1]['uptime_sec']/3600:.2f}h  PID={seg_rows[0].get('pid')}")
    print(f"被测 PID   : {pids}")
    print()

    pid_changes = len(pids)
    if pid_changes != 1:
        # 多段是用户明确接受的判据（分段累计 24h），故不是缺陷。
        # 但必须如实标注：跨段的"首末相减"类指标不可直接用（已在 D/E/F 中
        # 改为分段计算再求和），且段间趋势需谨慎解释。
        record("PASS", "进程连续性（分段累计）",
               f"{pid_changes} 个 PID / {len(segs)} 段；"
               f"段间计数器归零，D/E/F 已按分段累计处理")
    else:
        record("PASS", "进程连续性", "全程单一 PID，未发生重启")

    # ── A. 内存趋势 ─────────────────────────────────────────────────────────
    # 分段计算：进程重启后 RSS 基线会跳变，跨段混算会把"基线跳变"
    # 误读成趋势（同 B1，见 seg_slopes 的说明）。
    lo, hi = rng(rows, lambda r: r["process"]["rss_mb"])
    s, det = seg_slopes(rows, lambda r: r["process"]["rss_mb"])
    if s is None:
        record("SKIP", "A. RSS 趋势", "样本不足")
    else:
        det_s = " / ".join(f"{n}:{v:+.3f}" for n, v, _ in det)
        msg = f"最差分段斜率 {s:+.3f} MB/h（范围 {lo}~{hi} MB，分段 {det_s}）"
        record("PASS" if abs(s) <= RSS_SLOPE_MB_PER_H else "FAIL", "A. RSS 趋势", msg)

    # ── B. FD / socket ─────────────────────────────────────────────────────
    # FD 数量级大（~150），长窗口下斜率是有意义的泄漏指标。
    # 判据分两层：
    #   短窗口（<1h）→ 只判「是否有界」，斜率会被 ±1 抖动放大
    #                  （实测 0.08h 内 148→149 得 -2.145/h）
    #   长窗口       → **分段**斜率判定（跨段混算会被基线跳变污染，
    #                  实测段1末148/段2恒定151 混算出 +0.643/h 假增长）
    FD_SLOPE_MIN_HOURS = 1.0
    lo_fd, hi_fd = rng(rows, lambda r: r["process"]["fds"])
    s_fd, det_fd = seg_slopes(rows, lambda r: r["process"]["fds"])
    if hi_fd is None:
        record("SKIP", "B1. FD 有界性", "样本不足")
    else:
        fd_span = hi_fd - lo_fd
        if active_h >= FD_SLOPE_MIN_HOURS and s_fd is not None:
            det_s = " / ".join(f"{n}:{v:+.3f}" for n, v, _ in det_fd)
            record("PASS" if abs(s_fd) <= FD_SLOPE_PER_H else "FAIL",
                   "B1. FD 趋势",
                   f"最差分段斜率 {s_fd:+.3f}/h（范围 {lo_fd}~{hi_fd}，"
                   f"累计 {active_h:.2f}h，分段 {det_s}）")
        else:
            record("PASS" if fd_span <= FD_SPAN_LIMIT else "FAIL",
                   "B1. FD 有界性",
                   f"波动幅度 {fd_span}（范围 {lo_fd}~{hi_fd}，上限 {FD_SPAN_LIMIT}）"
                   f" —— 观测仅 {active_h:.2f}h 不足 {FD_SLOPE_MIN_HOURS}h，"
                   f"驻留趋势待长窗口判定")

    # socket / 线程是**小整数**指标（个位数），斜率在短窗口下会被 ±1 抖动
    # 放大成看似很大的漂移（实测 0.13h 内 ±1 得 -0.657/h）。对这类指标
    # 正确的判据是"是否有界"，而不是回归斜率。
    lo_sk, hi_sk = rng(rows, lambda r: r["process"]["sockets"])
    if hi_sk is None:
        record("SKIP", "B2. socket 有界性", "样本不足")
    else:
        sk_span = hi_sk - lo_sk
        record("PASS" if sk_span <= SOCK_SPAN_LIMIT else "FAIL",
               "B2. socket 有界性",
               f"波动幅度 {sk_span}（范围 {lo_sk}~{hi_sk}，上限 {SOCK_SPAN_LIMIT}）")

    lo_th, hi_th = rng(rows, lambda r: r["process"]["threads"])
    if hi_th is None:
        record("SKIP", "B3. 线程有界性", "样本不足")
    else:
        th_span = hi_th - lo_th
        record("PASS" if th_span <= THREAD_SPAN_LIMIT else "FAIL",
               "B3. 线程有界性",
               f"波动幅度 {th_span}（范围 {lo_th}~{hi_th}，上限 {THREAD_SPAN_LIMIT}）")

    # ── C. BPF map 水位 ────────────────────────────────────────────────────
    ep_vals = [r["bpf"].get("dns_self_endpoints_count") for r in rows]
    ep_vals = [v for v in ep_vals if v is not None]
    if not ep_vals:
        record("SKIP", "C. dns_self_endpoints 水位", "未采集")
    else:
        hi_ep, lo_ep = max(ep_vals), min(ep_vals)
        tail = ep_vals[len(ep_vals) // 2:]
        tail_hi = max(tail) if tail else hi_ep
        over = hi_ep > BPF_ENDPOINT_CAP

        # 锯齿判定：既要证明"有界"，也要证明"回收真的在工作"。
        # 只证有界不够 —— 若水位单调爬到接近上限再不动，虽有界但说明 TTL
        # 回收失效，长期仍有OOM/LRU 抖动风险。
        up, down, _ = sawtooth_stats(ep_vals)
        sawtooth_ok = up >= 3 and down >= 3
        # 末段水位应显著低于历史峰值，证明回收持续发生
        tail_low = min(tail) if tail else lo_ep
        not_pinned = tail_hi < hi_ep * 0.9 or tail_low < hi_ep * 0.5

        detail = (f"峰值 {hi_ep} / 内核硬上限 {BPF_ENDPOINT_CAP}"
                  f"（范围 {lo_ep}~{hi_ep}，后半程 {tail_low}~{tail_hi}）"
                  f"，升降段 {up}/{down}")

        if over:
            record("FAIL", "C. dns_self_endpoints 水位",
                   detail + " —— 越过内核上限（不可能，须查证）")
        elif not sawtooth_ok:
            record("FAIL", "C. dns_self_endpoints 水位",
                   detail + " —— 无锯齿形态，疑似回收失效或单调爬升")
        elif not not_pinned:
            record("FAIL", "C. dns_self_endpoints 水位",
                   detail + " —— 末段贴近峰值，疑似贴顶")
        else:
            record("PASS", "C. dns_self_endpoints 水位",
                   detail + " —— 有界且呈锯齿回落")

    # self_pid 类 map 的值必须与**该段**被测进程一致，否则 provenance 过滤失效。
    #
    # 注意判据不能是"全局只有一个值"：分段证据（重启前/后）本就该有两个 PID，
    # 那是正常的重启结果。真正要证伪的是「段内漂移」——即某个段中途
    # 过滤 PID 变成别的进程，那才说明过滤失效。
    bad_pid = []
    for s in sorted({r["_segment"] for r in rows}):
        seg_rows = [r for r in rows if r["_segment"] == s]
        seg_pid = seg_rows[0].get("pid")
        for k in ("http_self_pid_value", "tcp_self_pid_value", "retrans_self_pid_value"):
            vals = {r["bpf"].get(k) for r in seg_rows if r["bpf"].get(k)}
            if vals and vals != {seg_pid}:
                bad_pid.append(f"{os.path.basename(s)}/{k}={sorted(vals)}（该段 PID={seg_pid}）")
    if bad_pid:
        record("FAIL", "C2. provenance PID 段内稳定", "过滤 PID 段内漂移: " + "; ".join(bad_pid))
    else:
        record("PASS", "C2. provenance PID 段内稳定",
               f"各段的 self_pid 均与段内被测进程一致（段数 {len({r['_segment'] for r in rows})}）")

    # ── D. 事件丢失 ────────────────────────────────────────────────────────
    # 计数器在进程重启后归零，故**必须分段计算净增再求和**。
    # 直接用 rows[-1]-rows[0] 在分段证据上会得到负数或错误的 0。
    def seg_delta(key_path):
        total = 0
        detail = []
        for s in sorted({r["_segment"] for r in rows}):
            seg_rows = [r for r in rows if r["_segment"] == s]
            a = seg_rows[0]["event_loss"][key_path]
            b = seg_rows[-1]["event_loss"][key_path]
            total += max(0, b - a)
            detail.append(f"{os.path.basename(s)[:24]}:{a}→{b}")
        return total, " | ".join(detail)

    d_ef, det_ef = seg_delta("capture_emit_fail")
    d_pl, det_pl = seg_delta("perf_lost_events")
    record("PASS" if d_ef == 0 else "FAIL", "D1. capture emit_fail",
           f"分段累计净增 {d_ef}（{det_ef}）")
    record("PASS" if d_pl == 0 else "FAIL", "D2. perf lost_events",
           f"分段累计净增 {d_pl}（{det_pl}）")

    max_er = max((r["event_loss"]["capture_emit_failure_ratio"] for r in rows), default=0)
    max_lr = max((r["event_loss"]["perf_delivery_loss_ratio"] for r in rows), default=0)
    record("PASS" if max_er <= EMIT_FAIL_RATIO_LIMIT else "FAIL",
           "D3. emit 失败率峰值", f"{max_er:.4f}（门禁 {EMIT_FAIL_RATIO_LIMIT}）")
    record("PASS" if max_lr <= PERF_LOSS_RATIO_LIMIT else "FAIL",
           "D4. perf 投递丢失率峰值", f"{max_lr:.4f}（门禁 {PERF_LOSS_RATIO_LIMIT}）")

    # ── E. Tracker 健康 ────────────────────────────────────────────────────
    # 同 D：tracker 计数在进程重启后归零，须分段计算净增再求和
    def tracker_delta(key):
        total = 0
        for s in sorted({r["_segment"] for r in rows}):
            seg_rows = [r for r in rows if r["_segment"] == s]
            total += max(0, seg_rows[-1]["tracker"][key] - seg_rows[0]["tracker"][key])
        return total

    d_um, d_lt, d_am = (tracker_delta("unmatched"),
                        tracker_delta("late"),
                        tracker_delta("ambiguous"))
    record("PASS" if d_um <= TRACKER_GROWTH_LIMIT else "FAIL",
           "E1. unmatched 增长", f"分段累计净增 {d_um}（上限 {TRACKER_GROWTH_LIMIT}）")
    record("PASS" if d_lt <= TRACKER_GROWTH_LIMIT else "FAIL",
           "E2. late 增长", f"分段累计净增 {d_lt}（上限 {TRACKER_GROWTH_LIMIT}）")
    record("PASS" if d_am == 0 else "FAIL",
           "E3. ambiguous", f"分段累计净增 {d_am}（匹配歧义应为 0）")

    # ── F. 状态抖动 ────────────────────────────────────────────────────────
    # transitions 是**每段内累计**的计数器，跨段应求和而非取末值
    trans = sum(
        [r for r in rows if r["_segment"] == s][-1].get("transitions", 0)
        for s in sorted({r["_segment"] for r in rows})
    )
    budget = TRANSITION_BUDGET_PER_DAY * max(active_h / 24.0, 1.0 / 24.0)
    record("PASS" if trans <= max(budget, 1) else "FAIL",
           "F. 状态跃迁（flapping）",
           f"分段累计 {trans} 次（按 {active_h:.2f}h 折算预算 {budget:.1f} 次）")

    # ── G. 探测调度 ────────────────────────────────────────────────────────
    # 周期语义（查证 server.cpp:913 start_active_probe_thread）：
    #     runProbeRound(); sleep(interval_ms);
    # 即先跑完一整轮**再**计时，所以轮间隔 = interval + 一轮探测耗时。
    # 实测一轮含 3 个 capability 目标 + 4 个 oracle 端点（各有
    # DNS/TCP/TLS 阶段），耗时 3~8s，故期望轮间隔约 33~38s。
    # 判据必须按这个模型，而不是简单等于配置的 30s。
    gaps = [r["probe_interval_avg_s"] for r in rows if r.get("probe_interval_avg_s")]
    if not gaps:
        record("SKIP", "G. 探测调度稳定性", "窗口内 oracle 轮次不足，未能测出轮间隔")
    else:
        g_lo, g_hi = min(gaps), max(gaps)
        # 期望区间：interval + [最小轮耗时, 最大轮耗时] ± 容差
        exp_lo = ROUND_DURATION_MIN_S
        exp_hi = ROUND_DURATION_MAX_S + PROBE_INTERVAL_TOL
        ok_lo = exp_lo - PROBE_INTERVAL_TOL <= g_lo
        ok_hi = g_hi <= exp_hi
        detail = (f"轮间隔 {g_lo}~{g_hi}s"
                  f"（期望 interval {PROBE_INTERVAL_TARGET:.0f}s + 轮耗时 "
                  f"{ROUND_DURATION_MIN_S:.0f}~{ROUND_DURATION_MAX_S:.0f}s）")
        if ok_lo and ok_hi:
            record("PASS", "G. 探测调度稳定性", detail + " —— 符合 interval+轮耗时 模型")
        else:
            record("FAIL", "G. 探测调度稳定性", detail + " —— 偏离调度模型")

    # ── H. 数据库 ──────────────────────────────────────────────────────────
    s_db = slope_per_hour(rows, lambda r: r["db"]["db_size_kb"])
    lo_db, hi_db = rng(rows, lambda r: r["db"]["db_size_kb"])
    if s_db is None:
        record("SKIP", "H. 数据库增长", "样本不足")
    else:
        per_h_mb = s_db / 1024.0
        record("PASS" if abs(per_h_mb) <= DB_GROWTH_MB_PER_H_LIMIT else "FAIL",
               "H. 数据库增长",
               f"斜率 {per_h_mb:+.2f} MB/h（{lo_db}~{hi_db} KB）")

    # ── I. D-Bus 信号 ──────────────────────────────────────────────────────
    # 已由采集器归一化为「每分钟信号数」，与 journal 窗口长度无关，可跨时段比较。
    sig = [r.get("dbus_signal_per_min", 0.0) for r in rows]
    lo_s, hi_s = min(sig), max(sig)
    avg_s = sum(sig) / len(sig) if sig else 0.0
    # 健康系统信号速率应平稳；某窗口暴增数倍即为 storm 征兆
    storm = hi_s > max(avg_s * 5, 60)
    record("FAIL" if storm else "PASS", "I. D-Bus 信号平稳性",
           f"均值 {avg_s:.2f} 峰值 {hi_s:.2f} 次/分（范围 {lo_s:.2f}~{hi_s:.2f}）")

    # ── 汇总 ───────────────────────────────────────────────────────────────
    print("-" * 72)
    print(f"{'结论':<6} {'审计项':<28} 详情")
    print("-" * 72)
    for name, detail in PASS:
        print(f"\033[32mPASS\033[0m   {name:<28} {detail}")
    for name, detail in SKIP:
        print(f"\033[33mSKIP\033[0m   {name:<28} {detail}")
    for name, detail in FAIL:
        print(f"\033[31mFAIL\033[0m   {name:<28} {detail}")
    print("-" * 72)
    print(f"PASS={len(PASS)}  FAIL={len(FAIL)}  SKIP={len(SKIP)}")
    print()

    # ── 最终裁决 ───────────────────────────────────────────────────────────
    # 判据用 active_h（各段有效运行之和）而非 span_h（含停机的时间跨度）：
    # 长稳的目标是"设备有效运行 24h 无泄漏/无退化"，停机期间系统并未被观测，
    # 不应算作已完成的长稳时长。
    if args.min_hours and active_h < args.min_hours:
        print(f"⚠️  累计有效观测 {active_h:.2f}h < 要求 {args.min_hours}h "
              f"—— 仅趋势自检，不下最终结论。")
        return 0

    if FAIL:
        print("❌ 长稳审计未通过：存在 FAIL 项，v1 不得据此升级为 FROZEN。")
        return 1

    if SKIP:
        print("⚠️  长稳审计存在 SKIP 项：证据不完整，建议补齐后重跑。")
        return 0

    print("✅ 长稳审计全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
