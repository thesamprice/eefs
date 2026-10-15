# Runs one EEFS tool test.  Invoked by CTest as:
#   cmake -DEEFS_TEST=<name> -DGENEEPROMFS=... -DEEFSTOOL=... -DDATA_DIR=... -DGOLDEN_DIR=...
#         -DWORK_DIR=... -P run_test.cmake

set(TIMESTAMP 1700000000)
set(EEPROM_SIZE 65536)

# geneepromfs reads the input files relative to the current directory, and eefstool-img
# rewrites the image it opens, so every test works on a fresh copy of the inputs.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${DATA_DIR}/" DESTINATION "${WORK_DIR}")

function(run)
    execute_process(COMMAND ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Command failed (${result}): ${ARGN}\n${output}")
    endif()
endfunction()

function(expect_same actual expected)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${actual}" "${expected}"
        RESULT_VARIABLE different)
    if(different)
        message(FATAL_ERROR "${actual} differs from ${expected}")
    endif()
endfunction()

if(EEFS_TEST STREQUAL "geneepromfs_big")
    run(${GENEEPROMFS} -t ${TIMESTAMP} -s ${EEPROM_SIZE} files.dat big.img)
    expect_same("${WORK_DIR}/big.img" "${GOLDEN_DIR}/big.img")

elseif(EEFS_TEST STREQUAL "geneepromfs_little")
    run(${GENEEPROMFS} -e little -t ${TIMESTAMP} -s ${EEPROM_SIZE} files.dat little.img)
    expect_same("${WORK_DIR}/little.img" "${GOLDEN_DIR}/little.img")

elseif(EEFS_TEST STREQUAL "geneepromfs_map")
    run(${GENEEPROMFS} -t ${TIMESTAMP} -s ${EEPROM_SIZE} -m big.map files.dat big.img)
    expect_same("${WORK_DIR}/big.map" "${GOLDEN_DIR}/big.map")

elseif(EEFS_TEST STREQUAL "eefstool_read")
    # Read every file back out of the golden image and check it matches the original input.
    file(COPY "${GOLDEN_DIR}/big.img" DESTINATION "${WORK_DIR}")
    foreach(name hello.txt pattern.bin)
        run(${EEFSTOOL} --img_file=big.img --copy_from ${name} ${name}.out)
        expect_same("${WORK_DIR}/${name}.out" "${DATA_DIR}/${name}")
    endforeach()

else()
    message(FATAL_ERROR "Unknown test '${EEFS_TEST}'")
endif()
