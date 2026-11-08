# SPDX-License-Identifier: GPL-2.0
#
# hid-multitouch 外部模块构建规则
#
# 定位：**随发布包分发，但不自动加载**。插上多点触控设备时由
# /etc/udev/rules.d/99-weaknet-hid-multitouch.rules 按需 insmod。
#
# 为什么单独一个目录而不是并入 kmod/Makefile：
#   kmod/Makefile 里的两个模块会被 tools/kmod_cross_build.sh 编进发布包
#   并由 weaknet-kmod.service **开机无条件加载**——它们是无害的观测类模块
#   （只读内核数据）。而 hid-multitouch 会改变 HID 绑定行为：一旦在内核里，
#   HID 核心会把所有 HID_GROUP_MULTITOUCH 设备优先绑给它而非内建的
#   hid-generic。对触摸屏是改善，但这是全局输入行为变更，不该无条件生效。
#
#   两者因此是**两种加载模型**：无条件（systemd）vs 按需（udev）。
#   用两个 Makefile 分开，是让这个区别在文件系统结构上就看得见。
#
# 用法：
#   宿主机交叉编译（推荐，随发布包走）
#     ./tools/kmod_cross_build.sh          # 三个模块一起
#
#   板端编译（调试验证用）
#     make
#
#   加载 / 卸载
#     sudo insmod hid-multitouch.ko
#     sudo rmmod hid_multitouch
#
#   清理
#     make clean
#
# 前置条件（已实测成立，见 README.md）：
#   - 板端有 /lib/modules/$(uname -r)/build（板端编译时）
#   - # CONFIG_MODVERSIONS is not set（无符号 CRC 校验）
#   - # CONFIG_MODULE_SIG is not set（无签名强制）
#   - __hid_register_driver / input_mt_* 等符号已导出

obj-m += hid-multitouch.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
