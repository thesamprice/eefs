# Host tests of the NAND backend (EEFS_BACKEND=nand, not cross compiling).
#
# eefs_nand_testlib is the backend built with the largest page and OOB used by the test geometries
# (EEFS_NAND_MAX_PAGE_SIZE=8192, EEFS_NAND_MAX_OOB_SIZE=448), so one binary serves every geometry
# and the -D override path of eefs_config.h is exercised.  eefs_nand_test_default is the same test
# program linked against the real eefs target (default maxima).  eefs_nand_test_verifyopen is the
# test program on a second copy of the test library built with EEFS_NAND_VERIFY_ON_OPEN=1, so the
# optional open-time data check keeps being built and tested although it is off by default.
#
#   eefs_nand_test <suite> <page> <oob> <ppb> <blocks> [case-substring]

set(EEFS_NAND_TEST_SOURCES
    nand_test_main.c
    test_layout.c
    test_sim.c
    test_journal.c
    test_micro.c
    test_fileops.c
    test_powercut.c
    test_ab.c
    nand_fixture.c
    nand_vol.c
    ${PROJECT_SOURCE_DIR}/libraries/microeefs_nand/microeefs_nand.c)

# Suites registered so far (later phases append rebuild health)
set(EEFS_NAND_SUITES layout sim journal micro fileops powercut ab)

# PageSize:OobSize:PagesPerBlock
set(EEFS_NAND_GEOMETRIES 4096:224:128 2048:64:64 8192:448:64)
set(EEFS_NAND_TEST_BLOCKS 4096)

# EEFS_NAND_LIB_SOURCES is relative to the project root
set(_eefs_nand_testlib_sources ${EEFS_NAND_LIB_SOURCES} drivers/standalone_driver/eefs_filesys.c)
list(TRANSFORM _eefs_nand_testlib_sources PREPEND ${PROJECT_SOURCE_DIR}/)
# The power-cut sweeps run thousands of mounts, writes and CRC checks: optimize the test builds when
# no build type was chosen (the eefs library itself keeps the project's flags).
if(NOT CMAKE_BUILD_TYPE)
    set(EEFS_NAND_TEST_OPT -O2)
else()
    set(EEFS_NAND_TEST_OPT "")
endif()

foreach(_variant IN ITEMS "" _verifyopen)
    add_library(eefs_nand_testlib${_variant} STATIC ${_eefs_nand_testlib_sources} nand_sim.c)
    target_link_libraries(eefs_nand_testlib${_variant} PUBLIC eefs_headers)
    target_compile_definitions(eefs_nand_testlib${_variant} PUBLIC
        EEFS_NAND_MAX_PAGE_SIZE=8192
        EEFS_NAND_MAX_OOB_SIZE=448)
    if(_variant STREQUAL "_verifyopen")
        target_compile_definitions(eefs_nand_testlib${_variant} PUBLIC EEFS_NAND_VERIFY_ON_OPEN=1)
    endif()
    target_include_directories(eefs_nand_testlib${_variant} PUBLIC
        ${EEFS_MACROS_DIR}
        ${PROJECT_SOURCE_DIR}/drivers/standalone_driver
        ${PROJECT_SOURCE_DIR}/libraries/eefs_nand
        ${PROJECT_SOURCE_DIR}/libraries/microeefs_nand
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(eefs_nand_testlib${_variant} PRIVATE ${EEFS_WARNING_FLAGS} ${EEFS_NAND_TEST_OPT})
    set_target_properties(eefs_nand_testlib${_variant} PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)

    add_executable(eefs_nand_test${_variant} ${EEFS_NAND_TEST_SOURCES})
    target_link_libraries(eefs_nand_test${_variant} PRIVATE eefs_nand_testlib${_variant})
    target_compile_options(eefs_nand_test${_variant} PRIVATE ${EEFS_WARNING_FLAGS} ${EEFS_NAND_TEST_OPT})
    set_target_properties(eefs_nand_test${_variant} PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)
endforeach()

# Same tests against the real eefs library (default EEFS_NAND_MAX_PAGE_SIZE / OOB), registered for
# the 4096/224/128 geometry (nand_default_config_4096).
add_executable(eefs_nand_test_default
    ${EEFS_NAND_TEST_SOURCES}
    nand_sim.c)
target_link_libraries(eefs_nand_test_default PRIVATE eefs)
target_include_directories(eefs_nand_test_default PRIVATE
    ${PROJECT_SOURCE_DIR}/drivers/standalone_driver
    ${PROJECT_SOURCE_DIR}/libraries/eefs_nand
    ${PROJECT_SOURCE_DIR}/libraries/microeefs_nand
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(eefs_nand_test_default PRIVATE ${EEFS_WARNING_FLAGS} ${EEFS_NAND_TEST_OPT})
set_target_properties(eefs_nand_test_default PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)

foreach(geom_text IN LISTS EEFS_NAND_GEOMETRIES)
    string(REPLACE ":" ";" geom ${geom_text})
    list(GET geom 0 page)
    list(GET geom 1 oob)
    list(GET geom 2 ppb)
    foreach(suite IN LISTS EEFS_NAND_SUITES)
        add_test(NAME nand_${suite}_${page}_${oob}_${ppb}
                 COMMAND eefs_nand_test ${suite} ${page} ${oob} ${ppb} ${EEFS_NAND_TEST_BLOCKS})
    endforeach()
endforeach()

add_test(NAME nand_default_config_4096 COMMAND eefs_nand_test_default fileops 4096 224 128 ${EEFS_NAND_TEST_BLOCKS})
add_test(NAME nand_verify_on_open_4096 COMMAND eefs_nand_test_verifyopen fileops 4096 224 128 ${EEFS_NAND_TEST_BLOCKS})
add_test(NAME nand_verify_on_open_ab_4096 COMMAND eefs_nand_test_verifyopen ab 4096 224 128 ${EEFS_NAND_TEST_BLOCKS})
