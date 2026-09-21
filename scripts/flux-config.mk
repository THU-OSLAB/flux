POSIX_HOSTS := elf64 elf32

SMP_ORIGIN := $(origin SMP)
FNET_ORIGIN := $(origin FNET)
FAST_NET_ORIGIN := $(origin FAST_NET)
SPDK_ORIGIN := $(origin SPDK)
RUNC_ORIGIN := $(origin RUNC)
MPK_ORIGIN := $(origin MPK)
MAX_CPUS_ORIGIN := $(origin MAX_CPUS)
flux_input_origins := command line environment environment override

define flux_set_config_var
$(eval CONFIG_FLUX_$(1) := $(2))
$(eval export CONFIG_FLUX_$(1))
endef

define flux_flag_enabled
$(if $(filter-out 0 n N no NO false FALSE,$(strip $(1))),y,)
endef

define flux_int_gt_one
$(strip $(shell [ "$(strip $(1))" -gt 1 ] 2>/dev/null && printf y))
endef

define flux_find_include
    $(eval include_paths := $(shell $(CC) -E -Wp,-v -xc /dev/null 2>&1 | grep '^ '))
    $(foreach f,$(include_paths),$(wildcard $(f)/$(1)))
endef

define flux_apply_posix_host
    $(call flux_set_config_var,POSIX,y)
    LDFLAGS += -pie -z noexecstack
    CFLAGS += -fPIC -pthread
    SOSUF := .so
    LDLIBS += -lrt -lpthread
    $(if $(call flux_find_include,numa.h),LDLIBS += -lnuma)
    $(if $(strip $(call flux_find_include,jsmn.h)),$(call flux_set_config_var,JSMN,y))
endef

define flux_configure_gnu
    export CROSS_COMPILE := $(CROSS_COMPILE)
    export CC := $(CROSS_COMPILE)gcc
    export LD := $(CROSS_COMPILE)ld
    export AR := $(CROSS_COMPILE)ar
    $(eval LD := $(CROSS_COMPILE)ld)
    $(eval CC := $(CROSS_COMPILE)gcc)
    $(eval LD_FMT := $(shell $(LD) -r -print-output-format))
endef

define flux_llvm_target_to_ld_fmt
    $(if $(filter $(CROSS_COMPILE),x86_64-linux-gnu),elf64-x86-64,\
        $(error Unsupported LLVM target $(CROSS_COMPILE)))
endef

define flux_configure_llvm
    $(eval LLVM_PREFIX := $(if $(filter %/,$(LLVM)),$(LLVM)))
    $(eval LLVM_SUFFIX := $(if $(filter -%,$(LLVM)),$(LLVM)))
    export CLANG_TARGET_FLAGS_lkl := $(CROSS_COMPILE)
    export CC := $(LLVM_PREFIX)clang$(LLVM_SUFFIX)
    export CXX := $(LLVM_PREFIX)clang++$(LLVM_SUFFIX)
    export LD := $(LLVM_PREFIX)ld.lld$(LLVM_SUFFIX)
    export AR := $(LLVM_PREFIX)llvm-ar$(LLVM_SUFFIX)
    $(eval LD := $(LLVM_PREFIX)ld.lld$(LLVM_SUFFIX))
    $(eval CC := $(LLVM_PREFIX)clang$(LLVM_SUFFIX))
    $(eval LD_FMT := $(call flux_llvm_target_to_ld_fmt))
endef

