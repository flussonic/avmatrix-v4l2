# Build of the hwsv4l2 module: a V4L2 driver for AVMatrix HWS
# PCIe capture cards. The kbuild part is in Kbuild; dkms runs this file with
# KERNELRELEASE set, so that variable means nothing here.
#
#   make            - build/hwsv4l2.ko for the running kernel
#   make KDIR=...   - against another kernel tree
#   make install-header - include/sdi_av.h and include/hwav.h into $(PREFIX)/include
#   make load       - reload the module on this machine (root); no other
#                     module may be bound to the card
#   make install-dkms - register the source with dkms and build for the
#                     running kernel (root); for distributions without a
#                     package of ours
#   make deb        - build/hwsv4l2-dkms_*.deb and hwsv4l2-dev (needs
#                     dpkg-buildpackage, debhelper, dh-dkms)
#
# The module version lives in dkms.conf.

MODULE      := hwsv4l2
VERSION     := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
KVER        ?= $(shell uname -r)
KDIR        ?= /lib/modules/$(KVER)/build
BUILD       := build

.PHONY: all clean load unload tools install-header install-dkms deb
PREFIX      ?= /usr/local
DKMS_SRC    := /usr/src/$(MODULE)-$(VERSION)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
	@mkdir -p $(BUILD) && cp $(MODULE).ko $(BUILD)/

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf $(BUILD)
	@test ! -d tools || $(MAKE) -C tools clean

tools:
	$(MAKE) -C tools

install-header:
	install -D -m 0644 include/sdi_av.h $(DESTDIR)$(PREFIX)/include/sdi_av.h
	install -D -m 0644 include/hwav.h $(DESTDIR)$(PREFIX)/include/hwav.h

# What the Debian package installs, by hand: the source under /usr/src,
# the other driver kept from autoloading, the module built and installed
# for the running kernel. dkms rebuilds it for kernels installed later.
install-dkms:
	install -d $(DKMS_SRC)/hwsv4l2 $(DKMS_SRC)/include
	install -m 0644 dkms.conf Makefile Kbuild $(DKMS_SRC)/
	install -m 0644 hwsv4l2/*.c hwsv4l2/*.h $(DKMS_SRC)/hwsv4l2/
	install -m 0644 include/sdi_av.h include/hwav.h $(DKMS_SRC)/include/
	install -D -m 0644 packaging/modprobe.d/$(MODULE).conf /etc/modprobe.d/$(MODULE).conf
	dkms add -m $(MODULE) -v $(VERSION) || true
	dkms install -m $(MODULE) -v $(VERSION) -k $(KVER)

deb:
	dpkg-buildpackage -us -uc -b
	@mkdir -p $(BUILD) && mv ../$(MODULE)-dkms_$(VERSION)_all.deb ../$(MODULE)-dev_$(VERSION)_all.deb $(BUILD)/
	@rm -f ../$(MODULE)_$(VERSION)_*.buildinfo ../$(MODULE)_$(VERSION)_*.changes
	@ls $(BUILD)/*.deb

unload:
	-rmmod $(MODULE) 2>/dev/null
	-rmmod HwsCapture 2>/dev/null

load: unload
	modprobe videodev
	modprobe videobuf2-v4l2
	modprobe videobuf2-vmalloc
	modprobe v4l2-dv-timings
	insmod $(BUILD)/$(MODULE).ko
