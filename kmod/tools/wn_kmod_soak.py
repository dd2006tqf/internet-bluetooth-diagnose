#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
wn_kmod_soak.py - WeakNet 内核模块长稳测试

持续高频读取两个模块的 mmap 快照，同时记录：
  - 读取成功/失败/重试次数
  - 撕裂检测（字段粒度、单调性）
  - 每次采样的 seq 单调性（内核是否持续发布）
  - 进程自身 RSS
  - 系统内存 / slab 变化

用法：
  sudo python3 wn_kmod_soak.py --duration 1800 --interval 0
  sudo python3 wn_kmod_soak.py --duration 600 --interval 0.001   # 高频压测

设计要点：
  - 与 wn_kmod_read.c 用同一套 seqlock 读者逻辑，但纯 Python 实现，
    避免 C 工具本身的问题掩盖模块问题
  - 每 N 次采样打一行进度，便于中途观察
  - 退出时输出汇总，非零退出码表示发现问题
"""

import argparse
import mmap
import os
import struct
import sys
import time

MAX_CPUS = 256
NR_SOFTIRQ = 10

HDR = struct.Struct("<QQII")          # seq, sample_ns, abi_version, nr_cpus
CPU = struct.Struct("<IIIIQQQQQQQQQQ")  # cpu_id, online, valid, _pad, 10 x u64
# wn_softirq_stat 只有 cpu_id + online + cnt[10]，没有 valid/_pad，
# 因此是 16 + 80 = 96 字节？不是——见下面注释。
#
# 实测：wn_softirq_stat = cpu_id(4) + online(4) + cnt[10]*8(80) = 88 字节，
# 不是 96。两个 u32 之后直接就是 u64 数组，不需要补到 8 字节边界
# （起始偏移 8 已经对齐）。写成 96 会让总尺寸从 22552 变成 24600，
# 超过模块 PAGE_ALIGN 后的 24576，mmap 直接被 EINVAL 拒绝。
SOFTIRQ = struct.Struct("<IIQQQQQQQQQQ")  # cpu_id, online, cnt[10]

TICK_NS = 4_000_000  # CONFIG_HZ=250 -> TICK_NSEC


def read_statm_rss_kb():
    try:
        with open("/proc/self/statm") as f:
            return int(f.read().split()[1]) * (os.sysconf("SC_PAGE_SIZE") // 1024)
    except OSError:
        return -1


def read_meminfo_kb(field):
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith(field + ":"):
                    return int(line.split()[1])
    except OSError:
        pass
    return -1


class Map:
    def __init__(self, path, struct_t, max_entries):
        self.path = path
        self.size = HDR.size + max_entries * struct_t.size
        self.fd = os.open(path, os.O_RDONLY)
        self.mm = mmap.mmap(self.fd, self.size, prot=mmap.PROT_READ)
        self.mv = memoryview(self.mm)
        self.entry = struct_t

    def snapshot(self, max_retry=200000):
        """seqlock 读者：拷贝 -> 校验 seq -> 不一致重试。"""
        for _ in range(max_retry):
            seq = HDR.unpack_from(self.mm, 0)[0]
            if seq & 1:
                continue
            blob = bytes(self.mv[0:self.size])
            if HDR.unpack_from(blob, 0)[0] == seq:
                return seq, blob
        return None, None

    def close(self):
        self.mv.release()
        self.mm.close()
        os.close(self.fd)


def parse_cpu(blob):
    seq, sample_ns, abi, n = HDR.unpack_from(blob, 0)
    out = []
    for i in range(n):
        r = CPU.unpack_from(blob, HDR.size + i * CPU.size)
        out.append({
            "cpu": r[0], "online": r[1], "valid": r[2],
            "vals": r[4:14],  # user nice system idle iowait irq softirq steal guest guest_nice
        })
    return seq, sample_ns, abi, n, out


def parse_softirq(blob):
    seq, sample_ns, abi, n = HDR.unpack_from(blob, 0)
    out = []
    for i in range(n):
        r = SOFTIRQ.unpack_from(blob, HDR.size + i * SOFTIRQ.size)
        out.append({"cpu": r[0], "online": r[1], "cnt": r[2:12]})
    return seq, sample_ns, abi, n, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=int, default=1800, help="秒")
    ap.add_argument("--interval", type=float, default=0.0, help="秒，0=不睡")
    ap.add_argument("--progress", type=int, default=20000, help="每 N 次采样报一次")
    ap.add_argument("--cpu-dev", default="/dev/weaknet_cpu_stat")
    ap.add_argument("--softirq-dev", default="/dev/weaknet_softirq")
    args = ap.parse_args()

    stats = {
        "cpu_ok": 0, "cpu_fail": 0, "softirq_ok": 0, "softirq_fail": 0,
        "grain_violations": 0, "mono_violations": 0,
        "softirq_mono_violations": 0,
        "seq_backwards": 0, "abi_errors": 0,
        "first_seq": None, "last_seq": None,
        "first_sample_ns": None, "last_sample_ns": None,
    }
    examples = []

    cpu_map = Map(args.cpu_dev, CPU, MAX_CPUS)
    si_map = Map(args.softirq_dev, SOFTIRQ, MAX_CPUS)

    rss_start = read_statm_rss_kb()
    mem_start = read_meminfo_kb("MemAvailable")
    slab_start = read_meminfo_kb("Slab")

    prev_cpu = {}
    prev_si = {}
    prev_seq = None
    t0 = time.time()
    deadline = t0 + args.duration
    iters = 0
    last_progress = t0

    try:
        while time.time() < deadline:
            iters += 1

            seq, blob = cpu_map.snapshot()
            if blob is None:
                stats["cpu_fail"] += 1
            else:
                stats["cpu_ok"] += 1
                cseq, sample_ns, abi, n, recs = parse_cpu(blob)
                if abi != 2:
                    stats["abi_errors"] += 1
                    if len(examples) < 5:
                        examples.append(("abi", abi))
                if stats["first_seq"] is None:
                    stats["first_seq"] = cseq
                    stats["first_sample_ns"] = sample_ns
                stats["last_seq"] = cseq
                stats["last_sample_ns"] = sample_ns

                if prev_seq is not None and cseq < prev_seq:
                    stats["seq_backwards"] += 1
                    if len(examples) < 5:
                        examples.append(("seq_back", cseq, prev_seq))
                prev_seq = cseq

                for r in recs:
                    cid = r["cpu"]
                    v = r["vals"]
                    # 粒度：busy 类必须是 TICK_NS 整数倍
                    for k in (0, 1, 2, 5, 6, 7, 8, 9):
                        if v[k] % TICK_NS:
                            stats["grain_violations"] += 1
                            if len(examples) < 5:
                                examples.append(("grain", cid, k, v[k]))
                    # 单调性：仅当 valid 且上一轮也存在时检查
                    if r["valid"] and cid in prev_cpu:
                        pv = prev_cpu[cid]
                        for k in range(10):
                            if v[k] < pv[k]:
                                stats["mono_violations"] += 1
                                if len(examples) < 5:
                                    examples.append(("mono", cid, k, v[k], pv[k]))
                    prev_cpu[cid] = v

            seq2, blob2 = si_map.snapshot()
            if blob2 is None:
                stats["softirq_fail"] += 1
            else:
                stats["softirq_ok"] += 1
                sseq, ssample_ns, sabi, sn, srecs = parse_softirq(blob2)
                if sabi != 2:
                    stats["abi_errors"] += 1
                # softirq 没有 valid 字段，单调性按「只降不升即违规」判定。
                # CPU 离线时计数冻结（实测确认），不会倒退；重新上线后
                # kstat_softirqs_cpu 的累计值仍然保留，因此这里可以严格检查。
                for r in srecs:
                    cid = r["cpu"]
                    c = r["cnt"]
                    if cid in prev_si:
                        pv = prev_si[cid]
                        for k in range(NR_SOFTIRQ):
                            if c[k] < pv[k]:
                                stats["softirq_mono_violations"] += 1
                                if len(examples) < 5:
                                    examples.append(("si_mono", cid, k, c[k], pv[k]))
                    prev_si[cid] = c

            now = time.time()
            if (iters % args.progress == 0) or (now - last_progress >= 60):
                last_progress = now
                elapsed = now - t0
                total = stats["cpu_ok"] + stats["cpu_fail"]
                print("[%6.0fs] iters=%d cpu_ok=%d fail=%d 撕裂=%d seq_back=%d rss=%dkB"
                      % (elapsed, iters, stats["cpu_ok"], stats["cpu_fail"],
                         stats["grain_violations"] + stats["mono_violations"],
                         stats["seq_backwards"], read_statm_rss_kb()),
                      flush=True)

            if args.interval > 0:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n中断")

    elapsed = time.time() - t0
    rss_end = read_statm_rss_kb()
    mem_end = read_meminfo_kb("MemAvailable")
    slab_end = read_meminfo_kb("Slab")

    print("\n" + "=" * 60)
    print("长稳测试汇总")
    print("=" * 60)
    print("时长            : %.1f s" % elapsed)
    print("迭代次数        : %d (%.0f 次/秒)" % (iters, iters / elapsed if elapsed else 0))
    print("cpu 读取        : 成功 %d / 失败 %d" % (stats["cpu_ok"], stats["cpu_fail"]))
    print("softirq 读取    : 成功 %d / 失败 %d" % (stats["softirq_ok"], stats["softirq_fail"]))
    print("粒度违规        : %d" % stats["grain_violations"])
    print("单调性违规      : %d" % stats["mono_violations"])
    print("softirq 单调违规: %d" % stats["softirq_mono_violations"])
    print("seq 倒退        : %d" % stats["seq_backwards"])
    print("ABI 错误        : %d" % stats["abi_errors"])
    if stats["first_seq"] is not None:
        delta_ns = stats["last_sample_ns"] - stats["first_sample_ns"]
        print("内核采样推进    : seq %d -> %d, 时间 %.1f s"
              % (stats["first_seq"], stats["last_seq"], delta_ns / 1e9))
        print("实际耗时        : %.1f s" % elapsed)
        if elapsed > 0:
            print("采样节拍        : 期望 1.0 Hz, 实测 %.3f Hz"
                  % (delta_ns / 1e9 / elapsed))
    print("进程 RSS        : %d -> %d kB (Δ%+d)" % (rss_start, rss_end, rss_end - rss_start))
    print("MemAvailable    : %d -> %d kB (Δ%+d)" % (mem_start, mem_end, mem_end - mem_start))
    print("Slab            : %d -> %d kB (Δ%+d)" % (slab_start, slab_end, slab_end - slab_start))

    if examples:
        print("\n问题样例（最多 5 条）:")
        for e in examples:
            print("  ", e)

    cpu_map.close()
    si_map.close()

    bad = (stats["cpu_fail"] + stats["softirq_fail"] + stats["grain_violations"]
           + stats["mono_violations"] + stats["softirq_mono_violations"]
           + stats["seq_backwards"] + stats["abi_errors"])
    print("\n结论: %s" % ("PASS" if bad == 0 else "FAIL (%d 处异常)" % bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
