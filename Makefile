# Do not use make's built-in rules
# (this improves performance and avoids hard-to-debug behaviour);
# also do not print "Entering directory..." messages from make
.SUFFIXES:
MAKEFLAGS += -r --no-print-directory

KCONFIG ?= defconfig
KERNEL_DIR := $(CURDIR)/kernel

ifneq ($(silent),1)
  ifneq ($(V),1)
	QUIET_CONF           = @echo '  CONF    '$@;
	Q = @
  endif
endif

PREFIX := /usr
BINDIR := $(PREFIX)/bin
INCDIR := $(PREFIX)/include
LIBDIR := $(PREFIX)/lib

ifeq (,$(srctree))
  srctree := $(KERNEL_DIR)
endif
export srctree

include $(srctree)/tools/scripts/Makefile.include
include scripts/flux-paths.mk
NO_CONFIG_GOALS := clean mrproper clean-conf compile-commands

# Reuse an existing kernel .config as the base so top-level builds pick up
# menuconfig changes instead of regenerating from defconfig every time.
# Skip cleanup and compilation-database-only invocations; neither should
# regenerate the current build configuration.
ifeq ($(strip $(MAKECMDGOALS)),)
-include $(KERNEL_OUT)/.config
else ifneq ($(filter-out $(NO_CONFIG_GOALS),$(MAKECMDGOALS)),)
-include $(KERNEL_OUT)/.config
endif
include scripts/flux-config.mk

# Keep kernel .config as the merge base, but make each top-level build derive
# its exported FLUX_* knobs from the current invocation instead of reusing the
# previously merged values. Preserve the imported values only for menuconfig.
ifeq ($(filter menuconfig,$(MAKECMDGOALS)),)
$(eval $(call flux_reset_exported_config))
endif

OUTPUT := $(OBJ_DIR)/
export OUTPUT
KERNEL_MAKE := $(MAKE) -C $(srctree) O=$(KERNEL_OUT) ARCH=flux $(KOPT)
BUILD_PATHS := $(sort $(BUILD_DIR) $(OUTPUT) $(EXEC_DIR) $(KERNEL_OUT))
CONFIG_STAMP := $(CONFIG_DIR)/.config.stamp
KERNEL_FLUX_O := $(KERNEL_OUT)/flux.o
KERNEL_SYSCALL_DEFS := $(KERNEL_OUT)/arch/flux/include/generated/uapi/asm/syscall_defs.h
INSTALL_STAMP := $(OUTPUT)include/kernel/.install.stamp

$(eval $(call flux_apply_build_config))
export $(addprefix CONFIG_FLUX_,$(CONFIG_EXPORTS))
CONFIG_SIGNATURE := $(strip $(foreach name,$(CONFIG_EXPORTS),$(name)=$(strip $(CONFIG_FLUX_$(name)));))


THIRD_PARTY_LIBS := $(if $(wildcard $(CURDIR)/third-party),$(CURDIR)/third-party,$(OUTPUT)third-party)
FLUX_DIR := flux
FLUX_OBJ_DIR := $(OUTPUT)$(FLUX_DIR)/

ifeq ($(CONFIG_FLUX_FNET),y)
PKG_CONFIG_PATH := $(THIRD_PARTY_LIBS)/rdma-core/build/lib/pkgconfig:$(PKG_CONFIG_PATH)
PKG_CONFIG_PATH := $(THIRD_PARTY_LIBS)/dpdk/build/lib/x86_64-linux-gnu/pkgconfig:$(PKG_CONFIG_PATH)
endif
ifeq ($(CONFIG_FLUX_SPDK),y)
PKG_CONFIG_PATH := $(THIRD_PARTY_LIBS)/spdk/build/lib/pkgconfig:$(PKG_CONFIG_PATH)
PKG_CONFIG_PATH := $(THIRD_PARTY_LIBS)/dpdk/build/lib/x86_64-linux-gnu/pkgconfig:$(PKG_CONFIG_PATH)
PKG_CONFIG_PATH := $(THIRD_PARTY_LIBS)/rdma-core/build/lib/pkgconfig:$(PKG_CONFIG_PATH)
endif

