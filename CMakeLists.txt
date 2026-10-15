cmake_minimum_required(VERSION 3.21)

project(eefs
    VERSION 2.0.0
    DESCRIPTION "EEPROM File System"
    LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

#
# Options
#

# The tools run on the development host, so they are off by default when cross compiling.
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
    set(_eefs_default_tools ON)
else()
    set(_eefs_default_tools OFF)
endif()
option(EEFS_BUILD_TOOLS     "Build the host tools (geneepromfs, eefstool-img)" ${_eefs_default_tools})
option(EEFS_BUILD_BDM_TOOL  "Build eefstool-bdm (needs the RTEMS 4.10 BDM headers and libraries)" OFF)
option(EEFS_WERROR          "Treat warnings as errors where the original Makefiles did (geneepromfs)" ON)

# Optional processor define for common_types.h (_ix86_, __PPC__, _m68k_ or __SPARC__).  Leave it
# empty for everything else (ARM, AArch64, RISC-V, MicroBlaze, host builds): common_types.h then
# takes the endianness from the compiler.
set(EEFS_ARCH_DEFINE "" CACHE STRING "Optional processor define for common_types.h")

# Which OS binding to build the eefs library with.  Each driver directory provides its own
# eefs_macros.h (the EEPROM access / lock / time porting layer).
set(EEFS_DRIVER "standalone" CACHE STRING "EEFS driver: standalone, vxworks or rtems")
set_property(CACHE EEFS_DRIVER PROPERTY STRINGS standalone vxworks rtems)

# Directory holding the eefs_macros.h used by the eefs and microeefs libraries.  Point this at
# your own directory to port EEFS to new hardware without editing the tree.
if(EEFS_DRIVER STREQUAL "vxworks")
    set(_eefs_default_macros_dir "${PROJECT_SOURCE_DIR}/drivers/vxworks_driver")
else()
    set(_eefs_default_macros_dir "${PROJECT_SOURCE_DIR}/drivers/standalone_driver")
endif()
set(EEFS_MACROS_DIR "${_eefs_default_macros_dir}" CACHE PATH "Directory containing eefs_macros.h")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(EEFS_WARNING_FLAGS -Wall -W -Wstrict-prototypes -Wundef)
endif()

#
# eefs_headers: public headers and the processor define, shared by everything
#

add_library(eefs_headers INTERFACE)
add_library(eefs::headers ALIAS eefs_headers)
target_include_directories(eefs_headers INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/eefs>)
if(EEFS_ARCH_DEFINE)
    target_compile_definitions(eefs_headers INTERFACE ${EEFS_ARCH_DEFINE})
endif()

#
# eefs: the core file system library plus the selected OS driver
#

set(EEFS_DRIVER_SOURCES_standalone drivers/standalone_driver/eefs_filesys.c)
set(EEFS_DRIVER_SOURCES_vxworks    drivers/vxworks_driver/eefs_vxworks.c)
set(EEFS_DRIVER_SOURCES_rtems      drivers/rtems_driver/eefsDriver.c drivers/rtems_driver/init_eefs.c)
if(EEFS_DRIVER STREQUAL "rtems")
    # drivers/rtems_driver implements the RTEMS 4.10 filesystem API (evalpath_h, evalformake_h,
    # node_type_h).  RTEMS 4.11 replaced it, so the driver does not build on RTEMS 5, 6 or 7.
    # On those versions use the standalone driver, or port rtems_driver to the current API.
    message(FATAL_ERROR "EEFS_DRIVER=rtems needs RTEMS 4.10; the driver has not been ported to "
        "the RTEMS 4.11+ filesystem API. Use EEFS_DRIVER=standalone on RTEMS 6/7.")
endif()
if(NOT DEFINED EEFS_DRIVER_SOURCES_${EEFS_DRIVER})
    message(FATAL_ERROR "Unknown EEFS_DRIVER '${EEFS_DRIVER}' (expected standalone, vxworks or rtems)")
endif()

add_library(eefs STATIC
    libraries/eepromfs/eefs_fileapi.c
    ${EEFS_DRIVER_SOURCES_${EEFS_DRIVER}})
add_library(eefs::eefs ALIAS eefs)
target_link_libraries(eefs PUBLIC eefs_headers)
target_include_directories(eefs PUBLIC
    $<BUILD_INTERFACE:${EEFS_MACROS_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/drivers/${EEFS_DRIVER}_driver>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/eefs>)
target_compile_options(eefs PRIVATE ${EEFS_WARNING_FLAGS})
set_target_properties(eefs PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)

#
# microeefs: single-function file lookup for bootloaders
#

add_library(microeefs STATIC libraries/microeefs/microeefs.c)
add_library(eefs::microeefs ALIAS microeefs)
target_link_libraries(microeefs PUBLIC eefs_headers)
target_include_directories(microeefs
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/libraries/microeefs>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/eefs>
    PRIVATE
        ${EEFS_MACROS_DIR})
target_compile_options(microeefs PRIVATE ${EEFS_WARNING_FLAGS})
set_target_properties(microeefs PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)

#
# Host tools
#

if(EEFS_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

#
# Tests
#

include(CTest)
if(BUILD_TESTING AND EEFS_BUILD_TOOLS)
    add_subdirectory(tests)
endif()
# RTEMS_BSP is set by cmake/rtems-toolchain.cmake
if(BUILD_TESTING AND RTEMS_BSP)
    add_subdirectory(tests/rtems)
endif()

#
# Install and package config, so other projects can find_package(eefs)
#

install(TARGETS eefs_headers eefs microeefs
    EXPORT eefsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES
        inc/common_types.h
        inc/eefs_config.h
        inc/eefs_fileapi.h
        inc/eefs_version.h
        ${EEFS_MACROS_DIR}/eefs_macros.h
        libraries/microeefs/microeefs.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eefs)
if(EEFS_DRIVER STREQUAL "standalone")
    install(FILES drivers/standalone_driver/eefs_filesys.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eefs)
elseif(EEFS_DRIVER STREQUAL "vxworks")
    install(FILES drivers/vxworks_driver/eefs_vxworks.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eefs)
endif()

install(EXPORT eefsTargets
    NAMESPACE eefs::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/eefs)
configure_package_config_file(cmake/eefsConfig.cmake.in
    ${PROJECT_BINARY_DIR}/eefsConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/eefs)
write_basic_package_version_file(${PROJECT_BINARY_DIR}/eefsConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
        ${PROJECT_BINARY_DIR}/eefsConfig.cmake
        ${PROJECT_BINARY_DIR}/eefsConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/eefs)
