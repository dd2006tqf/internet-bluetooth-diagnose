#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# hid-multitouch 能力自证：在没有物理触控板的情况下，证明
# 「驱动能真正绑定一个 MT 设备，并产出 ABS_MT_SLOT / ABS_MT_TRACKING_ID」。
#
# 为什么需要这个脚本：
#   insmod 成功 + /sys/bus/hid/drivers 下出现驱动目录，只证明**驱动注册**成功。
#   它不证明驱动能绑定设备，更不证明绑定后产出的是多点触控语义而不是单点鼠标。
#   没有物理触控板时，这两步通常被跳过，于是「能力已具备」是个未验证的断言。
#
# 做法：
#   用 /dev/uhid 从用户态造一个 HID 数字转换器（Digitizer/Touch Screen）设备，
#   内核 HID 核心会把它判为 HID_GROUP_MULTITOUCH，hid-multitouch 的通用
#   id_table 条目（HID_BUS_ANY / HID_GROUP_MULTITOUCH / HID_ANY_ID / HID_ANY_ID）
#   会自动绑定。随后检查产出的 input 节点是否带 MT 能力位，并真的写一帧触摸
#   报告，确认事件从 HID 层一路走到 input 层。
#
# 为什么这算证据而不算模拟真机：
#   它验证的是「驱动与内核 HID 栈对接正确」——HID 报告描述符解析、MT 字段映射、
#   input 槽位分配。这三件事与设备是 USB 还是 uhid 无关。
#   它不验证的：具体触控板的固件行为、VID/PID 是否需要 new_id、以及贴合精度。
#   那些只能靠实物。本脚本不因此宣称「触控板已验证可用」。
#
# 用法（板端，需 root）：
#   sudo python3 probe_virtual_mt.py
#   sudo python3 probe_virtual_mt.py --keep   # 保留设备不销毁，便于 evtest 手工观察
#
# 退出码：
#   0 驱动绑定 + MT 能力位齐全 + 收到 MT 事件（能力闭环）
#   2 设备已创建但未被 hid-multitouch 绑定（驱动匹配面有问题）
#   3 已绑定但 input 节点缺 MT 能力位（字段映射有问题）
#   4 能力位齐全但未收到 MT 事件（事件通路有问题）
#   5 环境不满足（/dev/uhid 不可用 / 无权限）

import os
import select
import struct
import sys
import time

UHID_DEV = "/dev/uhid"

# ---- uhid 协议常量（include/uapi/linux/uhid.h）---------------------------
#
# 这里的值必须逐字对齐 enum uhid_event_type。写错不会有任何语法错误，
# 只会在板端得到一个 "Operation not supported"：
#   UHID_CREATE2     = 11
#   UHID_INPUT2      = 12   ← 曾误写成 13，而 13 是 UHID_GET_REPORT；
#   UHID_GET_REPORT  = 13     用户态写它落到 uhid_char_write 的 default 分支
#                             → -EOPNOTSUPP。现场极易误判成"内核不支持"。
UHID_DESTROY = 1
UHID_CREATE2 = 11
UHID_INPUT2 = 12

HID_MAX_DESCRIPTOR_SIZE = 4096
UHID_EVENT_SIZE = 4 + (128 + 64 + 64 + 2 + 2 + 4 + 4 + 4 + 4 + HID_MAX_DESCRIPTOR_SIZE)

BUS_USB = 0x03

