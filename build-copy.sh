#!/bin/bash

cd /home/bryson/Documents/Xilinx_Projects/pynq_dma_test/petalinux
petalinux-build
cd /home/bryson/Documents/Xilinx_Projects/pynq_dma_test/petalinux/images/linux
petalinux-package boot --u-boot --force
cp BOOT.BIN /media/bryson/8441-F5BF/
cp boot.scr /media/bryson/8441-F5BF/
cp image.ub /media/bryson/8441-F5BF/
sudo tar xvfp rootfs.tar.gz -C /media/bryson/dd8edc57-7308-476f-b90a-4f3dd6776c58/
cd /home/bryson/Documents/Xilinx_Projects/pynq_dma_test/petalinux
sudo rm /media/bryson/dd8edc57-7308-476f-b90a-4f3dd6776c58/lib/firmware/xilinx/pynq-dma-test/pynq-dma-test.bin
sudo cp project-spec/hw-description/design_1_wrapper.bit.bin /media/bryson/dd8edc57-7308-476f-b90a-4f3dd6776c58/lib/firmware/xilinx/pynq-dma-test/
sudo cp pl.dtbo /media/bryson/dd8edc57-7308-476f-b90a-4f3dd6776c58/lib/firmware/xilinx/pynq-dma-test/pynq-dma-test.dtbo
sudo putty
