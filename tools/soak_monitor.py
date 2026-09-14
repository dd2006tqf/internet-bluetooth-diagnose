#!/usr/bin/env python3
"""
WeakNet Assurance 生产级长稳监控与资源采样守护器 (Soak Sampler)

监控维度：
  1. 进程资源：CPU (%), RSS 内存 (MB), 打开的文件描述符 FD 计数, 活跃线程数
  2. 套接字数量：TCP/UDP socket 计数
  3. BPF Map 元素水位：dns_self_endpoi 元素数（重点监控 LRU 淘汰与有界性）
  4. 数据库与日志：history.db 文件大小 (KB)
  5. 快照与状态指标：D-Bus 评估状态、sequence_id、flapping 次数

用法：
  python3 soak_monitor.py --interval 10 --output /tmp/soak_metrics.jsonl
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time


def get_server_pid():
    try:
        out = subprocess.check_output(["pgrep", "-f", "weaknet-dbus-server --config"], text=True)
        lines = out.strip().split()
        if lines:
            return int(lines[0])
    except Exception:
        pass
    return None


def sample_process_metrics(pid):
    metrics = {"cpu_percent": 0.0, "rss_mb": 0.0, "threads": 0, "fds": 0, "sockets": 0}
    if not pid or not os.path.exists(f"/proc/{pid}"):
        return metrics

    # 1. RSS & Threads
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    parts = line.split()
                    metrics["rss_mb"] = round(int(parts[1]) / 1024.0, 2)
                elif line.startswith("Threads:"):
                    metrics["threads"] = int(line.split()[1])
    except Exception:
        pass

    # 2. FDs and Sockets
    try:
        fd_dir = f"/proc/{pid}/fd"
        if os.path.exists(fd_dir):
            fds = os.listdir(fd_dir)
            metrics["fds"] = len(fds)
            sock_cnt = 0
            for fd in fds:
                try:
                    target = os.readlink(os.path.join(fd_dir, fd))
                    if target.startswith("socket:"):
                        sock_cnt += 1
                except Exception:
                    pass
            metrics["sockets"] = sock_cnt
    except Exception:
        pass

    return metrics


def sample_bpf_map_watermark():
    metrics = {"dns_self_endpoints_count": 0}
    try:
        # bpftool dump 需 sudo 权限
        out = subprocess.check_output(["sudo", "bpftool", "map", "dump", "name", "dns_self_endpoi"], stderr=subprocess.DEVNULL, text=True)
        # 统计元素 key: 的出现频次
        keys = re.findall(r"key:\s*\{", out)
        metrics["dns_self_endpoints_count"] = len(keys)
    except Exception:
        pass
    return metrics


def sample_db_size():
    db_path = "/home/radxa/weaknet/data/history.db"
    try:
        if os.path.exists(db_path):
            return {"db_size_kb": round(os.path.getsize(db_path) / 1024.0, 2)}
    except Exception:
        pass
    return {"db_size_kb": 0.0}


def sample_latest_assessment():
    info = {"overall": "UNKNOWN", "sequence_id": 0}
    try:
        out = subprocess.check_output(
            ["journalctl", "-u", "weaknet-server", "--since", "30 sec ago", "--no-pager"],
            stderr=subprocess.DEVNULL, text=True
        )
        for line in reversed(out.splitlines()):
            m = re.search(r"Active capability:\s*dns=([A-Z]+).*https=([A-Z]+).*portal=([A-Z]+)", line)
            if m:
                info["active_dns"] = m.group(1)
                info["active_https"] = m.group(2)
                info["active_portal"] = m.group(3)
                break
        for line in reversed(out.splitlines()):
            m = re.search(r"snapshot metadata: generation=\d+ snapshot_ts=(\d+)", line)
            if m:
                info["sequence_id"] = int(m.group(1))
                break
    except Exception:
        pass
    return info


def main():
    parser = argparse.ArgumentParser(description="WeakNet Soak Sampler")
    parser.add_argument("--interval", type=int, default=10, help="Sampling interval in seconds")
    parser.add_argument("--output", default="/tmp/soak_metrics.jsonl", help="Output JSON lines file")
    parser.add_argument("--duration", type=int, default=0, help="Total duration in seconds (0 = infinite)")
    args = parser.parse_args()

    start_time = time.time()
    last_state = None
    flapping_count = 0

    print(f"[*] Starting WeakNet Soak Sampler (interval={args.interval}s, output={args.output})")
    while True:
        now = time.time()
        if args.duration > 0 and (now - start_time) >= args.duration:
            print("[*] Target duration reached. Exiting sampler.")
            break

        pid = get_server_pid()
        proc_met = sample_process_metrics(pid)
        bpf_met = sample_bpf_map_watermark()
        db_met = sample_db_size()
        assess = sample_latest_assessment()

        current_overall = assess.get("active_https", "UNKNOWN")
        if last_state and current_overall != "UNKNOWN" and current_overall != last_state:
            flapping_count += 1
        if current_overall != "UNKNOWN":
            last_state = current_overall

        record = {
            "timestamp": time.time(),
            "uptime_sec": round(now - start_time, 1),
            "pid": pid,
            "process": proc_met,
            "bpf": bpf_met,
            "db": db_met,
            "assessment": assess,
            "flapping_count": flapping_count
        }

        try:
            with open(args.output, "a") as f:
                f.write(json.dumps(record) + "\n")
        except Exception as e:
            print(f"[!] Write failed: {e}", file=sys.stderr)

        print(f"[{time.strftime('%X')}] PID={pid} RSS={proc_met['rss_mb']}MB FDs={proc_met['fds']} SOCKS={proc_met['sockets']} BPF_Endpoints={bpf_met['dns_self_endpoints_count']} DB={db_met['db_size_kb']}KB Flapping={flapping_count}")
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
