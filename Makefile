# Out-of-tree build of acx-mac80211, USB-only (TNETW1450, see Kbuild).
# Building PCI support into the same module oopses on USB devices, so the
# old EXTRA_KCONFIG standalone build is gone.

KVERSION ?= $(shell uname -r)
KERNELDIR ?= /lib/modules/$(KVERSION)/build

all modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) modules

install modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) clean

.PHONY: all modules install modules_install clean
