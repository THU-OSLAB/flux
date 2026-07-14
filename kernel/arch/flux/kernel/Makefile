extra-y := vmlinux.lds

KASAN_SANITIZE_init.o := n
KASAN_SANITIZE_stacktrace.o := n

obj-y = setup.o \
	misc.o \
	console.o \
	stacktrace.o \
	time.o \
	reboot.o \
	host_dev.o \
	host_fs.o \
	signal.o

obj-$(CONFIG_FLUX_RUNC) += execd.o

obj-$(CONFIG_SMP) += smp.o smpboot.o

obj-$(CONFIG_X86_64) += x86/

obj-$(CONFIG_FLUX_UINTR) += signal.o

# use default timer if uintr is not enabled
ifneq ($(CONFIG_FLUX_UINTR),y)
obj-y += timer-pcpu.o irq.o
endif
