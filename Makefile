DPRX_ROOT=$(ROOTDIR)vendor/qcom/opensource/dprx
CONFIG_DPRX=$(MODULE_DPRX)
KBUILD_OPTIONS := DPRX_ROOT=$(DPRX_ROOT) CONFIG_DPRX=$(CONFIG_DPRX)

ifeq ($(TARGET_SUPPORT),genericarmv8)
	KBUILD_OPTIONS += CONFIG_ARCH_SUN=y
endif

obj-m += dprcx/

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) INSTALL_MOD_STRIP=1 -C $(KERNEL_SRC) M=$(M) modules_install

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)

clean:
	rm -f *.o *.ko *.mod.c *.mod.o *~ .*.cmd Module.symvers
	rm -rf .tmp_versions
