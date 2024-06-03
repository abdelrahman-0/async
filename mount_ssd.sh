#!/bin/bash

DEVICE=

cd ~
mkdir /mnt/ssd_device
sudo mkfs.ext4 /dev/$DEVICE
sudo mount -t auto -v /dev/$DEVICE /mnt/ssd_device
