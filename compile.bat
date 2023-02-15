@echo off
set /p help=Do you want somme help for setup the compilation environement ? (1 = yes / 0 = no) :
if %help% EQU 0 goto :compile
echo if you have some error folow those steps for instaling all requiered tool for compiling:
echo ---------------------------------------------------------------------------------------- 
echo install cmake by downloading from here : "https://cmake.org/download/" (select "add variable to path" during instalation)
echo for instaling mingw you can download mingw for windows here : https://winlibs.com/#download-release and add the binaries to the system path
echo for instaling libusb download from here https://libusb.info/ latest windows binaries then copy the file folowing this rules :
echo copy the content of the "libusb-Mingw-x64/libs" inside the folder "mingw64/libs"
echo copy the file "libusb.h" from "libusb-Mingw-x64/include/libusb-1.0" inside the folder "mingw64/include"
echo copy the file "msys-usb-1.0.dll" from "libusb-Mingw-x64/bin" inside the folder "mingw64/bin" then rename it "libusb-1.0.dll"
echo download the lastest release from this repo https://github.com/skeeto/w64devkit and copy the file make.exe at the root of the project
echo you should now be ready to compile
pause

:compile

mkdir build
cd build
cmake -G "MinGW Makefiles" ../ -DINSTALL_UDEV_RULES=ON
start ../make.exe -j 3
cd src
start .
pause