#
# acrn-hypervisor/hypervisor/Makefile
#

API_MAJOR_VERSION=1
API_MINOR_VERSION=0

GCC_MAJOR=$(shell echo __GNUC__ | $(CC) -E -x c - | tail -n 1)
GCC_MINOR=$(shell echo __GNUC_MINOR__ | $(CC) -E -x c - | tail -n 1)

#enable stack overflow check
STACK_PROTECTOR := 1

MAKEFLAGS += -rR --no-print-directory

BASEDIR := $(shell pwd)
GIT_TOPDIR := $(shell git rev-parse --show-toplevel 2>/dev/null)
LICENSE_FILE := $(or $(firstword $(wildcard ../LICENSE $(if $(GIT_TOPDIR),$(GIT_TOPDIR)/LICENSE))),/dev/null)
HV_OBJDIR ?= $(CURDIR)/build
HV_MODDIR ?= $(HV_OBJDIR)/modules
HV_FILE := acrn

# initialize the flags we used
CFLAGS :=
ASFLAGS :=
LDFLAGS :=
ARFLAGS :=
ARCH_CFLAGS :=
ARCH_ASFLAGS :=
ARCH_ARFLAGS :=
ARCH_LDFLAGS :=

PLATFORM ?=
STATIC_QEMU_ARM64_CONFIG := $(if $(filter arm64,$(ARCH)),$(if $(filter qemu,$(PLATFORM)),y,))

ifeq ($(STATIC_QEMU_ARM64_CONFIG),y)
BOARD := qemu
SCENARIO := qemu
RELEASE ?= n
CONFIG_BOARD := qemu
CONFIG_SCENARIO := qemu
CONFIG_RELEASE := n
CONFIG_RELOC := n
CONFIG_MULTIBOOT2 := n
CONFIG_FDT_PARSE_ENABLED := n
CONFIG_STATIC_VFDT := y
CONFIG_GUEST_KERNEL_RAWIMAGE := y
CONFIG_GUEST_KERNEL_BZIMAGE := n
CONFIG_GUEST_KERNEL_ELF := n
CONFIG_SCHED_IORR := y
HV_CONFIG_DIR := $(HV_OBJDIR)/configs
HV_CONFIG_H := arch/arm64/platform/qemu/qemu_config.h
CFLAGS += -include $(HV_CONFIG_H)
HV_CONFIG_MK := $(HV_CONFIG_DIR)/config.mk
HV_CONFIG_TIMESTAMP := $(HV_CONFIG_DIR)/.configfiles.timestamp
HV_DIFFCONFIG_LIST := $(HV_CONFIG_DIR)/.diffconfig
else
include scripts/makefile/config.mk
endif

ifeq ($(STATIC_QEMU_ARM64_CONFIG),y)
BOARD_INFO_DIR := arch/arm64/platform/qemu
SCENARIO_CFG_DIR := arch/arm64/platform/qemu
BOARD_CFG_DIR := arch/arm64/platform/qemu
else
BOARD_INFO_DIR := $(HV_CONFIG_DIR)/boards
SCENARIO_CFG_DIR := $(HV_CONFIG_DIR)/scenarios/$(SCENARIO)
BOARD_CFG_DIR := $(SCENARIO_CFG_DIR)
endif

ifeq ($(V), 1)
	Q :=
else
	Q := @
endif

-include ../paths.make
libdir ?= /usr/lib
sysconfdir ?= /etc

LD_IN_TOOL = scripts/genld.sh
BASH = $(shell which bash)

ARFLAGS += crs

CFLAGS += -Wall -W
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -fshort-wchar -ffreestanding
CFLAGS += -fsigned-char
CFLAGS += -nostdinc -nostdlib -fno-common
CFLAGS += -Werror

# ACRN depends on zero length array. Silence the gcc if Warrary-bounds is default option
CFLAGS += -Wno-array-bounds
CFLAGS += -O2

ifdef STACK_PROTECTOR
ifeq (true, $(shell [ $(GCC_MAJOR) -gt 4 ] && echo true))
CFLAGS += -fstack-protector-strong
else
ifeq (true, $(shell [ $(GCC_MAJOR) -eq 4 ] && [ $(GCC_MINOR) -ge 9 ] && echo true))
CFLAGS += -fstack-protector-strong
else
CFLAGS += -fstack-protector
endif
endif
CFLAGS += -DSTACK_PROTECTOR
endif

