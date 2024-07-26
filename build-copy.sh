#!/bin/bash

# Build
cd /home/bryson/Documents/Xilinx_Projects/pynq_dma_test/petalinux
petalinux-build

# Package
cd /home/bryson/Documents/Xilinx_Projects/pynq_dma_test/petalinux/images/linux
petalinux-package boot --u-boot --force

# Copy image to SD card
cp BOOT.BIN /media/bryson/8441-F5BF/
cp boot.scr /media/bryson/8441-F5BF/
cp image.ub /media/bryson/8441-F5BF/
sudo tar xvfp rootfs.tar.gz -C /media/bryson/dd8edc57-7308-476f-b90a-4f3dd6776c58/

# Copy boot (network indpendent) .bin and .dtbo files (bitstream and device tree overlay) to SD card
cd /home/bryson/Documents/Xilinx_Projects/pynq_dma_test/petalinux
sudo rm /media/bryson/dd8edc57-7308-476f-b90a-4f3dd6776c58/lib/firmware/xilinx/pynq-dma-test/pynq-dma-test.bin
sudo cp design_1_wrapper.bit.bin /media/bryson/dd8edc57-7308-476f-b90a-4f3dd6776c58/lib/firmware/xilinx/pynq-dma-test/
sudo cp pl.dtbo /media/bryson/dd8edc57-7308-476f-b90a-4f3dd6776c58/lib/firmware/xilinx/pynq-dma-test/pynq-dma-test.dtbo

# Generate network .bin file and copy it to SD card
rm network.bit
rm network.bit.bin
cp ../vivado/pynq_dma_test.runs/impl_1/design_1_wrapper.bit network.bit
bootgen -image Full_Bitstream.bif -arch zynq -process_bitstream bin
sudo cp network.bit.bin /media/bryson/dd8edc57-7308-476f-b90a-4f3dd6776c58/lib/firmware/xilinx/pynq-dma-test/
sudo putty
