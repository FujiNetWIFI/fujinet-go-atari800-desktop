# Stage atari800's src/ into the build tree and apply tools/atari800/patches.
#
#   ATARI800_GEN    <build>/atari800-generated  patched; what the app links
#   ATARI800_STOCK  <build>/atari800-stock      unpatched; A/B tests only
#
# Re-stages when the pin, the checkout's dirty state, a patch or this file
# changes, or with -DATARI800_RESTAGE=ON.

option(ATARI800_RESTAGE "Re-stage the atari800 sources from the checkout" OFF)
option(A800_AB_TEST "Also stage unpatched atari800 for the A/B tests" ON)

a800_provide_dependency(
  NAME atari800
  PATH third_party/atari800
  URL "${ATARI800_URL}"
  COMMIT "${ATARI800_COMMIT}"
  SENTINEL src/atari.c
  OVERRIDE ATARI800_SRC
  RESULT ATARI800_DIR)

set(ATARI800_GEN "${CMAKE_BINARY_DIR}/atari800-generated")
set(ATARI800_STOCK "${CMAKE_BINARY_DIR}/atari800-stock")

file(GLOB ATARI800_PATCHES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/atari800/patches/*.patch")
list(SORT ATARI800_PATCHES)

set(_a8_head "")
if(GIT_EXECUTABLE AND EXISTS "${ATARI800_DIR}/.git")
  execute_process(COMMAND ${GIT_EXECUTABLE} -C "${ATARI800_DIR}" rev-parse HEAD
                  OUTPUT_VARIABLE _a8_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} -C "${ATARI800_DIR}" status --porcelain
                          --untracked-files=no -- src
                  OUTPUT_VARIABLE _a8_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_a8_dirty)
    string(SHA256 _a8_dirty_hash "${_a8_dirty}")
    set(_a8_head "${_a8_head}-dirty-${_a8_dirty_hash}")
  endif()
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _a8_stage_hash)
set(_a8_want "${ATARI800_DIR}\n${_a8_head}\n${_a8_stage_hash}\n")
foreach(_p IN LISTS ATARI800_PATCHES)
  file(SHA256 "${_p}" _h)
  string(APPEND _a8_want "${_p}=${_h}\n")
endforeach()

find_program(PATCH_EXECUTABLE patch)

function(_a8_copy_src dest)
  file(REMOVE_RECURSE "${dest}")
  file(MAKE_DIRECTORY "${dest}")
  # the checkout may hold an in-tree autotools build
  file(COPY "${ATARI800_DIR}/src" DESTINATION "${dest}"
       PATTERN ".deps" EXCLUDE
       PATTERN ".dirstamp" EXCLUDE
       PATTERN "*.o" EXCLUDE
       PATTERN "*.a" EXCLUDE
       PATTERN "*.png" EXCLUDE
       PATTERN "config.h" EXCLUDE
       PATTERN "Makefile" EXCLUDE
       PATTERN "Makefile.in" EXCLUDE
       PATTERN "atari800" EXCLUDE
       PATTERN "atari800.exe" EXCLUDE
       PATTERN "libatari800" EXCLUDE
       PATTERN "android" EXCLUDE
       PATTERN "sdl" EXCLUDE)
endfunction()

function(_a8_stage dest want apply)
  set(_have "")
  if(EXISTS "${dest}/.source-info")
    file(READ "${dest}/.source-info" _have)
  endif()
  if(NOT ATARI800_RESTAGE AND _have STREQUAL want AND EXISTS "${dest}/src/atari.c")
    return()
  endif()
  message(STATUS "atari800: staging ${ATARI800_DIR}/src -> ${dest}")
  _a8_copy_src("${dest}")
  if(apply)
    foreach(_p IN LISTS ATARI800_PATCHES)
      get_filename_component(_n "${_p}" NAME)
      if(PATCH_EXECUTABLE)
        execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 --forward --batch
                                --no-backup-if-mismatch -i "${_p}"
                        WORKING_DIRECTORY "${dest}"
                        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
      else()
        execute_process(COMMAND ${GIT_EXECUTABLE} apply -p1 "${_p}"
                        WORKING_DIRECTORY "${dest}"
                        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
      endif()
      if(NOT _rc EQUAL 0)
        file(REMOVE "${dest}/.source-info")
        message(FATAL_ERROR "atari800: ${_n} does not apply to ${ATARI800_DIR}:\n${_out}")
      endif()
    endforeach()
  endif()
  file(WRITE "${dest}/.source-info" "${want}")
endfunction()

_a8_stage("${ATARI800_GEN}" "${_a8_want}" TRUE)
if(A800_AB_TEST)
  _a8_stage("${ATARI800_STOCK}" "${_a8_want}stock\n" FALSE)
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${ATARI800_GEN}/.source-info" ${ATARI800_PATCHES})
