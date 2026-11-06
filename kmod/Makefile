# SPDX-License-Identifier: GPL-2.0
#
# WeakNet 实验性内核模块构建规则（B1 实验路径）
#
# 定位：不进 tools/ci.sh、不进 OTA manifest、不注册 IMonitorPlugin。
# 仅供板端手工 make / insmod 验证，或宿主机对拍编译。
#
# 用法：
#   板端就地编译（默认，开箱即用）
#     make
#
#   宿主机对拍编译（验证 API 在目标内核版本下是否成立，产物不可加载）
#     make KDIR=/path/to/linux-headers-5.15.x
#
#   交叉编译（需要完整 ARM64 内核构建树）
#     make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/build
#
#   清理
#     make clean

obj-m += weaknet_cpu_stat.o
obj-m += weaknet_softirq.o
obj-m += weaknet_lradc_keys.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ccflags-y += -Wall -Wextra -Wno-unused-parameter

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