# mlx5 libs
MLX5_LIBS = -L$(THIRD_PARTY_LIBS)/rdma-core/build/lib/
MLX5_LIBS += -L$(THIRD_PARTY_LIBS)/rdma-core/build/lib/statics/
MLX5_LIBS += -L$(THIRD_PARTY_LIBS)/rdma-core/build/util/
MLX5_LIBS += -L$(THIRD_PARTY_LIBS)/rdma-core/build/ccan/
MLX5_LIBS += -l:libmlx5.a -l:libibverbs.a -lnl-3 -lnl-route-3 -lrdma_util -lccan

FLUX_RUNTIME_RPATH-y :=
FLUX_RUNTIME_RPATH_STR-$(CONFIG_FLUX_SPDK) := '$$ORIGIN/../third-party/dpdk/build/lib/x86_64-linux-gnu:$$ORIGIN/../third-party/spdk/build/lib:$$ORIGIN/../third-party/rdma-core/build/lib'
FLUX_RUNTIME_RPATH-$(CONFIG_FLUX_SPDK) += -Wl,-rpath,$(FLUX_RUNTIME_RPATH_STR-y)

FNET_LDLIBS-$(CONFIG_FLUX_FNET) += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs --static libdpdk)
FNET_LDLIBS-$(CONFIG_FLUX_FNET) += $(MLX5_LIBS)
SPDK_CFLAGS-$(CONFIG_FLUX_SPDK) += -I$(THIRD_PARTY_LIBS)/spdk/build/include
SPDK_CFLAGS-$(CONFIG_FLUX_SPDK) += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags spdk_nvme spdk_env_dpdk)
SPDK_LDLIBS-$(CONFIG_FLUX_SPDK) += -L$(THIRD_PARTY_LIBS)/spdk/build/lib -lspdk
# Keep the decoder and its dependencies in the runtime image: each separate
# DSO adds VMAs that native dup_mm must reproduce for every LibOS process.
REWRITE_LDLIBS := -Wl,-Bstatic -lopcodes -lbfd -liberty -lz -lzstd -lsframe -Wl,-Bdynamic

INC := -Iinclude \
	-I$(CURDIR)/$(FLUX_DIR) \
	-I$(CURDIR) \
	-I$(OUTPUT)include \
	-include $(AUTOCONF_H)
INC-$(CONFIG_FLUX_FNET) += -I$(THIRD_PARTY_LIBS)/dpdk/build/include
INC-$(CONFIG_FLUX_FNET) += -I$(THIRD_PARTY_LIBS)/rdma-core/build/include
INC-$(CONFIG_FLUX_SPDK) += $(SPDK_CFLAGS-y)


export CFLAGS += $(INC) $(INC-y)\
	-Wall -g -O3 -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	-fno-strict-aliasing -fno-omit-frame-pointer \
	-mssse3 # for fnet/iokd dataplane
export CFLAGS += -muintr

DEBUG ?=
ifeq ($(DEBUG),1)
export CFLAGS += -DDEBUG
endif


# libs-y += lib/libflux
progs-y += flux
progs-$(CONFIG_FLUX_RUNC) += flux-runc
progs-y += flux-iokd
flux-runtime-deps-y += $(EXEC_DIR)/flux-iokd$(EXESUF)

# Expand targets to output location and suffix but preserve special
# targets (e.g. .WAIT)
# $1 - targets
# $2 - suffix
expand-targets = $(foreach t,$(1),$(if $(filter .%,$(t)),$(t),$(OUTPUT)$(t)$(2)))
expand-prog-targets = $(foreach t,$(1),$(EXEC_DIR)/$(t)$(EXESUF))

# fixdep watches these split-config markers rather than generated autoconf.h.
# Recreate them whenever the exported Flux configuration changes.
define flux_sync_config
tmp_autoconf=$$(mktemp); \
tmp_config=$$(mktemp); \
	changed=0; \
	{ \
	$(foreach name,$(CONFIG_EXPORTS),$(if $(strip $(CONFIG_FLUX_$(name))),$(if $(call flux_autoconf_line,$(name)),printf '%s\n' '$(call flux_autoconf_line,$(name))';))) \
	} > $$tmp_autoconf; \
	{ \
	$(foreach name,$(CONFIG_EXPORTS),$(if $(strip $(CONFIG_FLUX_$(name))),$(if $(call flux_kconfig_line,$(name)),printf '%s\n' '$(call flux_kconfig_line,$(name))';))) \
	} > $$tmp_config; \
