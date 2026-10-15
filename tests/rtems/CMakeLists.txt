# RTEMS target test.  EEPROM backend: an RTEMS application that mounts the little-endian golden
# image and exercises the eefs and microeefs libraries.  NAND backend: eefs_rtems_nand_test.c on
# the RAM NAND simulator.  Built when configuring with cmake/rtems-toolchain.cmake, and run under
# QEMU when EEFS_RTEMS_QEMU_COMMAND is set.

# QEMU command line for the BSP, up to (not including) the kernel image.  Preset for the
# Zynq QEMU BSP, so that one runs out of the box.
if(RTEMS_ARCH STREQUAL "arm" AND RTEMS_BSP STREQUAL "xilinx_zynq_a9_qemu")
    set(_qemu_default "qemu-system-arm;-M;xilinx-zynq-a9;-m;256M;-nographic;-no-reboot;-serial;null;-serial;mon:stdio;-kernel")
else()
    set(_qemu_default "")
endif()
set(EEFS_RTEMS_QEMU_COMMAND "${_qemu_default}" CACHE STRING
    "QEMU command (a ;-list ending just before the kernel path) to run the RTEMS test, empty to skip")

if(EEFS_BACKEND STREQUAL "nand")
    # NAND backend: the RAM NAND simulator of the host tests is the device.  The test runs the health check in a
    # low-priority task while a higher-priority task writes, so it needs the library lock.  It always uses the
    # EEFS-provided RTEMS lock (eefs_rtems_lock.c): with EEFS_LOCK=rtems it links the eefs target itself, which is
    # what that option ships; otherwise it links a private copy of the library built the way EEFS_LOCK=rtems builds
    # it, so that the eefs target keeps the configured lock (the empty default, or the application's hooks).
    if(EEFS_LOCK STREQUAL "rtems")
        set(_eefs_rtems_test_lib eefs)
    else()
        set(_eefs_rtems_locked_sources
            ${EEFS_NAND_LIB_SOURCES}
            drivers/standalone_driver/eefs_filesys.c
            ${EEFS_RTEMS_LOCK_SOURCES})
        list(TRANSFORM _eefs_rtems_locked_sources PREPEND ${PROJECT_SOURCE_DIR}/)
        add_library(eefs_rtems_locked STATIC ${_eefs_rtems_locked_sources})
        target_link_libraries(eefs_rtems_locked PUBLIC eefs_headers)
        target_compile_definitions(eefs_rtems_locked PUBLIC EEFS_LIB_LOCK_HOOKS EEFS_LIB_LOCK_RTEMS)
        target_include_directories(eefs_rtems_locked PUBLIC
            ${EEFS_MACROS_DIR}
            ${PROJECT_SOURCE_DIR}/drivers/standalone_driver)
        target_compile_options(eefs_rtems_locked PRIVATE ${EEFS_WARNING_FLAGS})
        set_target_properties(eefs_rtems_locked PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)
        set(_eefs_rtems_test_lib eefs_rtems_locked)
    endif()

    add_executable(eefs_rtems_test.exe
        eefs_rtems_nand_test.c
        ${PROJECT_SOURCE_DIR}/tests/nand/nand_sim.c)
    target_include_directories(eefs_rtems_test.exe PRIVATE ${PROJECT_SOURCE_DIR}/tests/nand)
    target_link_libraries(eefs_rtems_test.exe PRIVATE ${_eefs_rtems_test_lib} microeefs)
    target_compile_options(eefs_rtems_test.exe PRIVATE ${EEFS_WARNING_FLAGS})
    set_target_properties(eefs_rtems_test.exe PROPERTIES C_STANDARD 11)
    # The test needs several MB of RAM (the simulated NAND and the images), more than small BSPs have (e.g.
    # esp32c3db), so it is built by default only when QEMU runs it; "--target eefs_rtems_test.exe" builds it anyway.
    if(NOT EEFS_RTEMS_QEMU_COMMAND)
        set_target_properties(eefs_rtems_test.exe PROPERTIES EXCLUDE_FROM_ALL TRUE)
        if(TARGET eefs_rtems_locked)
            set_target_properties(eefs_rtems_locked PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endif()
else()
    # Embed the golden image as a C array
    set(_image ${PROJECT_SOURCE_DIR}/tests/golden/little.img)
    file(READ ${_image} _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    file(SIZE ${_image} _size)
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/eefs_test_image.c CONTENT
"/* Generated from tests/golden/little.img */
const unsigned char eefs_test_image[] = { ${_bytes} };
const unsigned int  eefs_test_image_size = ${_size};
")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_image})

    add_executable(eefs_rtems_test.exe
        eefs_rtems_test.c
        ${CMAKE_CURRENT_BINARY_DIR}/eefs_test_image.c)
    target_link_libraries(eefs_rtems_test.exe PRIVATE eefs microeefs)
    target_compile_options(eefs_rtems_test.exe PRIVATE ${EEFS_WARNING_FLAGS})
    set_target_properties(eefs_rtems_test.exe PROPERTIES C_STANDARD 11)
endif()

if(EEFS_RTEMS_QEMU_COMMAND)
    add_test(NAME eefs_rtems_qemu
        COMMAND ${EEFS_RTEMS_QEMU_COMMAND} $<TARGET_FILE:eefs_rtems_test.exe>)
    set_tests_properties(eefs_rtems_qemu PROPERTIES
        TIMEOUT 60
        PASS_REGULAR_EXPRESSION "EEFS RTEMS TEST PASSED"
        FAIL_REGULAR_EXPRESSION "FAIL")
endif()
