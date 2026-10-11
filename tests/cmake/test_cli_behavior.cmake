# Help/version must exit before configuration/database setup. Normalize the
# release number so this formatting contract survives ordinary version bumps.
file(READ "${BASELINE_FILE}" baseline)
foreach(option IN ITEMS --help --version)
    string(JSON expected_exit GET "${baseline}" "${option}" exit)
    string(JSON expected_stdout GET "${baseline}" "${option}" stdout)
    string(JSON expected_stderr GET "${baseline}" "${option}" stderr)
    execute_process(COMMAND "${SPINE_EXECUTABLE}" "${option}"
        RESULT_VARIABLE actual_exit OUTPUT_VARIABLE actual_stdout
        ERROR_VARIABLE actual_stderr TIMEOUT 10)
    string(REGEX REPLACE "^SPINE [^ ]+" "SPINE <version>" expected_stdout "${expected_stdout}")
    string(REGEX REPLACE "^SPINE [^ ]+" "SPINE <version>" actual_stdout "${actual_stdout}")
    if(NOT "${actual_exit}" STREQUAL "${expected_exit}" OR
       NOT "${actual_stdout}" STREQUAL "${expected_stdout}" OR
       NOT "${actual_stderr}" STREQUAL "${expected_stderr}")
        message(FATAL_ERROR "${option} differs from its captured output/exit contract")
    endif()
endforeach()
