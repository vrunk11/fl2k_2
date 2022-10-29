Fork of the osmo_fl2K project.

Turns FL2000-based USB 3.0 to VGA adapters into low cost DACs.

For more information on sorce see https://osmocom.org/projects/osmo-fl2k/wiki

This project fork is primarily for use with playing TBC files the following is for that use case.

# FL2K TBC Player

A Simple TBC playback utility, currently only CLI (Command Line Interface)

This will later be both GUI/CLI.

## What is a TBC? 

Its a digital Time Base Corrected lossless 16-bit video file thats in 1 file for Composite streams and 2 files for S-Video streams.

## How do I get a TBC file? 

Via [VHS-Decode](https://github.com/oyvindln/vhs-decode) (Tape Decoding) and [LD-Decode](https://github.com/happycube/ld-decode) (LaserDisk Decoding) or [CVBS-Decode](https://github.com/oyvindln/vhs-decode/wiki/CVBS-Composite-Decode) (Composite Decoding) 

## Ware you can buy the FL2K and adapters

The FL2K [Link 1](https://www.aliexpress.com/item/1005002872152601.html?) / [Link 2](https://www.reichelt.de/de/de/adapterkabel-usb-3-0-stecker-vga-buchse-schwarz-delock-62738-p287335.html)

VGA to RCA [Aliexpress](https://www.aliexpress.com/item/1005002872152601.html?)

VGA to BNC Male/Female [Amazon UK](https://www.amazon.co.uk/gp/product/B0033AF5Y0/) / [Amazon USA](https://www.amazon.com/s?k=VGA+to+BNC+Cable&crid=30JGI1TOFQ5I9&sprefix=vga+to+bnc+cable%2Caps%2C165&ref=nb_sb_noss_1)

# Setup 

## Standardised Cable Config 

### Composite 

Red - Right Audio

Blue - Left Audio

Green - Composite Video 

### S-Video 

Green - Lumanace Y 

Blue - Chroma C 

Red - Mono/Mono Mix Audio

## Windows 

Simply download the latest [Windows Release](https://github.com/vrunk11/fl2k_2/releases).

Decompress the .zip file.

For GUI users 

Open the fl2k_2.bat file.

For CLI users 

Open an CMD Window and open the directory with CD once inside simply use fl2k-tbc-player.exe to open the software.

## Linux

### Download Dependencies 

`sudo apt-get install libusb-1.0-0-dev`

Install the libusb headers if not already present

`git clone https://gitea.osmocom.org/sdr/osmo-fl2k
mkdir osmo-fl2k/build
cd osmo-fl2k/build`

`cmake ../ -DINSTALL_UDEV_RULES=ON
make -j 3
sudo make install
sudo ldconfig`

Before being able to use the device as a non-root user, the udev rules need to be reloaded:

`sudo udevadm control -R`

`sudo udevadm trigger`

### Download The Player 

`git clone https://github.com/vrunk11/fl2k_2/.git fl2k-tbc-player`

To enter into the install directory use CD 

`cd fl2k-tbc-player`

Compile the player with 

`sudo compile.sh`

Run the software with 

`./fl2k_2 [arguments]`

## MacOS 

Support yet to be Implimented.

# Usage 

As its an VGA R-G-B adapter so there is 3 ADC's

To play a file on an ADC channel you do -R for red -G for green and -B for blue

For the Samplerate/TV System you can do `-s ntsc` or `-s pal`

Currently to make tbc playback possible you need to do the 16 to 8 bit conversion with:

-R16 for red -G16 for green -B16 for blue 

Also needed is removal of the extra line on each frame:

-tbcR for red -tbcG for green -tbcB for blue

## Standard Oprational Commands

For windows just use fl2k_2.exe 

### Composite output on the red channel:

Linux:

`./fl2k_2 -u -s ntsc -G16 -tbcG -G example.tbc`

Windows:

`fl2k_2.exe -u -s ntsc -G16 -tbcG -G example.tbc`

### S-Video output with Luma on the green channel and Chroma on the blue channel:

Linux:

`./fl2k_2 -u -s pal -G16 -tbcG -G example.tbc -B16 -tbcB -B example_chroma.tbc`

Windows:

`fl2k_2.exe -u -s pal -G16 -tbcG -G example.tbc -B16 -tbcB -B example_chroma.tbc`

# Commandlist

`-d` device_index (default: 0)

`-readMode` (default = 0) option : 0 = multit-threading (RGB) / 1 = hybrid (R --> GB) / 2 = hybrid (RG --> B) / 3 = sequential (R -> G -> B)

`-s` samplerate (default: 100 MS/s) allows you to change TV System `-s ntsc` or `-s pal`

`-u` Set sample type to unsigned

`-R` filename (use '-' to read from stdin)

`-G` filename (use '-' to read from stdin)

`-B` filename (use '-' to read from stdin)

`-R16` (convert bits 16 to 8)

`-G16` (convert bits 16 to 8)

`-B16` (convert bits 16 to 8)

`-tbcR` interpret R as tbc file

`-tbcG` interpret G as tbc file

`-tbcB` interpret B as tbc file

`-CgainR` Control Signal Gain Level 

`-CgainG` Control Signal Gain Level 

`-CgainB` Control Signal Gain Level 

## Possible USB Issues

You might see this in Linux:

`Allocating 6 zero-copy buffers
libusb: error [op_dev_mem_alloc] alloc dev mem failed errno 12
Failed to allocate zero-copy buffer for transfer 4`

If so then you can then increase your allowed usbfs buffer size with the following command:

`echo 0 > /sys/module/usbcore/parameters/usbfs_memory_mb`

Falling back to buffers in userspace
Requested sample rate 14318181 not possible, using 14318170.000000, error is -11.000000

When the end of the file is reached you will see in CLI:

(RED) : Nothing more to read

Also, to enable USB zerocopy for better I/O stability and reduced CPU usage:

`echo 0 > /sys/module/usbcore/parameters/usbfs_memory_mb`

And reboot. This was added to the kernel [back in 2014](https://lkml.org/lkml/2014/7/2/377). The default buffer size is 16.

#### Based off the [osmo_fl2K project](https://osmocom.org/projects/osmo-fl2k/wiki) software.
