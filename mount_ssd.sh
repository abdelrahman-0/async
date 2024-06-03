#!/bin/bash

# lsblk -o NAME,PHY-SeC

DEVICE=

cd ~
sudo mkdir /mnt/ssd_device
sudo mkfs.ext4 /dev/$DEVICE
sudo mount -t auto -v /dev/$DEVICE /mnt/ssd_device
