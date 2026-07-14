module := flux

KDIR ?= /lib/modules/$(shell uname -r)/build
DEBUG ?=
DEVICE_MODE ?= 666

ifneq ($(DEBUG),)
  KBUILD_CFLAGS += -DDEBUG
endif

obj-m := $(module).o

$(module)-y := main.o uintr.o hook.o mm.o mpk.o

all:
	make -C $(KDIR) M=$(CURDIR) PAHOLE=true modules

clean:
	make -C $(KDIR) M=$(CURDIR) clean

insmod: rmmod
	sudo insmod $(module).ko
	major=$$(awk '$$2 == "$(module)" {print $$1}' /proc/devices | tail -n1); \
	if [ -z "$$major" ]; then \
		echo "failed to resolve major number for $(module)"; \
		exit 1; \
	fi; \
	sudo mknod /dev/$(module) c $$major 0; \
	sudo chmod $(DEVICE_MODE) /dev/$(module); \
	sudo mknod /dev/$(module)_uintr c $$major 1; \
	sudo chmod $(DEVICE_MODE) /dev/$(module)_uintr; \
	sudo mknod /dev/$(module)_mm c $$major 2; \
	sudo chmod $(DEVICE_MODE) /dev/$(module)_mm

rmmod:
	sudo rm -f /dev/$(module)
	sudo rm -f /dev/$(module)_uintr
	sudo rm -f /dev/$(module)_mm
	@if [ -d /sys/module/$(module) ]; then sudo rmmod $(module); fi