# ---- 报告描述符 ---------------------------------------------------------
# 一个最小可用的多点触控触摸屏描述符：
#   Digitizer / Touch Screen / Application
#     Finger / Logical
#       Tip Switch (1 bit) + 7 bit padding
#       Contact Identifier (8 bit)
#       X, Y (16 bit, 0..32767)
#
# 关键点是有 Contact Identifier（HID_DG_CONTACTID）：hid-multitouch 见到它才会
# 分配 maxcontacts 并调用 input_mt_init_slots()，也才会有 ABS_MT_SLOT 与
# ABS_MT_TRACKING_ID。只有 X/Y 而无 ContactID 的设备会被当成单点设备——
# 这正是「evtest 能打印坐标但实际不是多点触控」的那种情况。
REPORT_DESCRIPTOR = bytes([
    0x05, 0x0d,        # Usage Page (Digitizer)
    0x09, 0x04,        # Usage (Touch Screen)
    0xa1, 0x01,        # Collection (Application)
    0x85, 0x01,        #   Report ID (1)
    0x09, 0x22,        #   Usage (Finger)
    0xa1, 0x02,        #   Collection (Logical)
    0x09, 0x42,        #     Usage (Tip Switch)
    0x15, 0x00,        #     Logical Minimum (0)
    0x25, 0x01,        #     Logical Maximum (1)
    0x75, 0x01,        #     Report Size (1)
    0x95, 0x01,        #     Report Count (1)
    0x81, 0x02,        #     Input (Data,Var,Abs)
    0x95, 0x07,        #     Report Count (7)
    0x81, 0x03,        #     Input (Const,Var,Abs)
    0x09, 0x51,        #     Usage (Contact Identifier)
    0x75, 0x08,        #     Report Size (8)
    0x95, 0x01,        #     Report Count (1)
    0x81, 0x02,        #     Input (Data,Var,Abs)
    0x05, 0x01,        #     Usage Page (Generic Desktop)
    0x09, 0x30,        #     Usage (X)
    0x09, 0x31,        #     Usage (Y)
    0x16, 0x00, 0x00,  #     Logical Minimum (0)
    0x26, 0xff, 0x7f,  #     Logical Maximum (32767)
    0x75, 0x10,        #     Report Size (16)
    0x95, 0x02,        #     Report Count (2)
    0x81, 0x02,        #     Input (Data,Var,Abs)
    0xc0,              #   End Collection
    0xc0,              # End Collection
])

# ---- input 事件常量（include/uapi/linux/input-event-codes.h）-------------
EV_SYN = 0x00
EV_KEY = 0x01
EV_ABS = 0x03

BTN_TOUCH = 0x14a

ABS_MT_SLOT = 0x2f
ABS_MT_TOUCH_MAJOR = 0x30
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_TRACKING_ID = 0x39

ABS_NAMES = {
    ABS_MT_SLOT: "ABS_MT_SLOT",
    ABS_MT_TOUCH_MAJOR: "ABS_MT_TOUCH_MAJOR",
    ABS_MT_POSITION_X: "ABS_MT_POSITION_X",
    ABS_MT_POSITION_Y: "ABS_MT_POSITION_Y",
    ABS_MT_TRACKING_ID: "ABS_MT_TRACKING_ID",
}

# 判定「真多点触控」的必备能力位。缺任一即说明拿到的是单点语义。
REQUIRED_MT_ABS = (ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
                   ABS_MT_TRACKING_ID)

# struct input_event on aarch64: timeval(16) + type(2) + code(2) + value(4)
INPUT_EVENT = struct.Struct("qqHHi")


def log(msg=""):
    print(msg, flush=True)


def die_env(msg):
    log("环境不满足: " + msg)
    sys.exit(5)


def build_create2(name, phys, uniq, rd, bus, vendor, product, version):
    ev = bytearray(UHID_EVENT_SIZE)
    struct.pack_into("<I", ev, 0, UHID_CREATE2)
    for off, val, size in ((4, name, 128), (132, phys, 64), (196, uniq, 64)):
        raw = val.encode()
        if len(raw) > size - 1:  # 必须留结尾 NUL
            raise ValueError("uhid 字段过长: %r" % val)
        ev[off:off + len(raw)] = raw
    struct.pack_into("<H", ev, 260, len(rd))
    struct.pack_into("<H", ev, 262, bus)
    struct.pack_into("<I", ev, 264, vendor)
    struct.pack_into("<I", ev, 268, product)
    struct.pack_into("<I", ev, 272, version)
    struct.pack_into("<I", ev, 276, 0)  # country
    ev[280:280 + len(rd)] = rd
    return bytes(ev)


def build_input2(data):
    ev = bytearray(UHID_EVENT_SIZE)
    struct.pack_into("<I", ev, 0, UHID_INPUT2)
    struct.pack_into("<H", ev, 4, len(data))
    ev[6:6 + len(data)] = data
    return bytes(ev)


def build_destroy():
    ev = bytearray(UHID_EVENT_SIZE)
    struct.pack_into("<I", ev, 0, UHID_DESTROY)
    return bytes(ev)


def hid_devices():
    """当前总线上的 HID 设备目录名集合。"""
    try:
        return set(os.listdir("/sys/bus/hid/devices"))
    except OSError:
        return set()