mkdir -p $(dir $(AUTOCONF_H)); \
if ! cmp -s $$tmp_autoconf $(AUTOCONF_H) 2>/dev/null; then mv $$tmp_autoconf $(AUTOCONF_H); changed=1; else rm -f $$tmp_autoconf; fi; \
if ! cmp -s $$tmp_config $(FLUX_CONFIG) 2>/dev/null; then mv $$tmp_config $(FLUX_CONFIG); changed=1; else rm -f $$tmp_config; fi; \
if [ "$$changed" -eq 1 ] || [ ! -f $(CONFIG_STAMP) ] || [ "$$(cat $(CONFIG_STAMP) 2>/dev/null || true)" != '$(CONFIG_SIGNATURE)' ]; then \
	$(RM) $(KERNEL_OUT)/include/config/FLUX_*; \
	printf '%s\n' '$(CONFIG_SIGNATURE)' > $(CONFIG_STAMP); \
fi
endef

TARGETS := $(call expand-prog-targets,$(progs-y))
TARGETS += $(call expand-targets,$(libs-y),$(SOSUF))
all: $(TARGETS)

# Collect saved commands after all recursive builds have finished.
all compile-commands:
	$(Q)python3 scripts/gen_compile_commands.py --build-dir "$(BUILD_DIR)"


ASM_UAPI_GENERATED:=$(KERNEL_OUT)/arch/flux/include/generated/uapi/asm
ASM_CONFIG:=$(ASM_UAPI_GENERATED)/config.h
DOT_CONFIG:=$(KERNEL_OUT)/.config

$(BUILD_PATHS):
	$(Q)mkdir -p $@

$(ASM_CONFIG):
	$(Q)mkdir -p $$(dirname $@)
	$(Q)echo "" > $@

$(CONFIG_STAMP): FORCE scripts/flux-config.mk | $(OUTPUT)
	$(Q)$(call flux_sync_config)

# flux_sync_config updates both the stamp and the exported outer config files.
$(FLUX_CONFIG) $(AUTOCONF_H): $(CONFIG_STAMP)

$(DOT_CONFIG): $(CONFIG_STAMP) $(FLUX_CONFIG) | $(KERNEL_OUT)
	$(call QUIET_CONF, $@)if [ ! -f $@ ]; then $(KERNEL_MAKE) $(KCONFIG); fi
	+$(Q)$(srctree)/scripts/kconfig/merge_config.sh -m -O $(KERNEL_OUT) $(DOT_CONFIG) $(FLUX_CONFIG)
	+$(Q)$(KERNEL_MAKE) olddefconfig
	+$(Q)$(KERNEL_MAKE) syncconfig

conf: $(CONFIG_STAMP)

conf-kernel: $(CONFIG_STAMP) $(DOT_CONFIG) $(ASM_CONFIG)

menuconfig: $(CONFIG_STAMP) $(DOT_CONFIG) | $(KERNEL_OUT)
	+$(Q)$(KERNEL_MAKE) menuconfig

.PHONY: kernel-check

# Always let the kernel build system check whether sources changed; it will
# no-op on its own when nothing in the kernel tree or .config changed.
kernel-check: $(DOT_CONFIG) $(ASM_CONFIG) | $(KERNEL_OUT)
	+$(Q)$(KERNEL_MAKE)

# Validate that the expected kernel outputs exist after kernel-check.
$(KERNEL_FLUX_O): $(DOT_CONFIG) $(ASM_CONFIG) | kernel-check
	@test -f $@

$(KERNEL_SYSCALL_DEFS): $(DOT_CONFIG) $(ASM_CONFIG) | kernel-check
	@test -f $@

# Install the exported kernel object and headers into the outer build tree.
$(OUTPUT)lib/flux.o $(INSTALL_STAMP) &: $(KERNEL_FLUX_O) $(KERNEL_SYSCALL_DEFS) | $(OUTPUT)
	$(Q)$(RM) -r $(OUTPUT)include/kernel
	+$(Q)$(KERNEL_MAKE) install INSTALL_PATH=$(OUTPUT)
	@touch $(INSTALL_STAMP)

# libflux is special
$(OUTPUT)libflux$(SOSUF): $(FLUX_OBJ_DIR)libflux-in.o $(OUTPUT)lib/flux.o
$(OUTPUT)libflux.a: $(FLUX_OBJ_DIR)libflux-in.o $(OUTPUT)lib/flux.o
	$(QUIET_AR)$(AR) -rc $@ $^

