# SPDX-License-Identifier: GPL-2.0-or-later

obj-m      += smcbatt.o
KVER       ?= `uname -r`
KBASE      ?= /lib/modules/$(KVER)
KBUILD_DIR ?= $(KBASE)/build

# CachyOS/Arch kernels are LLVM-built; kbuild defaults to gcc.
CLANG_KERNEL := $(shell grep -qs '^CONFIG_CC_IS_CLANG=y' \
		$(KBUILD_DIR)/include/config/auto.conf && echo 1)
ifeq ($(CLANG_KERNEL),1)
TOOLCHAIN ?= LLVM=1
endif

all:
	$(MAKE) -C $(KBUILD_DIR) M=$(CURDIR) $(TOOLCHAIN)

clean:
	$(MAKE) -C $(KBUILD_DIR) M=$(CURDIR) clean
