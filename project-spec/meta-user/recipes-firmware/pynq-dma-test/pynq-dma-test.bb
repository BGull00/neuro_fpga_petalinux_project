#
# This file is the pynq-dma-test recipe.
#

SUMMARY = "Simple pynq-dma-test to use dfx_dtg_zynq_full class"
SECTION = "PETALINUX/apps"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

inherit dfx_dtg_zynq_full

COMPATIBLE_MACHINE:zynq = ".*"


SRC_URI = "file://design_1_wrapper.xsa \
	file://pl-custom.dtsi"