# rule to link flux
$(EXEC_DIR)/flux$(EXESUF): $(FLUX_OBJ_DIR)flux-in.o $(OUTPUT)libflux.a | $(EXEC_DIR) $(flux-runtime-deps-y)
	$(QUIET_LINK)$(CC) $(LDFLAGS) $(FLUX_RUNTIME_RPATH-y) -o $@ $^ $(LDLIBS) $(LDLIBS-y) $(SPDK_LDLIBS-y) $(REWRITE_LDLIBS)
	$(Q)if [ -n "$(FLUX_RUNTIME_RPATH_STR-y)" ] && command -v patchelf >/dev/null 2>&1; then patchelf --force-rpath --set-rpath $(FLUX_RUNTIME_RPATH_STR-y) $@; fi

$(EXEC_DIR)/flux-runc$(EXESUF): $(FLUX_OBJ_DIR)flux-runc-in.o $(OUTPUT)libflux.a | $(EXEC_DIR) $(flux-runtime-deps-y)
	$(QUIET_LINK)$(CC) $(LDFLAGS) $(FLUX_RUNTIME_RPATH-y) -o $@ $^ $(LDLIBS) $(LDLIBS-y) $(SPDK_LDLIBS-y) $(REWRITE_LDLIBS)
	$(Q)if [ -n "$(FLUX_RUNTIME_RPATH_STR-y)" ] && command -v patchelf >/dev/null 2>&1; then patchelf --force-rpath --set-rpath $(FLUX_RUNTIME_RPATH_STR-y) $@; fi

$(EXEC_DIR)/flux-iokd$(EXESUF): $(FLUX_OBJ_DIR)iokd-in.o | $(EXEC_DIR)
	$(QUIET_LINK)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(FNET_LDLIBS-y)

# Always let the userspace sub-build check whether sources changed; it will
# no-op on its own when nothing under the target tree or config changed.
# iokd is a standalone userspace daemon and does not need the exported kernel
# object or installed headers to build.
$(FLUX_OBJ_DIR)iokd-in.o: FORCE $(CONFIG_STAMP)
	+$(Q)$(MAKE) -f $(srctree)/tools/build/Makefile.build dir=$(FLUX_DIR) obj=iokd

# rule to build objects
$(OUTPUT)%-in.o: FORCE $(OUTPUT)lib/flux.o
	+$(Q)$(MAKE) -f $(srctree)/tools/build/Makefile.build dir=$(patsubst %/,%,$(dir $*)) obj=$(notdir $*)

clean:
	$(call QUIET_CLEAN, config)$(KERNEL_MAKE) clean
	$(call QUIET_CLEAN, objects) \
	if [ -d "$(OUTPUT)" ]; then \
		find "$(OUTPUT)" \
			-path "$(OUTPUT)third-party" -prune -o \
			-type f \( -name "*.d" -o -name "*.o" -o -name "*.cmd" \) \
			-exec rm -f {} +; \
	fi
	$(call QUIET_CLEAN, legacy-objects) \
	if [ -d "$(CURDIR)/$(FLUX_DIR)" ]; then \
		find "$(CURDIR)/$(FLUX_DIR)" \
			-type f \( -name "*.d" -o -name "*.o" -o -name "*.cmd" \) \
			-exec rm -f {} +; \
	fi
	$(call QUIET_CLEAN, headers)$(RM) -r $(OUTPUT)include/kernel/
	$(call QUIET_CLEAN, libflux.a)$(RM) $(OUTPUT)libflux.a
	$(call QUIET_CLEAN, targets)$(RM) $(TARGETS)

mrproper: clean
	$(call QUIET_CLEAN, vmlinux)$(KERNEL_MAKE) mrproper

clean-conf:
	$(call QUIET_CLEAN, autoconf.h)$(RM) $(AUTOCONF_H)
	$(call QUIET_CLEAN, .config)$(RM) $(FLUX_CONFIG)

FORCE: ;
.PHONY: all compile-commands clean mrproper conf conf-kernel menuconfig FORCE kernel-check
.NOTPARALLEL : $(OUTPUT)lib/flux.o
.SECONDARY:
.WAIT:
