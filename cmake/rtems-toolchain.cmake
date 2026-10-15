# CMake toolchain file for RTEMS 6 / 7 BSPs.
#
#   cmake -B build-rtems -DCMAKE_TOOLCHAIN_FILE=cmake/rtems-toolchain.cmake \
#         -DRTEMS_PREFIX=$HOME/rtems/7 -DRTEMS_VERSION=7 \
#         -DRTEMS_ARCH=arm -DRTEMS_BSP=xilinx_zynq_a9_qemu
#
# The compiler flags come from the BSP's pkg-config file,
# <prefix>/lib/pkgconfig/<arch>-rtems<version>-<bsp>.pc, which RTEMS 6+ installs for every BSP.
# The file is parsed directly, so pkg-config itself is not needed.

set(RTEMS_VERSION "7" CACHE STRING "RTEMS major version (6 or 7)")
set(RTEMS_PREFIX  "$ENV{RTEMS_PREFIX}" CACHE PATH "RTEMS tools and BSP install prefix")
set(RTEMS_ARCH    "$ENV{RTEMS_ARCH}" CACHE STRING "RTEMS architecture, e.g. arm, riscv, sparc")
set(RTEMS_BSP     "$ENV{RTEMS_BSP}" CACHE STRING "RTEMS BSP, e.g. xilinx_zynq_a9_qemu")

foreach(var RTEMS_PREFIX RTEMS_ARCH RTEMS_BSP)
    if(NOT ${var})
        message(FATAL_ERROR "rtems-toolchain.cmake: set ${var} (as -D${var}=... or in the environment)")
    endif()
endforeach()

# try_compile() reloads this file in a fresh project, so pass the settings through.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RTEMS_VERSION RTEMS_PREFIX RTEMS_ARCH RTEMS_BSP)

set(_rtems_target "${RTEMS_ARCH}-rtems${RTEMS_VERSION}")
set(_rtems_pc "${RTEMS_PREFIX}/lib/pkgconfig/${_rtems_target}-${RTEMS_BSP}.pc")
if(NOT EXISTS "${_rtems_pc}")
    message(FATAL_ERROR "rtems-toolchain.cmake: BSP not installed: ${_rtems_pc} not found")
endif()

file(STRINGS "${_rtems_pc}" _rtems_abi_line REGEX "^ABI_FLAGS=")
string(REGEX REPLACE "^ABI_FLAGS=" "" RTEMS_ABI_FLAGS "${_rtems_abi_line}")
set(RTEMS_BSP_LIBDIR  "${RTEMS_PREFIX}/${_rtems_target}/${RTEMS_BSP}/lib")
set(RTEMS_BSP_INCLUDE "${RTEMS_BSP_LIBDIR}/include")

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR ${RTEMS_ARCH})

set(CMAKE_C_COMPILER   "${RTEMS_PREFIX}/bin/${_rtems_target}-gcc")
set(CMAKE_CXX_COMPILER "${RTEMS_PREFIX}/bin/${_rtems_target}-g++")
set(CMAKE_AR           "${RTEMS_PREFIX}/bin/${_rtems_target}-ar"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${RTEMS_PREFIX}/bin/${_rtems_target}-ranlib" CACHE FILEPATH "")

set(CMAKE_C_FLAGS_INIT          "${RTEMS_ABI_FLAGS} -isystem ${RTEMS_BSP_INCLUDE}")
set(CMAKE_CXX_FLAGS_INIT        "${RTEMS_ABI_FLAGS} -isystem ${RTEMS_BSP_INCLUDE}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${RTEMS_ABI_FLAGS} -B${RTEMS_BSP_LIBDIR} -qrtems -Wl,--gc-sections")

# An RTEMS executable needs a configured application, so probe the compiler with a library.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${RTEMS_PREFIX}/${_rtems_target}/${RTEMS_BSP}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