define flux_apply_build_config
	$(if $(LLVM),$(call flux_configure_llvm),$(call flux_configure_gnu))
	$(call flux_set_config_var,UINTR,y)
    $(if $(filter $(flux_input_origins),$(MAX_CPUS_ORIGIN)),\
        $(call flux_set_config_var,MAX_CPUS,$(MAX_CPUS)),\
        $(if $(strip $(CONFIG_FLUX_MAX_CPUS)),,$(call flux_set_config_var,MAX_CPUS,1)))
    $(if $(call flux_int_gt_one,$(CONFIG_FLUX_MAX_CPUS)),\
        $(if $(filter $(flux_input_origins),$(SMP_ORIGIN)),\
            $(if $(call flux_flag_enabled,$(SMP)),$(call flux_set_config_var,SMP,y),\
                $(error MAX_CPUS=$(CONFIG_FLUX_MAX_CPUS) requires SMP=1)),\
            $(error MAX_CPUS=$(CONFIG_FLUX_MAX_CPUS) requires SMP=1)),\
        $(if $(filter $(flux_input_origins),$(SMP_ORIGIN)),\
            $(if $(call flux_flag_enabled,$(SMP)),$(error SMP=1 requires MAX_CPUS>1),\
                $(call flux_set_config_var,SMP,n)),\
            $(call flux_set_config_var,SMP,n)))
    $(if $(filter $(flux_input_origins),$(FNET_ORIGIN)),$(if $(call flux_flag_enabled,$(FNET)),$(call flux_set_config_var,FNET,y),$(call flux_set_config_var,FNET,n)))
    $(if $(filter $(flux_input_origins),$(FAST_NET_ORIGIN)),$(if $(call flux_flag_enabled,$(FAST_NET)),$(call flux_set_config_var,FNET,y)))
    $(if $(filter $(flux_input_origins),$(FAST_NET_ORIGIN)),$(if $(call flux_flag_enabled,$(FAST_NET)),$(call flux_set_config_var,FAST_NET,y),$(call flux_set_config_var,FAST_NET,n)))
    $(if $(filter $(flux_input_origins),$(SPDK_ORIGIN)),$(if $(call flux_flag_enabled,$(SPDK)),$(call flux_set_config_var,SPDK,y),$(call flux_set_config_var,SPDK,n)))
    $(if $(filter $(flux_input_origins),$(RUNC_ORIGIN)),\
        $(if $(call flux_flag_enabled,$(RUNC)),$(call flux_set_config_var,RUNC,y),$(call flux_set_config_var,RUNC,n)),\
        $(if $(strip $(CONFIG_FLUX_RUNC)),,$(call flux_set_config_var,RUNC,y)))
    $(if $(filter $(flux_input_origins),$(MPK_ORIGIN)),$(if $(call flux_flag_enabled,$(MPK)),$(call flux_set_config_var,MPK,y),$(call flux_set_config_var,MPK,n)),\
        $(if $(strip $(CONFIG_FLUX_MPK)),,$(call flux_set_config_var,MPK,n)))
    $(eval EXEC_FMT := $(shell echo $(LD_FMT) | cut -d "-" -f1))
    $(if $(filter $(EXEC_FMT),$(POSIX_HOSTS)),$(call flux_apply_posix_host))
endef

CONFIG_EXPORTS := UINTR MAX_CPUS SMP FNET FAST_NET SPDK RUNC MPK POSIX JSMN

define flux_reset_exported_config
$(foreach name,$(CONFIG_EXPORTS),$(eval undefine CONFIG_FLUX_$(name)))
endef

define flux_autoconf_line
$(if $(filter y,$(CONFIG_FLUX_$(1))),#define CONFIG_FLUX_$(1) 1,$(if $(filter n,$(CONFIG_FLUX_$(1))),,#define CONFIG_FLUX_$(1) $(CONFIG_FLUX_$(1))))
endef

define flux_kconfig_line
$(if $(and $(filter MAX_CPUS,$(1)),$(filter-out y,$(CONFIG_FLUX_SMP))),,$(if $(filter y,$(CONFIG_FLUX_$(1))),CONFIG_FLUX_$(1)=y,$(if $(filter n,$(CONFIG_FLUX_$(1))),# CONFIG_FLUX_$(1) is not set,CONFIG_FLUX_$(1)=$(CONFIG_FLUX_$(1)))))
endef

define flux_emit_config
$(file >$(AUTOCONF_H),)
$(file >$(FLUX_CONFIG),)
$(foreach name,$(CONFIG_EXPORTS),\
  $(if $(strip $(CONFIG_FLUX_$(name))),\
    $(if $(call flux_autoconf_line,$(name)),$(file >>$(AUTOCONF_H),$(call flux_autoconf_line,$(name))))\
    $(if $(call flux_kconfig_line,$(name)),$(file >>$(FLUX_CONFIG),$(call flux_kconfig_line,$(name))))\
  )\
)
endef