ifeq (y, $(CONFIG_MULTIBOOT2))
ASFLAGS += -DCONFIG_MULTIBOOT2
endif

ifeq (y, $(CONFIG_RELOC))
ASFLAGS += -DCONFIG_RELOC
endif

LDFLAGS += -Wl,--gc-sections -nostartfiles -nostdlib
LDFLAGS += -Wl,-n,-z,max-page-size=0x1000
LDFLAGS += -Wl,--no-dynamic-linker

ifeq (y, $(CONFIG_RELEASE))
LDFLAGS += -s
endif

ARCH_CFLAGS += -gdwarf-2
ARCH_ASFLAGS += -gdwarf-2 -DASSEMBLER=1
ARCH_ARFLAGS +=
ARCH_LDFLAGS +=

ARCH_LDSCRIPT = $(HV_OBJDIR)/link_ram.ld

REL_INCLUDE_PATH += include
REL_INCLUDE_PATH += include/lib
REL_INCLUDE_PATH += include/lib/crypto
REL_INCLUDE_PATH += include/lib/libfdt
REL_INCLUDE_PATH += include/common
REL_INCLUDE_PATH += include/debug
REL_INCLUDE_PATH += include/public
REL_INCLUDE_PATH += include/dm
REL_INCLUDE_PATH += include/hw
REL_INCLUDE_PATH += sdk/boot/include
REL_INCLUDE_PATH += sdk/boot/include/guest

ARCH ?= x86
REL_INCLUDE_PATH += include/arch/$(ARCH)

INCLUDE_PATH := $(realpath $(REL_INCLUDE_PATH))
INCLUDE_PATH += $(HV_OBJDIR)/include
INCLUDE_PATH += $(BOARD_INFO_DIR)
INCLUDE_PATH += $(BOARD_CFG_DIR)
INCLUDE_PATH += $(SCENARIO_CFG_DIR)

CC ?= gcc
AS ?= as
AR ?= ar
LD ?= ld
OBJCOPY ?= objcopy

include arch/$(ARCH)/Makefile

LIB_DEBUG = $(HV_MODDIR)/libdebug.a
LIB_RELEASE = $(HV_MODDIR)/librelease.a

export ARCH
export CC AS AR LD OBJCOPY
export CFLAGS ASFLAGS ARFLAGS LDFLAGS ARCH_CFLAGS ARCH_ASFLAGS ARCH_ARFLAGS ARCH_LDFLAGS
export HV_OBJDIR HV_MODDIR CONFIG_RELEASE INCLUDE_PATH
export LIB_DEBUG LIB_RELEASE

ifneq ($(CONFIG_RELEASE),y)
CFLAGS += -DHV_DEBUG -DPROFILING_ON -fno-omit-frame-pointer
endif

COMMON_C_SRCS += core/vcpu.c
COMMON_C_SRCS += core/vm.c

# FIXME: During initial development stage of riscv enabling,
# we would like to first confine the core files to x86-only.
# As we progress through the riscv enabling process, multi-arch
# and modularization of below modules will be done per-feature.
#
# TODO: When a module is done refactoring, move the line out from below
# if block. The block should be entirely eliminated when we've done
# all the work.
#
COMMON_C_SRCS += core/notify.c
COMMON_C_SRCS += core/percpu.c
COMMON_C_SRCS += core/cpu.c
COMMON_C_SRCS += core/ticks.c
COMMON_C_SRCS += core/delay.c
COMMON_C_SRCS += core/timer.c
COMMON_C_SRCS += core/softirq.c
COMMON_C_SRCS += core/trace.c
COMMON_C_SRCS += core/schedule.c
COMMON_C_SRCS += core/mmu.c
ifeq ($(CONFIG_SCHED_NOOP),y)
COMMON_C_SRCS += core/sched_noop.c
endif
ifeq ($(CONFIG_SCHED_BVT),y)
COMMON_C_SRCS += core/sched_bvt.c
endif
ifeq ($(CONFIG_SCHED_IORR),y)
COMMON_C_SRCS += core/sched_iorr.c
endif
ifeq ($(CONFIG_SCHED_PRIO),y)
COMMON_C_SRCS += core/sched_prio.c
endif
COMMON_C_SRCS += core/sbuf.c
COMMON_C_SRCS += core/logmsg.c
COMMON_C_SRCS += core/irq.c

