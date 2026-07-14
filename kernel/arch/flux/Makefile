# SPDX-License-Identifier: GPL-2.0

KBUILD_CFLAGS += -fno-builtin

exec_format = $(firstword $(subst -, ,$(CONFIG_OUTPUT_FORMAT)))

ifneq (,$(filter elf64 elf32,$(exec_format)))
KBUILD_CFLAGS += -fPIC
else ifneq (,$(filter pe,$(exec_format)))
ifneq ($(CONFIG_OUTPUT_FORMAT),pe-x86-64)
prefix=_
KBUILD_CFLAGS += -fno-leading-underscore
endif
# Keep the arch-specific system headers reachable when building with O=.
LINUXINCLUDE := -isystem $(srctree)/arch/flux/include/system $(LINUXINCLUDE)
# workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=52991
KBUILD_CFLAGS += -mno-ms-bitfields -D__DISABLE_EXPORTS
else ifneq (,$(CONFIG_OUTPUT_FORMAT))
$(error Unrecognized platform: $(CONFIG_OUTPUT_FORMAT))
endif

ifeq ($(shell uname -s), Linux)
NPROC=$(shell nproc)
else ifneq ($(shell uname -s | grep MSYS),)
NPROC=${NUMBER_OF_PROCESSORS}
else # e.g., FreeBSD
NPROC=$(shell sysctl -n hw.ncpu)
endif

FLUX_INSTALL_JOB_FLAG = $(lastword $(filter -j% --jobs=%,$(MAKEFLAGS)))
FLUX_INSTALL_JOBS = $(strip $(if $(FLUX_INSTALL_JOB_FLAG),\
	$(if $(filter -j --jobs,$(FLUX_INSTALL_JOB_FLAG)),$(NPROC),\
	$(patsubst --jobs=%,%,$(patsubst -j%,%,$(FLUX_INSTALL_JOB_FLAG)))),\
	1))

#
# Prevent GCC from generating any FP code by mistake.
#
# This must happen before we try the -mpreferred-stack-boundary, see:
#
#    https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53383
#
KBUILD_CFLAGS += -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx

LDFLAGS_vmlinux += -r
FLUX_ENTRY_POINTS := $(shell cat $(srctree)/arch/flux/export.syms)

core-y += arch/flux/kernel/
core-y += arch/flux/mm/
core-y += arch/flux/drivers/

all: flux.o arch/flux/include/generated/uapi/asm/syscall_defs.h

flux.o: vmlinux
	$(OBJCOPY) $(if $(KEEP_EH_FRAMES),,-R .eh_frame) \
		-R .syscall_defs $(foreach sym,$(FLUX_ENTRY_POINTS),-G$(prefix)$(sym)) --prefix-symbols=$(prefix) \
		vmlinux flux.o

arch/flux/include/generated/uapi/asm/syscall_defs.h: vmlinux
	$(OBJCOPY) -j .syscall_defs -O binary --set-section-flags .syscall_defs=alloc $< $@
	$(Q) export tmpfile=$(shell mktemp); \
	sed 's/\x0//g' $@ > $$tmpfile; mv $$tmpfile $@ ; rm -f $$tmpfile

# Syscall table generation
archheaders:
	$(Q)$(MAKE) $(build)=arch/flux/kernel/x86/syscalls all

install: scripts_unifdef
	@mkdir -p $(INSTALL_PATH)/lib/
	@if [ ! -r $(INSTALL_PATH)/lib/flux.o ] || ! cmp -s flux.o $(INSTALL_PATH)/lib/flux.o; then \
		echo "  INSTALL	$(INSTALL_PATH)/lib/flux.o"; \
		cp flux.o $(INSTALL_PATH)/lib/flux.o; \
	fi
	@$(srctree)/arch/flux/scripts/headers_install.py \
		-j$(FLUX_INSTALL_JOBS) \
		$(INSTALL_PATH)/include

define archhelp
  echo '  install	- Install library and headers to INSTALL_PATH/{lib,include}'
endef
