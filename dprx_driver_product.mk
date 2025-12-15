# SPDX-License-Identifier: GPL-2.0-only

DPRX_DLKM_ENABLE := true
ifeq ($(TARGET_KERNEL_DLKM_DISABLE), true)
        ifeq ($(TARGET_KERNEL_DLKM_DPRX_OVERRIDE), false)
            DPRX_DLKM_ENABLE := false
        endif
endif

ifeq ($(DPRX_DLKM_ENABLE),  true)
        PRODUCT_PACKAGES += dprx.ko
        DPRX_MODULES_DRIVER := dprx.ko
endif

