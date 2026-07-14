# SPDX-License-Identifier: GPL-2.0
out := arch/$(SRCARCH)/include/generated/asm
uapi := arch/$(SRCARCH)/include/generated/uapi/asm

# Create output directory if not already present
$(shell mkdir -p $(out) $(uapi))

syscall64 := $(src)/syscall_64.tbl

syshdr := $(srctree)/scripts/syscallhdr.sh
systbl := $(srctree)/arch/$(SRCARCH)/scripts/syscalltbl.sh
offset :=
prefix :=

quiet_cmd_syshdr = SYSHDR  $@
      cmd_syshdr = tmp=$@.tmp; \
		$(CONFIG_SHELL) $(syshdr) --abis $(abis) --emit-nr \
		$(if $(offset),--offset $(offset)) \
		$(if $(prefix),--prefix $(prefix)) \
		$< $$tmp; \
		if [ ! -r $@ ] || ! cmp -s $$tmp $@; then \
			mv -f $$tmp $@; \
		else \
			rm -f $$tmp; \
		fi
quiet_cmd_systbl = SYSTBL  $@
      cmd_systbl = tmp=$@.tmp; \
		$(CONFIG_SHELL) $(systbl) --abis $(abis) $< $$tmp; \
		if [ ! -r $@ ] || ! cmp -s $$tmp $@; then \
			mv -f $$tmp $@; \
		else \
			rm -f $$tmp; \
		fi

$(uapi)/unistd_64.h: abis := common
$(uapi)/unistd_64.h: $(syscall64) $(syshdr) FORCE
	$(call if_changed,syshdr)

ifeq ($(CONFIG_FLUX_FAST_NET),y)
fastnet_abis := common,fastnet
else
fastnet_abis := common
endif
$(out)/syscalls_64.h: abis := $(fastnet_abis)
$(out)/syscalls_64.h: $(syscall64) $(systbl) FORCE
	$(call if_changed,systbl)

uapisyshdr-y			+= unistd_64.h
syshdr-$(CONFIG_X86_64)	+= syscalls_64.h

uapisyshdr-y	:= $(addprefix $(uapi)/, $(uapisyshdr-y))
syshdr-y	:= $(addprefix $(out)/, $(syshdr-y))
targets		+= $(addprefix ../../../../, $(uapisyshdr-y) $(syshdr-y))

PHONY += all
all: $(uapisyshdr-y) $(syshdr-y)
	@:
