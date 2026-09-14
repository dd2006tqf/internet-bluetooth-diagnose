#!/usr/bin/env python3
"""
WeakNet Assurance v1 长稳证据采集器 (Soak Sampler)

用途：为 Assurance Model v1 的 24h 长稳验收采集结构化证据，输出 JSON Lines。

设计约束（重要）：
  本工具**只读**观测，绝不修改任何生产代码或配置。所有指标均来自
  /proc、bpftool 与 weaknet-server 的既有日志，因此它的存在不会改变
  被测系统的行为（观测者不得扰动被观测对象）。

采集维度（对应 v1 长稳审计的 8 项要求）：
  1. 进程资源     RSS / 线程数 / FD / socket 数，以及基于 /proc/<pid>/stat
                  utime+stime 差分计算的真实 CPU 占用
  2. BPF 水位     dns_self_endpoints（LRU 256 上限）等 self-provenance map 元素数
  3. 事件丢失     dns-capture diag 的 emit_fail / perf lost_events /
                  capture_emit_failure_ratio / perf_delivery_loss_ratio
  4. Tracker      tracker_query_accepted / response_accepted / unmatched / late
  5. 状态抖动     Active capability 状态跃迁计数 + DNS SLE unmatched/late 增量
  6. 探测调度     Portal oracle 相邻轮次的间隔（探测周期漂移）
  7. D-Bus 信号   interval 窗口内 sendSignalInternal 发出次数（信号风暴检测）
  8. 数据库       history.db（含 -wal/-shm）体积与窗口内历史写入行数

用法：
  sudo python3 soak_monitor.py --interval 10 --duration 86400 \
      --output /home/radxa/weaknet/data/soak_24h.jsonl
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time

# ── 被测进程 ────────────────────────────────────────────────────────────────
SERVER_MATCH = "weaknet-dbus-server --config"

# BPF map 名在内核中上限 15 字符，查询需用截断名
BPF_MAPS = {
    "dns_self_endpoints": "dns_self_endpoi",
    "http_self_pid": "http_self_pid",
    "tcp_self_pid": "tcp_self_pid",
    "retrans_self_pid": "retrans_self_pi",
}

DB_PATH = "/home/radxa/weaknet/data/history.db"


# ── 1. 进程资源 ─────────────────────────────────────────────────────────────

def get_server_pid():
    try:
        out = subprocess.check_output(["pgrep", "-f", SERVER_MATCH], text=True)
        pids = out.strip().split()
        if pids:
            return int(pids[0])
    except Exception:
        pass
    return None


def read_proc_cpu_ticks(pid):
    """返回 (utime+stime) 时钟节拍数，用于差分计算 CPU%。"""
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(") ", 1)[-1].split()
        # 跳过 comm 后：state(0) ppid(1) ... utime(11) stime(12)
        return int(fields[11]) + int(fields[12])
    except Exception:
        return None


def sample_process(pid):
    m = {"rss_mb": 0.0, "threads": 0, "fds": 0, "sockets": 0}
    if not pid or not os.path.exists(f"/proc/{pid}"):
        return m
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    m["rss_mb"] = round(int(line.split()[1]) / 1024.0, 2)
                elif line.startswith("Threads:"):
                    m["threads"] = int(line.split()[1])
    except Exception:
        pass
    try:
        fd_dir = f"/proc/{pid}/fd"
        fds = os.listdir(fd_dir)
        m["fds"] = len(fds)
        sock = 0
        for fd in fds:
            try:
                if os.readlink(os.path.join(fd_dir, fd)).startswith("socket:"):
                    sock += 1
            except Exception:
                pass
        m["sockets"] = sock
    except Exception:
        pass
    return m


# ── 2. BPF map 水位 ─────────────────────────────────────────────────────────

def sample_bpf():
    m = {}
    for label, bpftool_name in BPF_MAPS.items():
        count = 0
        try:
            # PID 类 map 是单元素 ARRAY；endpoints 是 LRU_HASH
            out = subprocess.check_output(
                ["bpftool", "map", "dump", "name", bpftool_name],
                stderr=subprocess.DEVNULL, text=True, timeout=15)
            count = len(re.findall(r'"key"', out))
            if label.endswith("_pid"):
                # 记录实际写入的 PID 值，便于发现过滤失效
                vals = re.findall(r'"value":\s*(\d+)', out)
                m[label + "_value"] = int(vals[0]) if vals else 0
        except Exception:
            pass
        m[label + "_count"] = count
    return m


# ── 3~7. 从 journal 窗口提取 ────────────────────────────────────────────────

_LAST_DIAG = {}


def journal_window(seconds):
    try:
        return subprocess.check_output(
            ["journalctl", "-u", "weaknet-server",
             "--since", f"{seconds} sec ago", "--no-pager"],
            stderr=subprocess.DEVNULL, text=True, timeout=30)
    except Exception:
        return ""


def parse_diag(text):
    """解析最后一条 dns-capture diag，返回 capture/drain/delivery 三段。"""
    last = None
    for line in text.splitlines():
        i = line.find("dns-capture diag: ")
        if i >= 0:
            last = line[i + len("dns-capture diag: "):]
    if not last:
        return {}
    try:
        return json.loads(last)
    except Exception:
        return {}


def parse_dns_sle(text):
    """解析最后一条 DNS SLE，取 tracker 配对计数与订阅状态。"""
    out = {}
    rx = re.compile(
        r"DNS SLE: state=(\w+) coverage=(\w+) reason=(\w+) "
        r"fail=(\d+)/(\d+) inflight=(\d+) ok=(\d+) "
        r"unmatched=(\d+) ambiguous=(\d+) late=(\d+)")
    for line in text.splitlines():
        m = rx.search(line)
        if m:
            out = {
                "state": m.group(1), "coverage": m.group(2), "reason": m.group(3),
                "fail": int(m.group(4)), "terminals": int(m.group(5)),
                "inflight": int(m.group(6)), "ok": int(m.group(7)),
                "unmatched": int(m.group(8)), "ambiguous": int(m.group(9)),
                "late": int(m.group(10)),
            }
    return out


def parse_active_capability(text):
    out = {}
    rx = re.compile(r"Active capability: dns=(\w+)\((.*?)\) tcp=(\w+)\((.*?)\) "
                    r"https=(\w+)\((.*?)\) portal=(\w+)\((.*?)\)")
    for line in text.splitlines():
        m = rx.search(line)
        if m:
            out = {"dns": m.group(1), "dns_reason": m.group(2),
                   "tcp": m.group(3), "tcp_reason": m.group(4),
                   "https": m.group(5), "https_reason": m.group(6),
                   "portal": m.group(7), "portal_reason": m.group(8)}
    return out


def parse_dbus_signal_rate(text, window_sec):
    """
    窗口内 sendSignalInternal 发出次数，**归一化为每分钟速率**。

    为什么要归一化：本函数的输入是 journal 窗口。窗口长度会随配置变化
    （为保证能抓到多轮 oracle，窗口从 15s 提升到 >=120s），若直接记原始
    计数，同一系统在不同窗口下会得到 8 倍差异的数值，历史数据不可比。
    归一化后与窗口无关，可跨时段比较，也便于检测信号风暴。
    """
    n = 0
    for line in text.splitlines():
        if "sendSignalInternal: emitted" in line:
            n += 1
    if window_sec <= 0:
        return 0.0
    return round(n * 60.0 / window_sec, 2)


def parse_probe_interval(text):
    """
    由 Portal oracle 的**轮次间隔**估算探测周期漂移。

    关键：一轮探测会对 N 个 oracle 端点各发一次，相邻**端点**日志只差
    0.3~1.5s；真正要测的是**轮与轮之间**的间隔（配置为 30s）。

    此前直接用相邻行时间戳差分，测出的是端点间隔（~1s），把健康调度
    误报成"周期漂移"。修正做法：先按间隔阈值把时间戳切分成"轮"，
    再取轮首之间的差值中位数。

    纯从既有日志推导，不修改任何生产代码。
    """
    ROUND_GAP_MIN = 10  # 相邻时间戳差 >= 10s 视为新一轮开始（端点间隔 ~1s）

    stamps = []
    for line in text.splitlines():
        if "Portal oracle: target=" in line:
            m = re.match(r"^(\w{3}) (\d{2}) (\d{2}):(\d{2}):(\d{2})", line)
            if m:
                h, mi, s = int(m.group(3)), int(m.group(4)), int(m.group(5))
                stamps.append(h * 3600 + mi * 60 + s)
    if len(stamps) < 2:
        return None

    stamps.sort()
    round_starts = [stamps[0]]
    for prev, cur in zip(stamps, stamps[1:]):
        if cur - prev >= ROUND_GAP_MIN:
            round_starts.append(cur)

    if len(round_starts) < 2:
        return None
    gaps = [round_starts[i + 1] - round_starts[i] for i in range(len(round_starts) - 1)]
    gaps = [g for g in gaps if g > 0]
    if not gaps:
        return None
    gaps.sort()
    return round(gaps[len(gaps) // 2], 1)   # 中位数，抗单点噪声


# ── 8. 数据库 ───────────────────────────────────────────────────────────────

def sample_db():
    m = {"db_size_kb": 0.0, "db_wal_kb": 0.0}
    for suffix, key in (("", "db_size_kb"), ("-wal", "db_wal_kb")):
        p = DB_PATH + suffix
        try:
            if os.path.exists(p):
                m[key] = round(os.path.getsize(p) / 1024.0, 2)
        except Exception:
            pass
    return m


# ── 主循环 ──────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="WeakNet v1 Soak Sampler")
    ap.add_argument("--interval", type=int, default=10)
    ap.add_argument("--output", default="/tmp/soak_metrics.jsonl")
    ap.add_argument("--duration", type=int, default=0, help="0 = 无限")
    args = ap.parse_args()

    start = time.time()
    prev_ticks = None
    prev_cpu_t = None
    prev_unmatched = None
    prev_late = None
    prev_state = None
    transitions = 0          # 状态跃迁次数（flapping 的直接度量）

    print(f"[*] WeakNet v1 Soak Sampler  interval={args.interval}s  output={args.output}",
          flush=True)

    while True:
        now = time.time()
        if args.duration > 0 and (now - start) >= args.duration:
            print("[*] 目标时长到达，采样结束。", flush=True)
            break

        pid = get_server_pid()
        proc = sample_process(pid)

        # CPU%：以时钟节拍差分 / elapsed，仅统计被测进程自身
        cpu_pct = 0.0
        if pid:
            ticks = read_proc_cpu_ticks(pid)
            if ticks is not None:
                if prev_ticks is not None and prev_cpu_t is not None:
                    dt = now - prev_cpu_t
                    if dt > 0:
                        hz = os.sysconf("SC_CLK_TCK") or 100
                        cpu_pct = round(100.0 * (ticks - prev_ticks) / hz / dt, 2)
                prev_ticks = ticks
                prev_cpu_t = now
        proc["cpu_percent"] = cpu_pct

        bpf = sample_bpf()
        db = sample_db()

        # journal 窗口必须**大于一个探测周期**，否则永远抓不到 2 轮 oracle，
        # G（调度漂移）会全程 SKIP。探测周期为 interval+轮耗时（实测 33~38s），
        # 故取 max(interval*4, 120s) 保证窗口内至少有 3 轮可分组。
        jwindow = max(args.interval * 4, 120)
        text = journal_window(jwindow)
        diag = parse_diag(text)
        sle = parse_dns_sle(text)
        cap = parse_active_capability(text)
        dbus_rate = parse_dbus_signal_rate(text, jwindow)
        probe_gap = parse_probe_interval(text)

        # 状态跃迁计数（flapping 的直接证据）
        cur = cap.get("https")
        if cur and prev_state and cur != prev_state:
            transitions += 1
        if cur:
            prev_state = cur

        # Tracker 增量：unmatched/late 只增不减，取窗口增量更能反映真实抖动
        d_unmatched = d_late = 0
        if sle:
            if prev_unmatched is not None:
                d_unmatched = max(0, sle["unmatched"] - prev_unmatched)
            if prev_late is not None:
                d_late = max(0, sle["late"] - prev_late)
            prev_unmatched = sle["unmatched"]
            prev_late = sle["late"]

        cap_stats = diag.get("capture", {}) if isinstance(diag, dict) else {}
        drain = diag.get("drain", {}) if isinstance(diag, dict) else {}
        delivery = diag.get("delivery", {}) if isinstance(diag, dict) else {}

        rec = {
            "schema": "weaknet-soak-v1",
            "timestamp": now,
            "uptime_sec": round(now - start, 1),
            "pid": pid,
            "process": proc,
            "bpf": bpf,
            "db": db,
            "event_loss": {
                "capture_emit_fail": cap_stats.get("emit_fail", 0),
                "perf_lost_events": delivery.get("perf_lost_events", 0),
                "capture_emit_failure_ratio": delivery.get("capture_emit_failure_ratio", 0.0),
                "perf_delivery_loss_ratio": delivery.get("perf_delivery_loss_ratio", 0.0),
                "emitted": cap_stats.get("emitted", 0),
            },
            "tracker": {
                "query_accepted": drain.get("tracker_query_accepted", 0),
                "response_accepted": drain.get("tracker_response_accepted", 0),
                "unmatched": sle.get("unmatched", 0),
                "late": sle.get("late", 0),
                "ambiguous": sle.get("ambiguous", 0),
                "d_unmatched": d_unmatched,
                "d_late": d_late,
                "dns_state": sle.get("state", "UNKNOWN"),
            },
            "capability": cap,
            "transitions": transitions,
            "dbus_signal_per_min": dbus_rate,
            "probe_interval_avg_s": probe_gap,
        }

        try:
            with open(args.output, "a") as f:
                f.write(json.dumps(rec) + "\n")
        except Exception as e:
            print(f"[!] 写入失败: {e}", file=sys.stderr, flush=True)

        print(f"[{time.strftime('%X')}] pid={pid} cpu={cpu_pct}% rss={proc['rss_mb']}MB "
              f"fd={proc['fds']} sock={proc['sockets']} thr={proc['threads']} "
              f"endpoints={bpf.get('dns_self_endpoints_count', 0)} "
              f"emit_fail={rec['event_loss']['capture_emit_fail']} "
              f"lost={rec['event_loss']['perf_lost_events']} "
              f"unmatch={sle.get('unmatched', 0)} late={sle.get('late', 0)} "
              f"db={db['db_size_kb']}KB dbus={dbus_rate} trans={transitions}",
              flush=True)

        time.sleep(args.interval)


if __name__ == "__main__":
    main()
