#!/bin/sh
mkdir build
cd build
cmake ../ -DINSTALL_UDEV_RULES=ON
make -j 3
sudo make install
sudo ldconfig
sudo udevadm control -R
sudo udevadm trigger
