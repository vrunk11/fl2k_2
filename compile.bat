@echo off
mkdir build
cd build
cmake -G "MinGW Makefiles" ../ -DINSTALL_UDEV_RULES=ON
make -j 3
pause