# cmake -DA=<exe> -DB=<exe> -DARGS=<a|b|c> -P ab_compare.cmake
string(REPLACE "|" ";" ARGS "${ARGS}")
execute_process(COMMAND "${A}" ${ARGS} OUTPUT_VARIABLE _a RESULT_VARIABLE _ra
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
execute_process(COMMAND "${B}" ${ARGS} OUTPUT_VARIABLE _b RESULT_VARIABLE _rb
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
if(NOT _ra EQUAL 0 OR NOT _rb EQUAL 0)
  message(FATAL_ERROR "runs failed: ${_ra} ${_rb}")
endif()
message(STATUS "patched: ${_a}stock:   ${_b}")
if(NOT _a STREQUAL _b)
  message(FATAL_ERROR "A/B mismatch")
endif()
