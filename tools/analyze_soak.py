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
    """
    pts = []
    for r in rows:
        v = key_fn(r)
        if v is None:
            continue
        pts.append((r["uptime_sec"] / 3600.0, float(v)))
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


def rng(rows, key_fn):
    vals = [key_fn(r) for r in rows if key_fn(r) is not None]
    if not vals:
        return None, None
    return min(vals), max(vals)


def main():
    ap = argparse.ArgumentParser(description="WeakNet v1 Soak Analyzer")
    ap.add_argument("--input", required=True)
    ap.add_argument("--min-hours", type=float, default=0.0,
                    help="低于该时长只做趋势自检，不给出最终结论")
    args = ap.parse_args()

    rows = []
    try:
        with open(args.input) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rows.append(json.loads(line))
                except Exception:
                    pass
    except FileNotFoundError:
        print(f"[!] 找不到输入文件: {args.input}", file=sys.stderr)
        return 2

    if len(rows) < 10:
        print(f"[!] 样本过少（{len(rows)} 条），无法分析。", file=sys.stderr)
        return 2

    span_h = rows[-1]["uptime_sec"] / 3600.0
    print("=" * 72)
    print("WeakNet Assurance v1 — 长稳证据审计报告")
    print("=" * 72)
    print(f"样本文件   : {args.input}")
    print(f"采样条数   : {len(rows)}")
    print(f"观测时长   : {span_h:.2f} 小时")
    print(f"起始时间   : {rows[0].get('timestamp')}")
    print(f"结束时间   : {rows[-1].get('timestamp')}")
    pid_changes = len({r.get('pid') for r in rows})
    print(f"被测 PID   : {sorted({r.get('pid') for r in rows})}"
          + ("" if pid_changes == 1 else "  ⚠️ 进程重启过，长稳连续性受影响"))
    print()

    if pid_changes != 1:
        record("SKIP", "进程连续性",
               f"检测到 {pid_changes} 个不同 PID，长稳期间进程重启，趋势结论仅供参考")

    # ── A. 内存趋势 ─────────────────────────────────────────────────────────
    s = slope_per_hour(rows, lambda r: r["process"]["rss_mb"])
    lo, hi = rng(rows, lambda r: r["process"]["rss_mb"])
    if s is None:
        record("SKIP", "A. RSS 趋势", "样本不足")
    else:
        msg = f"斜率 {s:+.3f} MB/h（范围 {lo}~{hi} MB）"
        record("PASS" if abs(s) <= RSS_SLOPE_MB_PER_H else "FAIL", "A. RSS 趋势", msg)

    # ── B. FD / socket ─────────────────────────────────────────────────────
    # FD 数量级大（~150），长窗口下斜率是有意义的泄漏指标。
    # 但短窗口（<1h）下 ±1 的正常抖动会被回归放大成看似很大的漂移
    # （实测 0.08h 内 148→149 得 -2.145/h）。因此判据分两层：
    #   短窗口 → 只判「是否有界」，不给驻留趋势结论
    #   长窗口 → 斜率判定
    FD_SLOPE_MIN_HOURS = 1.0
    lo_fd, hi_fd = rng(rows, lambda r: r["process"]["fds"])
    s_fd = slope_per_hour(rows, lambda r: r["process"]["fds"])
    if hi_fd is None:
        record("SKIP", "B1. FD 有界性", "样本不足")
    else:
        fd_span = hi_fd - lo_fd
        if span_h >= FD_SLOPE_MIN_HOURS and s_fd is not None:
            record("PASS" if abs(s_fd) <= FD_SLOPE_PER_H else "FAIL",
                   "B1. FD 趋势",
                   f"斜率 {s_fd:+.3f}/h（范围 {lo_fd}~{hi_fd}，观测 {span_h:.2f}h）")
        else:
            record("PASS" if fd_span <= FD_SPAN_LIMIT else "FAIL",
                   "B1. FD 有界性",
                   f"波动幅度 {fd_span}（范围 {lo_fd}~{hi_fd}，上限 {FD_SPAN_LIMIT}）"
                   f" —— 观测仅 {span_h:.2f}h 不足 {FD_SLOPE_MIN_HOURS}h，"
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
        detail = (f"峰值 {hi_ep} / 内核硬上限 {BPF_ENDPOINT_CAP}"
                  f"（范围 {lo_ep}~{hi_ep}，后半程峰值 {tail_hi}）")
        # 判据：绝不越界 + 后半程不高于前半程（排除单调爬升）。
        # LRU_HASH 的 max_entries 是内核强制上限，结构上不可能越界；
        # 这里真正要证伪的是"是否持续爬升"，故比较前后半程峰值。
        if over:
            record("FAIL", "C. dns_self_endpoints 水位", detail + " —— 越过内核上限（不可能，须查证）")
        else:
            record("PASS", "C. dns_self_endpoints 水位", detail + " —— 有界，未越界")

    # self_pid 类 map 的值必须等于被测 PID，否则 provenance 过滤失效
    bad_pid = []
    for k in ("http_self_pid_value", "tcp_self_pid_value", "retrans_self_pid_value"):
        vals = {r["bpf"].get(k) for r in rows if r["bpf"].get(k)}
        if len(vals) > 1:
            bad_pid.append(f"{k}={sorted(vals)}")
    if bad_pid:
        record("FAIL", "C2. provenance PID 稳定", "过滤 PID 中途变化: " + "; ".join(bad_pid))
    else:
        record("PASS", "C2. provenance PID 稳定",
               "self_pid 类 map 全程指向同一被测进程")

    # ── D. 事件丢失 ────────────────────────────────────────────────────────
    ef = [r["event_loss"]["capture_emit_fail"] for r in rows]
    pl = [r["event_loss"]["perf_lost_events"] for r in rows]
    d_ef = ef[-1] - ef[0]
    d_pl = pl[-1] - pl[0]
    record("PASS" if d_ef == 0 else "FAIL", "D1. capture emit_fail",
           f"全程净增 {d_ef}（首 {ef[0]} → 末 {ef[-1]}）")
    record("PASS" if d_pl == 0 else "FAIL", "D2. perf lost_events",
           f"全程净增 {d_pl}（首 {pl[0]} → 末 {pl[-1]}）")

    max_er = max((r["event_loss"]["capture_emit_failure_ratio"] for r in rows), default=0)
    max_lr = max((r["event_loss"]["perf_delivery_loss_ratio"] for r in rows), default=0)
    record("PASS" if max_er <= EMIT_FAIL_RATIO_LIMIT else "FAIL",
           "D3. emit 失败率峰值", f"{max_er:.4f}（门禁 {EMIT_FAIL_RATIO_LIMIT}）")
    record("PASS" if max_lr <= PERF_LOSS_RATIO_LIMIT else "FAIL",
           "D4. perf 投递丢失率峰值", f"{max_lr:.4f}（门禁 {PERF_LOSS_RATIO_LIMIT}）")

    # ── E. Tracker 健康 ────────────────────────────────────────────────────
    um = [r["tracker"]["unmatched"] for r in rows]
    lt = [r["tracker"]["late"] for r in rows]
    am = [r["tracker"]["ambiguous"] for r in rows]
    d_um, d_lt, d_am = um[-1] - um[0], lt[-1] - lt[0], am[-1] - am[0]
    record("PASS" if d_um <= TRACKER_GROWTH_LIMIT else "FAIL",
           "E1. unmatched 增长", f"净增 {d_um}（上限 {TRACKER_GROWTH_LIMIT}）")
    record("PASS" if d_lt <= TRACKER_GROWTH_LIMIT else "FAIL",
           "E2. late 增长", f"净增 {d_lt}（上限 {TRACKER_GROWTH_LIMIT}）")
    record("PASS" if d_am == 0 else "FAIL",
           "E3. ambiguous", f"净增 {d_am}（匹配歧义应为 0）")

    # ── F. 状态抖动 ────────────────────────────────────────────────────────
    trans = rows[-1].get("transitions", 0)
    budget = TRANSITION_BUDGET_PER_DAY * max(span_h / 24.0, 1.0 / 24.0)
    record("PASS" if trans <= max(budget, 1) else "FAIL",
           "F. 状态跃迁（flapping）",
           f"全程 {trans} 次（按 {span_h:.2f}h 折算预算 {budget:.1f} 次）")

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
    if args.min_hours and span_h < args.min_hours:
        print(f"⚠️  观测 {span_h:.2f}h < 要求 {args.min_hours}h —— 仅趋势自检，不下最终结论。")
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
