# Native WMT daemons for the Gemini PDA Debian 13 rootfs (Slice 7, issue #8).
#
# Targets:
#   all      cross-build wmt_loader + wmt_launcher for aarch64 glibc (build/)
#   test     build and run host unit tests with the native compiler
#   install  copy binaries into DESTDIR (default: rootfs-overlay/) under usr/sbin
#   clean
#
# The daemons cannot be compiled on the project host (no aarch64 glibc
# toolchain there) — CI (ubuntu-24.04 + gcc-aarch64-linux-gnu) is the
# reference builder. `make CROSS_COMPILE= all` builds native-arch binaries
# for local sanity checks.

CROSS_COMPILE ?= aarch64-linux-gnu-
CC      := $(CROSS_COMPILE)gcc
HOSTCC  ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=

BUILDDIR := build
HOSTDIR  := $(BUILDDIR)/host
DESTDIR  ?= rootfs-overlay

LAUNCHER_SRCS := src/stp_uart_launcher.c src/wmt_patch.c src/wmt_cfg.c src/wmt_dev_wait.c
LOADER_SRCS   := src/wmt_loader.c src/wmt_dev_wait.c
HEADERS       := src/wmt_ioctl.h src/wmt_detect_ioctl.h src/wmt_patch.h \
                 src/wmt_cfg.h src/wmt_dev_wait.h

.PHONY: all test install clean

all: $(BUILDDIR)/wmt_loader $(BUILDDIR)/wmt_launcher

$(BUILDDIR)/wmt_loader: $(LOADER_SRCS) $(HEADERS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) $(LOADER_SRCS) -o $@ $(LDFLAGS)

$(BUILDDIR)/wmt_launcher: $(LAUNCHER_SRCS) $(HEADERS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) $(LAUNCHER_SRCS) -o $@ $(LDFLAGS)

# ---- host unit tests (parse logic only; no devices, no cross tools) ----

test: $(HOSTDIR)/test_wmt_cfg $(HOSTDIR)/test_wmt_patch
	$(HOSTDIR)/test_wmt_cfg tests/fixtures/WMT_SOC.cfg
	$(HOSTDIR)/test_wmt_patch

$(HOSTDIR)/test_wmt_cfg: tests/test_wmt_cfg.c src/wmt_cfg.c src/wmt_cfg.h
	@mkdir -p $(HOSTDIR)
	$(HOSTCC) $(CFLAGS) tests/test_wmt_cfg.c src/wmt_cfg.c -o $@

$(HOSTDIR)/test_wmt_patch: tests/test_wmt_patch.c src/wmt_patch.c src/wmt_patch.h
	@mkdir -p $(HOSTDIR)
	$(HOSTCC) $(CFLAGS) tests/test_wmt_patch.c src/wmt_patch.c -o $@

# ---- staging ----

install: all
	install -D -m 0755 $(BUILDDIR)/wmt_loader   $(DESTDIR)/usr/sbin/wmt_loader
	install -D -m 0755 $(BUILDDIR)/wmt_launcher $(DESTDIR)/usr/sbin/wmt_launcher

clean:
	rm -rf $(BUILDDIR)
