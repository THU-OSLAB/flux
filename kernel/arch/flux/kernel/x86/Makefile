obj-y += entry.o process.o match.o setup.o syscalls.o signal.o extable.o

obj-y += crypto/

obj-y += lib/

obj-$(CONFIG_SMP) += percpu.o

obj-y += uintr.o timer-uintr.o
obj-$(CONFIG_FLUX_MPK) += uaccess.o

obj-$(CONFIG_X86_TSC) += tsc.o

# Reuse Linux 6.6's names, using the Flux copy of the feature definitions.
obj-$(CONFIG_PROC_FS) += capflags.o
CFLAGS_capflags.o += -include $(srctree)/arch/flux/include/asm/x86/cpufeatures.h

quiet_cmd_flux_mkcapflags = MKCAP   $@
      cmd_flux_mkcapflags = $(CONFIG_SHELL) $(srctree)/arch/x86/kernel/cpu/mkcapflags.sh $@ $^

$(obj)/capflags.c: $(srctree)/arch/flux/include/asm/x86/cpufeatures.h \
		 $(srctree)/arch/x86/include/asm/vmxfeatures.h \
		 $(srctree)/arch/x86/kernel/cpu/mkcapflags.sh FORCE
	$(call if_changed,flux_mkcapflags)
targets += capflags.c

# Use native decoder headers only for the objects that need them. Kbuild keeps
# Flux architecture headers first in the search path.
flux-x86-includes := -I$(srctree)/arch/x86/include
CFLAGS_xol.o += $(flux-x86-includes)
CFLAGS_xol-insn.o += $(flux-x86-includes)
CFLAGS_xol-inat.o += $(flux-x86-includes)
CFLAGS_ioport.o += $(flux-x86-includes)

# Native Linux 6.6 instruction analysis shared by the MPK fault adapters.
obj-$(CONFIG_FLUX_MPK) += xol.o xol-insn.o xol-inat.o

quiet_cmd_flux_xol_core = GEN     $@
      cmd_flux_xol_core = $(PYTHON3) $(srctree)/arch/flux/kernel/x86/gen-xol-core.py $(srctree)/arch/x86/kernel/uprobes.c $@

$(obj)/xol-core.inc: $(srctree)/arch/x86/kernel/uprobes.c $(srctree)/arch/flux/kernel/x86/gen-xol-core.py FORCE
	$(call if_changed,flux_xol_core)
$(obj)/xol.o: $(obj)/xol-core.inc

targets += xol-core.inc inat-tables.c xol-insn.o xol-inat.o
quiet_cmd_flux_inat = GEN     $@
      cmd_flux_inat = $(AWK) -f $(srctree)/arch/x86/tools/gen-insn-attr-x86.awk $(srctree)/arch/x86/lib/x86-opcode-map.txt > $@
$(obj)/inat-tables.c: $(srctree)/arch/x86/lib/x86-opcode-map.txt $(srctree)/arch/x86/tools/gen-insn-attr-x86.awk FORCE
	$(call if_changed,flux_inat)
$(obj)/xol-insn.o: $(srctree)/arch/x86/lib/insn.c FORCE
	$(call if_changed_rule,cc_o_c)
$(obj)/xol-inat.o: $(srctree)/arch/x86/lib/inat.c $(obj)/inat-tables.c FORCE
	$(call if_changed_rule,cc_o_c)
CFLAGS_xol-inat.o += -I$(obj)

# Existing Linux 6.6 I/O permission state, with Flux fault-based execution.
obj-$(CONFIG_FLUX_MPK) += ioport.o
quiet_cmd_flux_ioport_core = GEN     $@
      cmd_flux_ioport_core = $(PYTHON3) $(srctree)/arch/flux/kernel/x86/gen-ioport-core.py $(srctree)/arch/x86/kernel/ioport.c $@
$(obj)/ioport-core.inc: $(srctree)/arch/x86/kernel/ioport.c $(srctree)/arch/flux/kernel/x86/gen-ioport-core.py FORCE
	$(call if_changed,flux_ioport_core)
$(obj)/ioport.o: $(obj)/ioport-core.inc
targets += ioport-core.inc
