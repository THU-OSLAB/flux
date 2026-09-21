extra-y := vmlinux.lds

KASAN_SANITIZE_init.o := n
KASAN_SANITIZE_stacktrace.o := n

obj-y = setup.o \
	exec.o \
	misc.o \
	console.o \
	stacktrace.o \
	time.o \
	reboot.o \
	host_dev.o \
	host_fs.o \
	proc_maps.o \
	signal.o \
	vdso.o \
	vdso/

obj-$(CONFIG_FLUX_RUNC) += execd.o

obj-$(CONFIG_MODULES) += module.o

obj-$(CONFIG_SMP) += smpboot.o

obj-$(CONFIG_X86_64) += x86/
