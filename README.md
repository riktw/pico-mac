# Pico Micro Mac (pico-umac)

I too have run roughshod across the code!

This now builds and works on the Bells&Whistles board without any external electronics needed. 

Configuration is purely done by changing the CmakeLists.txt, this so it can be build using the vscode pi pico plugin. USB is again done over the on board USB port, not via PIO. 

PSRAM works, and when PSRAM is detected, it's used. A slight overclock is done on the RP2350 so the PSRAM can run in spec on half the CPU speed. 

SD card support is the default and an image with the correct name (umac0ro.img or umac0w.img) should be on a FAT32 formatted SD card.


v0.21-fruitjam 28 March 2025

I (@jepler) have run roughshod across the code, breaking things willy-nilly and adding

 * 512x342 & 640x480 digital output on HSTX
 * PIO USB
 * PSRAM support
 * Some Sound support on the onboard I2S DAC (speaker and headphones)

Several pre-compiled variants are offered:
 * 400kB or 4096kB (the latter uses PSRAM, and may perform slower overall but can run more software)
 * 512x342 or 640x480 desktop resolution (512x342 is more compatible but has black screen margins)
 * overclocked or not (overclocked may run faster but may be less reliable)

What works?
 * System beep
 * Dark Castle including audio
 * After Dark screensavers including audio
 * Glider works, but without sound

What doesn't work?
 * Hypercard "play" and some hypercard screen transitions

Some of the software I tested with:
 * https://archive.org/details/HyperCardBootSystem7
 * https://archive.org/details/mac\_DarkCastle\_1\_2
 * https://archive.org/details/AfterDark2

Plug mouse & keyboard into the USB ports of the fruit jam.

Put the software (a mac HFS volume with no additional headers or metadata) on a
SD card as "umac0w.img" (if you want to be able to write files) or
"umac0ro.img" (if you want the drive to be read only) and press the reset
button to start.


**Important note on overclocking:** The "oc" uf2 files overclock your RP2 chip to 264MHz. Simply including the `<Adafruit_dvhstx.h>` header enables this overclocking, separate from the option in the Arduino Tools menu.
Just like PC overclocking, there’s some risk of reduced component lifespan, though the extent (if any) can’t be precisely quantified and could vary from one chip to another.
Proceed at your own discretion.

v0.21 20 December 2024


This project embeds the [umac Mac 128K
emulator](https://github.com/evansm7/umac) project into a Raspberry Pi
Pico microcontroller.  At long last, the worst Macintosh in a cheap,
portable form factor!

It has features, many features, the best features:

   * Outputs VGA 640x480@60Hz, monochrome, using three resistors
   * USB HID keyboard and mouse
   * Read-only disc image in flash (your creations are ephemeral, like life itself)
   * Or, if you have a hard time letting go, support for rewritable
     disc storage on an SPI-attached SD card
   * Mac 128K by default, or you can make use of more of the Pico's
     memory and run as a _Mac 208K_
   * Since you now have more memory, you can splash out on more
     screen real-estate, and use 640x480 resolution!

Great features.  It even doesn't hang at random!  (Anymore.)

The _Mac 208K_ was, of course, never a real machine.  But, _umac_
supports odd-sized memories, and more memory runs more things.  A
surprising amount of software runs on the 128K config, but if you need
to run _MacPaint_ specifically then you'll need to build both SD
storage in addition to the _Mac 208K_ config.

So anyway, you can build this project yourself for less than the cost
of a beer!  You'll need at least a RPi Pico board, a VGA monitor (or
VGA-HDMI adapter), a USB mouse (and maybe a USB keyboard/hub), plus a
couple of cheap components.

# Build

## Prerequisites/essentials

   * git submodules
      - Clone the repo with `--recursive`, or `git submodule update --init --recursive`
   * Install/set up the [Pico/RP2040 SDK](https://github.com/raspberrypi/pico-sdk)
   * Get a ROM & disc image with `sh fetch-rom-dsk.sh` (needs curl & 7z (debian package p7zip-full))

## Build pico-umac

Run the configure-and-build script:
```
$ ./fruitjam-build.sh -h
Usage: ./fruitjam-build.sh [-v] [-m KiB] [-d diskimage]

   -v: Use framebuffer resolution 640x480 instead of 512x342
   -m: Set memory size in KiB (over 400kB requires psram)
   -d: Specify disc image to include
   -o: Overclock to 264MHz (known to be incompatible with psram)

PSRAM is automatically set depending on memory & framebuffer details
```

## Disc image

If you don't build SD support, an internal read-only disc image is
stored in flash.  If you do build SD support, you have the option to
still include an image in flash, and this is used as a fallback if
SD boot fails.

Grab a Macintosh system disc from somewhere.  A 400K or 800K floppy
image works just fine, up to System 3.2 (the last version to support
Mac128Ks).  I've used images from
<https://winworldpc.com/product/mac-os-0-6/system-3x> but also check
the various forums and MacintoshRepository.  See the `umac` README for
info on formats (it needs to be raw data without header).

The image size can be whatever you have space for in flash (typically
about 1.3MB is free there), or on the SD card.  (I don't know what the
HFS limits are.  But if you make a 50MB disc you're unlikely to fill
it with software that actually works on the _Mac 128K_ :) )

If using an SD card, use a FAT-formatted card and copy your disc image
into _one_ of the following files in the root of the card:

   * `umac0.img`:  A normal read/write disc image
   * `umac0ro.img`:  A read-only disc image
# Software

Both CPU cores are used, and are optionally overclocked (blush) to 264MHz so that
Missile Command is enjoyable to play.

The `umac` emulator and video output runs on core 1, and core 0 deals
with USB HID input.  Video DMA is initialised pointing to the
framebuffer in the Mac's RAM, or to a mirrored region in SRAM depending
on the configuration.

Other than that, it's just a main loop in `main.c` shuffling things
into `umac`.

Quite a lot of optimisation has been done in `umac` and `Musashi` to
get performance up on Cortex-M0+ and the RP2040, like careful location
of certain routines in RAM, ensuring inlining/constants can be
foldeed, etc.  It's 5x faster than it was at the beginning.

The top-level project might be a useful framework for other emulators,
or other projects that need USB HID input and a framebuffer (e.g. a
VT220 emulator!).

The USB HID code is largely stolen from the TinyUSB example, but shows
how in practice you might capture keypresses/deal with mouse events.

# Licence

`hid.c` and `tusb_config.h` are based on code from the TinyUSB
project, which is Copyright (c) 2019, 2021 Ha Thach (tinyusb.org) and
released under the MIT licence.  `sd_hw_config.c` is based on code
from the no-OS-FatFS-SD-SPI-RPi-Pico project, which is Copyright (c)
2021 Carl John Kugler III.

The remainder of the code is released under the MIT licence:

 Copyright (c) 2024 Matt Evans:

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.

