# SPDX-License-Identifier: GPL-2.0
include $(srctree)/lib/vdso/Makefile

obj-y += image.o
targets += vclock_gettime.o gate.o vdso.so
GCOV_PROFILE := n
KCOV_INSTRUMENT := n
KASAN_SANITIZE := n
UBSAN_SANITIZE := n
ccflags-y += -fPIC -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone
ccflags-y += -DDISABLE_BRANCH_PROFILING
CFLAGS_REMOVE_vclock_gettime.o = $(CC_FLAGS_FTRACE)

# Linux x86 exports the ABI and uses lib/vdso/gettimeofday.c. Flux supplies
# only asm/vdso/gettimeofday.h and the syscall gate.
$(obj)/vclock_gettime.o: $(srctree)/arch/x86/entry/vdso/vclock_gettime.c FORCE
	$(call if_changed_rule,cc_o_c)

$(obj)/image.o: $(obj)/vdso.so

quiet_cmd_flux_vdso = VDSOLD  $@
      cmd_flux_vdso = $(LD) -shared --hash-style=both -Bsymbolic --no-undefined \
	-z noexecstack -z max-page-size=4096 -soname linux-vdso.so.1 \
	-T $(srctree)/$(src)/vdso.lds --version-script=$(srctree)/$(src)/vdso.map \
	-o $@ $(obj)/vclock_gettime.o $(obj)/gate.o; $(OBJCOPY) -S $@; $(cmd_vdso_check)

$(obj)/vdso.so: $(obj)/vclock_gettime.o $(obj)/gate.o \
		$(srctree)/$(src)/vdso.lds $(srctree)/$(src)/vdso.map FORCE
	$(call if_changed,flux_vdso)
