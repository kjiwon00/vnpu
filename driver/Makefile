# SPDX-License-Identifier: GPL-2.0-only
#
# 트리 밖(out-of-tree) 빌드. 실제 빌드 규칙은 Kbuild에 있다.
#   make            모듈 빌드 (vnpu.ko)
#   make clean
# 게스트에서: insmod vnpu.ko

KDIR ?= $(HOME)/workspace/linux

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
