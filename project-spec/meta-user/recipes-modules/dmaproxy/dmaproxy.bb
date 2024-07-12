SUMMARY = "Recipe for  build an external dmaproxy Linux kernel module"
SECTION = "PETALINUX/modules"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING;md5=12f884d2ae1ff87c09e5b7ccc2c4ca7e"

inherit module

INHIBIT_PACKAGE_STRIP = "1"

SRC_URI = "file://Makefile \
           file://dmaproxy.c \
           file://dmaproxy.h \
	       file://COPYING \
          "

S = "${WORKDIR}"

do_install:append() {
    install -d ${D}/usr/include/dmaproxy
    install -m 0755 ${S}/dmaproxy.h ${D}/usr/include/dmaproxy/
}

# The inherit of module.bbclass will automatically name module packages with
# "kernel-module-" prefix as required by the oe-core build environment.
