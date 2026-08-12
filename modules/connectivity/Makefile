# Convenience wrapper: build the connectivity modules against a prepared
# kernel tree.
#
#   make KDIR=/path/to/linux-6.6 ARCH=arm64 CROSS_COMPILE=aarch64-linux-
#
KDIR ?= /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