def driver_of(hiddev):
    """返回 HID 设备当前绑定的驱动名，未绑定返回 None。"""
    link = "/sys/bus/hid/devices/%s/driver" % hiddev
    try:
        return os.path.basename(os.readlink(link))
    except OSError:
        return None


def event_node_of(hiddev):
    """在 HID 设备的 input 子树里找 event 节点，返回 (sysname, /dev 路径)。

    注意不能靠 /sys/bus/hid/devices/<dir>/name 定位设备：uhid 创建的设备
    在这个内核上没有该文件（读会 Errno 2）。正确的做法是比对创建前后
    的设备目录差集——目录名本身就是 0003:222A:0001.000N 这种形态。
    """
    base = "/sys/bus/hid/devices/%s/input" % hiddev
    try:
        inputs = os.listdir(base)
    except OSError:
        return None, None
    for inp in sorted(inputs):
        if not inp.startswith("input"):
            continue
        d = os.path.join(base, inp)
        try:
            entries = os.listdir(d)
        except OSError:
            continue
        for e in sorted(entries):
            if e.startswith("event"):
                return e, "/dev/input/" + e
    return None, None


def abs_capabilities(event_sysname):
    """读 /sys/class/input/eventN/device/capabilities/abs 的十六进制位图。"""
    path = "/sys/class/input/%s/device/capabilities/abs" % event_sysname
    try:
        with open(path) as f:
            text = f.read().strip()
    except OSError:
        return set()
    if not text:
        return set()
    mask = int(text, 16)
    return {code for code in range(0, 64) if mask & (1 << code)}


def try_bind_via_new_id(bus, vendor, product):
    """驱动没自动绑定时，用 sysfs new_id 强制匹配一次。

    注意 new_id 只是往驱动的动态匹配表里加一条 ID，只有当总线上真的存在
    该 VID/PID 的设备时才会触发绑定。它不会凭空空造一个设备。
    """
    path = "/sys/bus/hid/drivers/hid-multitouch/new_id"
    if not os.path.exists(path):
        return False
    line = "%04x %04x %04x" % (bus, vendor, product)
    try:
        with open(path, "w") as f:
            f.write(line)
        return True
    except OSError as exc:
        log("  new_id 写入失败: %s" % exc)
        return False


def read_events(fd, timeout=1.0):
    """从 input 节点读事件，返回 [(type, code, value)]。"""
    got = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        remain = deadline - time.time()
        if remain <= 0:
            break
        r, _, _ = select.select([fd], [], [], remain)
        if not r:
            continue
        try:
            buf = os.read(fd, INPUT_EVENT.size * 64)
        except OSError:
            break
        if not buf:
            break
        for off in range(0, len(buf) - INPUT_EVENT.size + 1, INPUT_EVENT.size):
            _, _, etype, code, value = INPUT_EVENT.unpack_from(buf, off)
            got.append((etype, code, value))
    return got


def describe_events(events):
    out = []
    for etype, code, value in events:
        if etype == EV_ABS:
            out.append("EV_ABS %s = %d" % (ABS_NAMES.get(code, hex(code)), value))
        elif etype == EV_KEY:
            name = "BTN_TOUCH" if code == BTN_TOUCH else hex(code)
            out.append("EV_KEY %s = %d" % (name, value))
        elif etype == EV_SYN:
            out.append("EV_SYN")
        else:
            out.append("type=%d code=0x%x value=%d" % (etype, code, value))
    return out


