#!/usr/bin/env python3
"""
DNS ground-truth oracle（P0）

为什么需要独立 oracle：
  此前用 `host -W 2 <name> <resolver>` 的**进程退出码**代表"A 记录解析是否正常"。
  但 host 在不指定 -t 时会额外查询 AAAA/MX 等 RR 类型，那些查询返回 NXDOMAIN，
  整个进程因而 exit=1 —— 于是一个解析器在 A 记录完全正常的情况下被判为"故障"。
  这个错误曾导致对开发板网关 DNS 的错误结论，并污染了验收脚本的判定。

本 oracle 不依赖任何 CLI 语义：
  自行构造 DNS 查询报文，显式指定 QTYPE，直接解析响应中的
  RCODE / ANCOUNT / 答案记录 / 往返时延，因此不会被额外 RR 类型干扰。

用法：
  ./dns_oracle.py --resolver 192.168.137.1 --name www.baidu.com --qtype A
  ./dns_oracle.py --resolver 192.168.137.1 --name www.baidu.com --qtype A --repeat 5
  ./dns_oracle.py --resolver 192.168.137.1 --name no-such-name.invalid --qtype A
  ./dns_oracle.py --resolver 192.168.137.1 --selftest     # 同时测 A/AAAA/NXDOMAIN
"""

import argparse
import random
import socket
import struct
import sys
import time

QTYPE = {"A": 1, "NS": 2, "CNAME": 5, "SOA": 6, "MX": 15, "AAAA": 28}
QTYPE_NAME = {v: k for k, v in QTYPE.items()}

RCODE = {
    0: "NOERROR", 1: "FORMERR", 2: "SERVFAIL", 3: "NXDOMAIN",
    4: "NOTIMP", 5: "REFUSED",
}


def encode_name(name):
    """把域名编码为 DNS wire format。"""
    out = b""
    for label in name.rstrip(".").split("."):
        if not label:
            continue
        b = label.encode("ascii")
        if len(b) > 63:
            raise ValueError("label too long")
        out += bytes([len(b)]) + b
    return out + b"\x00"


def build_query(name, qtype, txid):
    header = struct.pack(">HHHHHH", txid, 0x0100, 1, 0, 0, 0)  # RD=1
    return header + encode_name(name) + struct.pack(">HH", qtype, 1)


def parse_name(buf, off):
    """解码（可能带压缩指针的）域名。"""
    labels = []
    jumped = False
    orig = off
    hops = 0
    while True:
        if hops > 32:
            raise ValueError("compression loop")
        hops += 1
        if off >= len(buf):
            raise ValueError("truncated name")
        ln = buf[off]
        if ln == 0:
            off += 1
            break
        if ln & 0xC0 == 0xC0:
            ptr = struct.unpack(">H", buf[off:off + 2])[0] & 0x3FFF
            if not jumped:
                orig = off + 2
                jumped = True
            off = ptr
            continue
        labels.append(buf[off + 1:off + 1 + ln].decode("ascii", "replace"))
        off += 1 + ln
    return ".".join(labels), (orig if jumped else off)


def parse_response(buf, txid):
    if len(buf) < 12:
        raise ValueError("short response")
    rid, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", buf[:12])
    if rid != txid:
        raise ValueError("txid mismatch")

    rcode = flags & 0x0F
    tc = (flags >> 9) & 1
    qr = (flags >> 15) & 1
    if not qr:
        raise ValueError("not a response")

    answers = []
    off = 12
    for _ in range(qd):
        _, off = parse_name(buf, off)
        off += 4
    for _ in range(an):
        rname, off = parse_name(buf, off)
        if off + 10 > len(buf):
            break
        rtype, rclass, ttl, rdlen = struct.unpack(">HHIH", buf[off:off + 10])
        off += 10
        rdata = buf[off:off + rdlen]
        value = None
        if rtype == 1 and rdlen == 4:
            value = socket.inet_ntoa(rdata)
        elif rtype == 28 and rdlen == 16:
            value = socket.inet_ntop(socket.AF_INET6, rdata)
        elif rtype in (5, 2):
            try:
                value, _ = parse_name(buf, off)
            except Exception:
                value = None
        answers.append({"name": rname, "type": QTYPE_NAME.get(rtype, str(rtype)),
                        "ttl": ttl, "value": value})
        off += rdlen

    return {
        "rcode": rcode,
        "rcode_name": RCODE.get(rcode, "UNKNOWN(%d)" % rcode),
        "truncated": bool(tc),
        "answer_count": an,
        "answers": answers,
    }


def query(resolver, name, qtype_str, timeout=3.0, port=53):
    qtype = QTYPE[qtype_str]
    txid = random.randint(0, 0xFFFF)
    pkt = build_query(name, qtype, txid)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    start = time.monotonic()
    try:
        sock.sendto(pkt, (resolver, port))
        data, _ = sock.recvfrom(4096)
        latency_ms = (time.monotonic() - start) * 1000.0
        result = parse_response(data, txid)
        result["latency_ms"] = round(latency_ms, 2)
        result["ok"] = True
        return result
    except socket.timeout:
        return {"ok": False, "error": "timeout", "rcode": None, "rcode_name": "TIMEOUT",
                "answer_count": 0, "answers": [], "latency_ms": None}
    except Exception as e:
        return {"ok": False, "error": str(e), "rcode": None, "rcode_name": "ERROR",
                "answer_count": 0, "answers": [], "latency_ms": None}
    finally:
        sock.close()


def fmt(r):
    if not r["ok"]:
        return "FAIL(%s)" % r.get("error", "?")
    return "%s an=%d %s %.1fms" % (
        r["rcode_name"], r["answer_count"],
        ",".join(a["value"] or "-" for a in r["answers"][:3]) or "-",
        r["latency_ms"] or 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--resolver", required=True)
    ap.add_argument("--name", default="www.baidu.com")
    ap.add_argument("--qtype", default="A", choices=list(QTYPE))
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=3.0)
    ap.add_argument("--selftest", action="store_true",
                    help="对同一 resolver 测 A/AAAA/NXDOMAIN，用于区分 RR 类型差异")
    args = ap.parse_args()

    if args.selftest:
        print("resolver=%s" % args.resolver)
        cases = [(args.name, "A"), (args.name, "AAAA"), (args.name, "MX")]
        for nm, qt in cases:
            r = query(args.resolver, nm, qt, args.timeout)
            print("  %-6s %-28s -> %s" % (qt, nm, fmt(r)))
        nx = "this-name-should-not-exist-9f3a2b.invalid"
        r = query(args.resolver, nx, "A", args.timeout)
        print("  %-6s %-28s -> %s" % ("A", nx, fmt(r)))
        return 0

    results = []
    for i in range(args.repeat):
        r = query(args.resolver, args.name, args.qtype, args.timeout)
        results.append(r)
        print("[%d/%d] %s %s %s -> %s" % (
            i + 1, args.repeat, args.resolver, args.name, args.qtype, fmt(r)))

    if args.repeat > 1:
        ok = sum(1 for r in results if r["ok"] and r["rcode"] == 0)
        lat = [r["latency_ms"] for r in results if r.get("latency_ms")]
        med = sorted(lat)[len(lat) // 2] if lat else 0.0
        print("summary: NOERROR %d/%d  median %.1fms" % (ok, args.repeat, med))
    return 0


if __name__ == "__main__":
    sys.exit(main())