# library componment
COMMON_C_SRCS += lib/memory.c
COMMON_C_SRCS += lib/bits.c
COMMON_C_SRCS += lib/string.c
COMMON_C_SRCS += lib/crypto/crypto_api.c
COMMON_C_SRCS += lib/crypto/mbedtls/hkdf.c
COMMON_C_SRCS += lib/crypto/mbedtls/sha256.c
COMMON_C_SRCS += lib/crypto/mbedtls/md.c
COMMON_C_SRCS += lib/crypto/mbedtls/md_wrap.c
COMMON_C_SRCS += lib/sprintf.c

FDT_LIB_ENABLED := n
ifeq ($(CONFIG_FDT_PARSE_ENABLED),y)
FDT_LIB_ENABLED := y
endif
ifeq ($(CONFIG_STATIC_VFDT),y)
FDT_LIB_ENABLED := y
endif

ifeq ($(FDT_LIB_ENABLED),y)
COMMON_C_SRCS += lib/fdt/fdt.c
COMMON_C_SRCS += lib/fdt/fdt_addresses.c
COMMON_C_SRCS += lib/fdt/fdt_check.c
COMMON_C_SRCS += lib/fdt/fdt_empty_tree.c
COMMON_C_SRCS += lib/fdt/fdt_overlay.c
COMMON_C_SRCS += lib/fdt/fdt_ro.c
COMMON_C_SRCS += lib/fdt/fdt_rw.c
COMMON_C_SRCS += lib/fdt/fdt_strerror.c
COMMON_C_SRCS += lib/fdt/fdt_sw.c
COMMON_C_SRCS += lib/fdt/fdt_wip.c
endif

ifeq ($(CONFIG_FDT_PARSE_ENABLED),y)
COMMON_C_SRCS += core/vfdt.c
else
COMMON_C_SRCS += core/vfdt_static.c
endif

ifdef STACK_PROTECTOR
COMMON_C_SRCS += lib/stack_protector.c
endif
COMMON_C_SRCS += core/vm_config.c
COMMON_C_SRCS += core/event.c
COMMON_C_SRCS += core/fdt.c
COMMON_C_SRCS += sdk/boot/guest/vboot_info.c
COMMON_C_SRCS += core/vm_load.c
ifeq ($(CONFIG_GUEST_KERNEL_RAWIMAGE),y)
COMMON_C_SRCS += sdk/boot/guest/rawimage_loader.c
endif
COMMON_C_SRCS += sdk/boot/boot.c
COMMON_C_SRCS += sdk/boot/multiboot/multiboot.c
ifeq ($(CONFIG_MULTIBOOT2),y)
COMMON_C_SRCS += sdk/boot/multiboot/multiboot2.c
endif
COMMON_C_SRCS += sdk/boot/bare.c

# dm componment
COMMON_C_SRCS += sdk/dm/vuart.c
ifneq ($(filter $(ARCH),x86 arm64),)
COMMON_C_SRCS += sdk/dm/io_req.c
endif

