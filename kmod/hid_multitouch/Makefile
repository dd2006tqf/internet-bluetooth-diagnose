# SPDX-License-Identifier: GPL-2.0
#
# hid-multitouch 外部模块构建规则
#
# 定位：**不进 tools/ci.sh、不进 OTA manifest、不随发布包分发**。
# 仅供需要多点触控时在板端手工 make + insmod。
#
# 为什么单独一个目录而不是并入 kmod/Makefile：
#   kmod/Makefile 里的三个模块会被 tools/ci.sh 整个交叉编译进发布包
#   （releases/<ver>/kmod/），它们是无害的观测类模块。而 hid-multitouch
#   会与内核内建的 hid-generic 争抢同一批 USB HID 设备，属于**改变系统
#   输入行为**的模块——必须由人显式加载，绝不能默认随包落地。
#
# 用法：
#   板端编译（唯一受支持的方式：必须用板端内核头文件，否则 vermagic 失配）
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
#   - 板端有 /lib/modules/$(uname -r)/build
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
