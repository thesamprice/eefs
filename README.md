eefs
====

EEPROM File System

This is the EEPROM File System Project (EEFS). It is a simple file system for memory devices such as EEPROM, RAM, ROM, etc. Currently it is not intended for block oriented devices such as disks and flash devices.

It can be used as a simple file system to boot an embedded system running vxWorks, RTEMS, or even no operating system. An EEFS image can be created on the development host, providing a single file to burn into an image that is loaded on a target. The file system is easy to understand, debug, and dump. 

There are drivers for RTEMS, vxWorks, and there is a standalone API for systems that do not have a file system. 

There is even a "microeefs" interface that allows the lookup of a file from a single function. This allows the bootloader to locate an image in EEPROM by the file name with a minimal amount of code. 
Future releases will include the ability to allow multiple EEFS volumes ( volumes in RAM and EEPROM at the same time ) 



Building
--------

EEFS builds with CMake 3.21 or newer.

Host build (the `eefs` and `microeefs` libraries, plus the `geneepromfs` and `eefstool-img` tools):

    cmake -B build
    cmake --build build
    ctest --test-dir build

Cross build for an RTEMS 6 or 7 BSP (libraries only, plus an RTEMS test app that runs under QEMU for the Zynq BSP):

    cmake -B build-rtems -DCMAKE_TOOLCHAIN_FILE=cmake/rtems-toolchain.cmake \
          -DRTEMS_PREFIX=$HOME/rtems/7 -DRTEMS_VERSION=7 \
          -DRTEMS_ARCH=arm -DRTEMS_BSP=xilinx_zynq_a9_qemu
    cmake --build build-rtems
    ctest --test-dir build-rtems

Useful options:

* `EEFS_DRIVER` selects the OS binding: `standalone` (default) or `vxworks`. The `rtems` driver in `drivers/rtems_driver` targets the RTEMS 4.10 filesystem API and does not build on RTEMS 4.11 or later, so use `standalone` on RTEMS 6/7.
* `EEFS_MACROS_DIR` points at the directory holding the `eefs_macros.h` porting layer (EEPROM read/write, locking, time), so you can port to new hardware without editing the tree.
* `EEFS_BUILD_TOOLS` builds the host tools. It defaults to ON for native builds and OFF for cross builds.
* `EEFS_BUILD_BDM_TOOL` builds `eefstool-bdm`, which needs the RTEMS 4.10 BDM headers and libraries under `EEFS_BDM_ROOT`.

Installing exports CMake targets, so other projects can use `find_package(eefs)` and link `eefs::eefs` or `eefs::microeefs`.