ifeq ($(ARCH),x86)
COMMON_C_SRCS += core/efi_mmap.c
COMMON_C_SRCS += core/vm_event.c
COMMON_C_SRCS += core/hv_main.c
COMMON_C_SRCS += core/hypercall.c
COMMON_C_SRCS += core/ptdev.c
COMMON_C_SRCS += sdk/dm/vrtc.c
COMMON_C_SRCS += sdk/dm/vpci/vdev.c
COMMON_C_SRCS += sdk/dm/vpci/vpci.c
COMMON_C_SRCS += sdk/dm/vpci/vroot_port.c
COMMON_C_SRCS += sdk/dm/vpci/vpci_bridge.c
COMMON_C_SRCS += sdk/dm/vpci/vpci_mf_dev.c
COMMON_C_SRCS += sdk/dm/vpci/ivshmem.c
COMMON_C_SRCS += sdk/dm/vpci/pci_pt.c
COMMON_C_SRCS += sdk/dm/vpci/vmsi.c
COMMON_C_SRCS += sdk/dm/vpci/vmsix.c
COMMON_C_SRCS += sdk/dm/vpci/vmsix_on_msi.c
COMMON_C_SRCS += sdk/dm/vpci/vsriov.c
ifeq ($(CONFIG_VMCS9900),y)
COMMON_C_SRCS += sdk/dm/vpci/vmcs9900.c
endif
COMMON_C_SRCS += sdk/dm/mmio_dev.c
COMMON_C_SRCS += sdk/dm/vgpio.c
ifeq ($(CONFIG_GUEST_KERNEL_BZIMAGE),y)
COMMON_C_SRCS += sdk/boot/guest/bzimage_loader.c
endif
ifeq ($(CONFIG_GUEST_KERNEL_ELF),y)
COMMON_C_SRCS += sdk/boot/guest/elf_loader.c
endif
COMMON_C_SRCS += sdk/hw/pci.c
endif # ifeq ($(ARCH),x86)

ifeq ($(ARCH),arm64)
COMMON_C_SRCS += arch/arm64/guest/hypercall.c
endif

COMMON_C_OBJS := $(patsubst %.c,$(HV_OBJDIR)/%.o,$(COMMON_C_SRCS))

COMMON_MOD = $(HV_MODDIR)/core_mod.a

MODULES += $(COMMON_MOD)

ifeq ($(CONFIG_RELEASE),y)
MODULES += $(LIB_RELEASE)
LIB_BUILD = $(LIB_RELEASE)
LIB_MK = sdk/release/Makefile
else
MODULES += $(LIB_DEBUG)
LIB_BUILD = $(LIB_DEBUG)
LIB_MK = sdk/debug/Makefile
endif

DISTCLEAN_OBJS := $(shell find $(BASEDIR) -name '*.o')
VERSION := $(HV_OBJDIR)/include/version.h
HEADERS := $(VERSION) $(HV_CONFIG_H) $(HV_CONFIG_TIMESTAMP)

ifeq ($(STATIC_QEMU_ARM64_CONFIG),y)
$(HV_CONFIG_MK): | $(HV_CONFIG_DIR)
	@echo "CONFIG_HV_RAM_START=0x50000000" > $@

$(HV_CONFIG_TIMESTAMP): $(HV_CONFIG_MK)
	@touch $@

$(HV_CONFIG_DIR):
	@mkdir -p $@
endif

.PHONY: all
all: $(ARCH_ALL_TARGETS) $(HV_OBJDIR)/$(HV_FILE).bin

.PHONY: lib

$(LIB_BUILD): $(HEADERS)
	$(Q)$(MAKE) -f $(LIB_MK) MKFL_NAME=$(LIB_MK)

lib: $(LIB_BUILD)

.PHONY: core-mod

core-mod: $(COMMON_MOD)

$(COMMON_MOD): $(COMMON_C_OBJS)
	$(Q)echo "ar        $(notdir $@)"
	$(Q)$(AR) $(ARFLAGS) $(COMMON_MOD) $(COMMON_C_OBJS)

$(HV_OBJDIR)/$(HV_FILE).bin: $(HV_OBJDIR)/$(HV_FILE).out
	$(Q)echo "objcopy   $(notdir $@)"
	$(Q)$(OBJCOPY) -O binary $< $(HV_OBJDIR)/$(HV_FILE).bin
	$(Q)rm -f $(UPDATE_RESULT)

$(HV_OBJDIR)/$(HV_FILE).out: $(MODULES)
	$(Q)echo "cc        $(notdir $@)"
	$(Q)${BASH} ${LD_IN_TOOL} $(ARCH_LDSCRIPT_IN) $(ARCH_LDSCRIPT) ${HV_CONFIG_MK}
	$(Q)$(CC) -Wl,-Map=$(HV_OBJDIR)/$(HV_FILE).map -o $@ $(LDFLAGS) $(ARCH_LDFLAGS) -T$(ARCH_LDSCRIPT) \
		-Wl,--start-group $^ -Wl,--end-group

