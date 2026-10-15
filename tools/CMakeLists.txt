#
# geneepromfs: builds an EEFS image from an input file list
#

add_executable(geneepromfs
    geneepromfs/cmdlineopt.c
    geneepromfs/parser.c
    geneepromfs/geneepromfs.c)
# geneepromfs/ comes first so its own eefs_config.h is used, as in the original Makefile.
target_include_directories(geneepromfs BEFORE PRIVATE geneepromfs)
target_link_libraries(geneepromfs PRIVATE eefs_headers)
target_compile_options(geneepromfs PRIVATE ${EEFS_WARNING_FLAGS})
if(EEFS_WERROR AND EEFS_WARNING_FLAGS)
    target_compile_options(geneepromfs PRIVATE -Werror)
endif()
set_target_properties(geneepromfs PROPERTIES C_STANDARD 90 C_EXTENSIONS OFF)
find_library(EEFS_LIBM m)
if(EEFS_LIBM)
    target_link_libraries(geneepromfs PRIVATE ${EEFS_LIBM})
endif()

#
# eefstool: inspects and edits EEFS images (eefstool-img) or live EEPROM over BDM (eefstool-bdm).
# It compiles its own byte-swapping copy of eefs_fileapi.c and the standalone driver against
# its own eefs_macros.h, so it does not link the eefs library.
#

set(EEFSTOOL_COMMON_SOURCES
    eefstool/src/eefstool_main.c
    eefstool/src/eefs_fileapi.c
    eefstool/src/eefs_swap.c
    ${PROJECT_SOURCE_DIR}/drivers/standalone_driver/eefs_filesys.c)

function(eefs_add_eefstool name)
    add_executable(${name} ${EEFSTOOL_COMMON_SOURCES} ${ARGN})
    # eefstool/inc comes first so its eefs_macros.h wins over the standalone driver's.
    target_include_directories(${name} BEFORE PRIVATE
        eefstool/inc
        ${PROJECT_SOURCE_DIR}/inc
        ${PROJECT_SOURCE_DIR}/drivers/standalone_driver)
    target_link_libraries(${name} PRIVATE eefs_headers)
    if(EEFS_WARNING_FLAGS)
        target_compile_options(${name} PRIVATE -Wall)
    endif()
endfunction()

eefs_add_eefstool(eefstool-img
    eefstool/src/eefstool_driver_img.c
    eefstool/src/eefstool_opts_img.c)

if(EEFS_BUILD_BDM_TOOL)
    set(EEFS_BDM_ROOT "/opt/rtems-4.10" CACHE PATH "Prefix holding the BDM headers (include/) and libBDM (lib/)")
    find_library(EEFS_BDM_LIBRARY BDM PATHS ${EEFS_BDM_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
    find_library(EEFS_TBLCF_LIBRARY tblcf PATHS ${EEFS_BDM_ROOT}/lib REQUIRED)
    find_library(EEFS_USB_LIBRARY usb REQUIRED)

    eefs_add_eefstool(eefstool-bdm
        eefstool/src/eefstool_driver_bdm.c
        eefstool/src/eefstool_opts_bdm.c
        eefstool/src/eeprom.c)
    target_include_directories(eefstool-bdm PRIVATE ${EEFS_BDM_ROOT}/include)
    target_link_libraries(eefstool-bdm PRIVATE
        ${EEFS_BDM_LIBRARY} ${EEFS_TBLCF_LIBRARY} ${EEFS_USB_LIBRARY})
endif()

install(TARGETS geneepromfs eefstool-img RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(EEFS_BUILD_BDM_TOOL)
    install(TARGETS eefstool-bdm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