def main():
    keep = "--keep" in sys.argv

    if os.geteuid() != 0:
        die_env("需要 root 才能写 /dev/uhid")
    if not os.path.exists(UHID_DEV):
        die_env("%s 不存在（CONFIG_UHID 未加载？）" % UHID_DEV)

    name = "weaknet-virtual-touchscreen"
    vendor, product, version = 0x222A, 0x0001, 0x0100

    log("=== 前置检查 ===")
    log("  %s 可写: %s" % (UHID_DEV, os.access(UHID_DEV, os.W_OK)))
    log("  描述符大小: %d 字节" % len(REPORT_DESCRIPTOR))
    log()

    before = hid_devices()
    log("=== 创建虚拟 HID 设备 ===")
    log("  name=%s  bus=0x%02x  vid:pid=%04X:%04X"
        % (name, BUS_USB, vendor, product))

    uhid = os.open(UHID_DEV, os.O_RDWR)
    event_path = None  # 提前初始化：finally 里要用，不能依赖 try 内部是否走到赋值
    try:
        os.write(uhid, build_create2(name, "uhid/probe", "weaknet",
                                     REPORT_DESCRIPTOR, BUS_USB,
                                     vendor, product, version))
        # 给内核 HID 核心解析描述符 + 驱动 probe 的时间
        time.sleep(0.6)

        new_devs = sorted(hid_devices() - before)
        if not new_devs:
            log("  失败: /sys/bus/hid/devices 下没有出现新设备")
            return 2
        hiddev = new_devs[0]
        log("  HID 设备目录: %s" % hiddev)

        drv = driver_of(hiddev)
        log("  绑定驱动: %s" % (drv or "<未绑定>"))

        if drv != "hid-multitouch":
            log("  驱动未自动绑定，尝试 new_id 强制匹配 ...")
            try_bind_via_new_id(BUS_USB, vendor, product)
            time.sleep(0.3)
            drv = driver_of(hiddev)
            log("  重试后驱动: %s" % (drv or "<仍未绑定>"))
        if drv != "hid-multitouch":
            log("  失败: 设备存在但未由 hid-multitouch 接管")
            log("        （若被 hid-generic 抢占，说明该设备的 HID group 不是 MULTITOUCH）")
            return 2
        log("  ✓ 已由 hid-multitouch 绑定")
        log()

        event_sysname, event_path = event_node_of(hiddev)
        if not event_sysname:
            log("  失败: 该 HID 设备下没有 input event 节点")
            return 3
        log("=== input 节点 ===")
        log("  %s" % event_path)

        caps = abs_capabilities(event_sysname)
        log("  ABS 能力位: %s"
            % " ".join(ABS_NAMES[c] for c in sorted(caps) if c in ABS_NAMES))
        missing = [c for c in REQUIRED_MT_ABS if c not in caps]
        for code in REQUIRED_MT_ABS:
            log("    %-22s %s" % (ABS_NAMES[code],
                                 "YES" if code in caps else "NO"))
        if missing:
            log("  失败: 缺 MT 能力位 %s"
                % ", ".join(ABS_NAMES[c] for c in missing))
            log("        （拿到的是单点语义，不是多点触控）")
            return 3
        log("  ✓ MT 能力位齐全")
        log()

        # 必须在写报告**之前**打开 event 节点，否则事件已经过去，读到 0 条。
        efd = os.open(event_path, os.O_RDONLY | os.O_NONBLOCK)
        # ReportID=1 | tip_switch=1 + 7bit padding | contact_id=3 | X=1000 | Y=2000
        report = bytes([0x01, 0x01, 0x03]) + struct.pack("<HH", 1000, 2000)
        log("=== 写入一帧触摸报告 ===")
        log("  %s" % report.hex())
        os.write(uhid, build_input2(report))
        events = read_events(efd, timeout=1.0)
        os.close(efd)

        log("  收到 %d 个 input 事件:" % len(events))
        for line in describe_events(events):
            log("    " + line)

        mt_codes = {code for etype, code, _ in events if etype == EV_ABS}
        if ABS_MT_TRACKING_ID not in mt_codes:
            log("  失败: 未收到 ABS_MT_TRACKING_ID（事件通路有问题）")
            return 4
        log("  ✓ 事件从 HID 层贯穿到 input 层")
        log()
        log("=== 结论 ===")
        log("  能力闭环：驱动注册 → 设备绑定 → MT 字段映射 → 事件产出")
        log("  未验证（需实物）：具体触控板固件行为、VID/PID 是否需 new_id、贴合精度")
        return 0
    except OSError as exc:
        log("  失败: %s" % exc)
        return 5
    finally:
        if keep:
            log()
            log("(--keep) 设备保留以便手工观察：")
            log("  sudo evtest %s" % (event_path or "/dev/input/eventN"))
            log("  注意：脚本退出后 uhid fd 已关闭，该设备随即被内核销毁，")
            log("        --keep 只在本进程存活期间有意义，主要用于加长观察窗口。")
        else:
            try:
                os.write(uhid, build_destroy())
            except OSError:
                pass
        os.close(uhid)


if __name__ == "__main__":
    sys.exit(main())