.PHONY: clean
clean:
	rm -rf $(VERSION)
	rm -rf $(HV_OBJDIR)

.PHONY: distclean
distclean:
	rm -f $(DISTCLEAN_OBJS)
	rm -f $(VERSION)
	rm -rf $(HV_OBJDIR)
	rm -f tags TAGS cscope.files cscope.in.out cscope.out cscope.po.out GTAGS GPATH GRTAGS GSYMS

PHONY: (VERSION)
$(VERSION): $(HV_CONFIG_H)
	@mkdir -p $(dir $(VERSION))
	@touch $(VERSION)
	@if [ "$(BUILD_VERSION)"x = x ];then \
		COMMIT=`git rev-parse --verify --short HEAD 2>/dev/null`;\
		DIRTY=`git diff-index --name-only HEAD`;\
		if [ -n "$$DIRTY" ];then PATCH="$$COMMIT-dirty";else PATCH="$$COMMIT";fi;\
	else \
		PATCH="$(BUILD_VERSION)"; \
	fi; \
	COMMIT_TAGS=$$(git tag --points-at HEAD|tr -s "\n" " "); \
	COMMIT_TAGS=$$(eval echo $$COMMIT_TAGS);\
	COMMIT_TIME=$$(git log -1 --date=format:"%Y-%m-%d-%T" --format=%cd); \
	TIME=$$(date -u -d "@$${SOURCE_DATE_EPOCH:-$$(date +%s)}" "+%F %T"); \
	USER="$${USER:-$$(id -u -n)}"; \
	if [ x$(CONFIG_RELEASE) = "xy" ];then BUILD_TYPE="REL";else BUILD_TYPE="DBG";fi;\
	echo "/*" > $(VERSION); \
	sed 's/^/ * /' "$(LICENSE_FILE)" >> $(VERSION); \
	echo " */" >> $(VERSION); \
	echo "" >> $(VERSION); \
	echo "#ifndef VERSION_H" >> $(VERSION); \
	echo "#define VERSION_H" >> $(VERSION); \
	echo "#define HV_API_MAJOR_VERSION $(API_MAJOR_VERSION)U" >> $(VERSION);\
	echo "#define HV_API_MINOR_VERSION $(API_MINOR_VERSION)U" >> $(VERSION);\
	echo "#define HV_BRANCH_VERSION "\"$(BRANCH_VERSION)\""" >> $(VERSION);\
	echo "#define HV_COMMIT_DIRTY "\""$$PATCH"\""" >> $(VERSION);\
	echo "#define HV_COMMIT_TAGS "\"$$COMMIT_TAGS\""" >> $(VERSION);\
	echo "#define HV_COMMIT_TIME "\"$$COMMIT_TIME\""" >> $(VERSION);\
	echo "#define HV_BUILD_TYPE "\""$$BUILD_TYPE"\""" >> $(VERSION);\
	echo "#define HV_BUILD_TIME "\""$$TIME"\""" >> $(VERSION);\
	echo "#define HV_BUILD_USER "\""$$USER"\""" >> $(VERSION);\
	echo "#define HV_BUILD_SCENARIO "\"$(SCENARIO)\""" >> $(VERSION);\
	echo "#define HV_BUILD_BOARD "\"$(BOARD)\""" >> $(VERSION);\
	echo "#endif" >> $(VERSION)

-include $(C_OBJS:.o=.d)
-include $(S_OBJS:.o=.d)

$(HV_OBJDIR)/%.o: %.c $(HEADERS) $(ARCH_PRE_BUILD_TARGETS)
	$(Q)[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "cc        $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@

$(VM_CFG_C_SRCS): %.c: $(HV_CONFIG_TIMESTAMP)

$(VM_CFG_C_OBJS): $(HV_OBJDIR)/%.o: %.c $(HEADERS) $(ARCH_PRE_BUILD_TARGETS)
	$(Q)[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "cc        $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@

$(HV_OBJDIR)/%.o: %.S $(HEADERS) $(ARCH_PRE_BUILD_TARGETS)
	$(Q)[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "cc        $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. $(ASFLAGS) $(ARCH_ASFLAGS) -c $< -o $@ -MMD -MT $@

.DEFAULT_GOAL := all
