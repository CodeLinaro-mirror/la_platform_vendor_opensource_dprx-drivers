#SPDX-License-Identifier: GPL-2.0-only
DPRX_DLKM_ENABLE := true
ifeq ($(TARGET_KERNEL_DLKM_DISABLE), true)
	ifeq ($(TARGET_KERNEL_DLKM_DPRX_OVERRIDE), false)
		DPRX_DLKM_ENABLE := false
	endif
endif

ifneq ($(TARGET_DISABLE_DPRX_DLKM),true)
ifeq ($(DPRX_DLKM_ENABLE),  true)
	ifeq ($(call is-board-platform-in-list,$(TARGET_BOARD_PLATFORM)),true)
		BOARD_VENDOR_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/dprx.ko
		BOARD_VENDOR_RAMDISK_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/dprx.ko
		BOARD_VENDOR_RAMDISK_RECOVERY_KERNEL_MODULES_LOAD += $(KERNEL_MODULES_OUT)/dprx.ko
	endif
endif
endif
