#!/bin/bash

sudo apt-get update && sudo apt -y install make cmake ninja-build g++ libtbb-dev libtbbmalloc2

cd ~
git clone https://github.com/axboe/liburing.git
cd liburing
./configure
make
sudo make install
