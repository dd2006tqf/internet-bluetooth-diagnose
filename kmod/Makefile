# SPDX-License-Identifier: GPL-2.0
#
# WeakNet 观测类内核模块构建规则
#
# 定位：这两个模块随发布包分发，并由 weaknet-kmod.service **开机无条件加载**。
# 它们是只读观测模块，不改任何系统行为，因此可以无条件生效。
#
# 注意：hid_multitouch/ 是**另一个独立构建单元**（有自己的 Makefile），
# 不能被本 Makefile 的 obj-m 收进来——那个模块会改变 HID 绑定行为，
# 走的是 udev 按需加载而非开机加载。两种加载模型必须分开。
#
# 用法：
#   宿主机交叉编译（推荐，随发布包走）
#     ./tools/kmod_cross_build.sh      # 三个模块一起编
#
#   板端就地编译（调试验证用）
#     make
#
#   宿主机对拍编译（验证 API 在目标内核版本下是否成立）
#     make KDIR=/path/to/linux-headers-5.15.x
#
#   清理
#     make clean

obj-m += weaknet_cpu_stat.o
obj-m += weaknet_softirq.o

# weaknet_lradc_keys.o 已于 2026-11-08 移除（LRADC 在 A733 上不可用，
# 且无业务消费者）。历史实现见 git show 289cad4:kmod/weaknet_lradc_keys.c，
# 根因定位与诊断方法学保留在 README.md 第八节。

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ccflags-y += -Wall -Wextra -Wno-unused-parameter

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
