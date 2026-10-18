# External source dependencies, provided by the build itself (no submodules).
#
#   atari800          upstream atari800/atari800, staged and patched by
#                     cmake/StageAtari800.cmake.
#   fujinet-firmware  FujiNetWIFI master; built as the in-process runtime by
#                     cmake/FujiNetRuntime.cmake. Local builds move this pin
#                     forward automatically (cmake/FujiNetUpdate.cmake).
#
# Each pin is recorded in three places: here and in both flatpak manifests
# (build-aux/flatpak/*.yml). tools/fujinet/update-fujinet-firmware.sh keeps
# the FujiNet ones in step.
#
# Resolution order: ATARI800_SRC / FUJINET_SRC (cache or environment) -> the
# gitignored clone in third_party/<name> -> a fresh clone of the pin.

find_package(Git QUIET)

set(ATARI800_COMMIT "b49865e63f5675c798d0122a5ff65c89b2f2748b")
set(ATARI800_URL "https://github.com/atari800/atari800")

set(FUJINET_COMMIT "5674a013514e6370c0100f5f6295aa2069218ff3")
set(FUJINET_URL "https://github.com/FujiNetWIFI/fujinet-firmware")

# a800_provide_dependency(NAME <n> PATH <p> SENTINEL <file> OVERRIDE <VAR>
#                         RESULT <out> [URL <u> COMMIT <c>]
#                         [ARCHIVE <url> SHA256 <h> STRIP <dir>])
function(a800_provide_dependency)
  cmake_parse_arguments(DEP ""
    "NAME;PATH;URL;COMMIT;ARCHIVE;SHA256;STRIP;SENTINEL;OVERRIDE;RESULT" ""
    ${ARGN})

  set(_override "")
  if(DEFINED ${DEP_OVERRIDE})
    set(_override "${${DEP_OVERRIDE}}")
  elseif(DEFINED ENV{${DEP_OVERRIDE}})
    set(_override "$ENV{${DEP_OVERRIDE}}")
  endif()
  if(_override)
    if(NOT EXISTS "${_override}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_OVERRIDE}=${_override} does not look like a ${DEP_NAME} "
        "checkout (no ${DEP_SENTINEL}).")
    endif()
    message(STATUS "${DEP_NAME}: using ${_override} (${DEP_OVERRIDE})")
    set(${DEP_RESULT} "${_override}" PARENT_SCOPE)
    return()
  endif()

  set(_path "${CMAKE_SOURCE_DIR}/${DEP_PATH}")

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}" AND DEP_ARCHIVE)
    set(_dl "${CMAKE_BINARY_DIR}/_deps/${DEP_NAME}.archive")
    get_filename_component(_parent "${_path}" DIRECTORY)
    set(_tmp "${_parent}/.${DEP_NAME}-extract")

    if(NOT EXISTS "${_dl}")
      message(STATUS "${DEP_NAME}: downloading ${DEP_ARCHIVE}")
      file(DOWNLOAD "${DEP_ARCHIVE}" "${_dl}"
           EXPECTED_HASH SHA256=${DEP_SHA256}
           TLS_VERIFY ON
           STATUS _dl_status)
      list(GET _dl_status 0 _dl_rc)
      if(NOT _dl_rc EQUAL 0)
        list(GET _dl_status 1 _dl_msg)
        file(REMOVE "${_dl}")
        message(FATAL_ERROR
          "Could not download ${DEP_NAME} from ${DEP_ARCHIVE}: ${_dl_msg}\n"
          "Download it by hand and unpack it, then point ${DEP_OVERRIDE} at "
          "the result.")
      endif()
    endif()

    file(REMOVE_RECURSE "${_tmp}")
    file(MAKE_DIRECTORY "${_tmp}")
    file(ARCHIVE_EXTRACT INPUT "${_dl}" DESTINATION "${_tmp}")

    set(_extracted "${_tmp}")
    if(DEP_STRIP)
      set(_extracted "${_tmp}/${DEP_STRIP}")
    endif()
    if(NOT EXISTS "${_extracted}/${DEP_SENTINEL}")
      message(FATAL_ERROR
        "${DEP_NAME} archive did not contain ${DEP_STRIP}/${DEP_SENTINEL} "
        "(${DEP_ARCHIVE}).")
    endif()

    file(REMOVE_RECURSE "${_path}")
    file(RENAME "${_extracted}" "${_path}" RESULT _mv)
    file(REMOVE_RECURSE "${_tmp}")
    if(NOT _mv EQUAL 0)
      message(FATAL_ERROR "${DEP_NAME}: could not move the unpacked archive "
                          "into ${_path}: ${_mv}")
    endif()

  elseif(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
      message(FATAL_ERROR
        "${DEP_NAME} is missing and git is not installed. Either install git "
        "or unpack ${DEP_URL} (commit ${DEP_COMMIT}) into ${DEP_PATH}.")
    endif()

    set(_is_submodule FALSE)
    if(EXISTS "${CMAKE_SOURCE_DIR}/.gitmodules")
      file(READ "${CMAKE_SOURCE_DIR}/.gitmodules" _gitmodules)
      string(FIND "${_gitmodules}" "path = ${DEP_PATH}" _found)
      if(NOT _found EQUAL -1)
        set(_is_submodule TRUE)
      endif()
    endif()

    if(_is_submodule AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
      message(STATUS "${DEP_NAME}: fetching submodule ${DEP_PATH}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule update --init --filter=blob:none
                -- "${DEP_PATH}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} submodule update --init -- "${DEP_PATH}"
          WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
          RESULT_VARIABLE _rc)
      endif()
    endif()

    if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
      message(STATUS "${DEP_NAME}: cloning ${DEP_URL} @ ${DEP_COMMIT}")
      file(REMOVE_RECURSE "${_path}")
      execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --filter=blob:none "${DEP_URL}" "${_path}"
        RESULT_VARIABLE _rc)
      if(_rc EQUAL 0)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -c advice.detachedHead=false checkout
                  --quiet "${DEP_COMMIT}"
          WORKING_DIRECTORY "${_path}"
          RESULT_VARIABLE _rc)
      endif()
    endif()
  endif()

  if(NOT EXISTS "${_path}/${DEP_SENTINEL}")
    if(DEP_ARCHIVE)
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Unpack ${DEP_ARCHIVE} into "
        "${DEP_PATH}, or point ${DEP_OVERRIDE} at an existing checkout.")
    else()
      message(FATAL_ERROR
        "Could not provide ${DEP_NAME}. Fetch it manually with\n"
        "    git submodule update --init ${DEP_PATH}\n"
        "or point ${DEP_OVERRIDE} at an existing checkout.")
    endif()
  endif()

  if(DEP_COMMIT AND EXISTS "${_path}/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
      execute_process(
        COMMAND ${GIT_EXECUTABLE} -C "${_path}" rev-parse HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
      if(_rc EQUAL 0 AND NOT _head STREQUAL DEP_COMMIT)
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -C "${_path}" status --porcelain
                  --untracked-files=no
          OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET)
        if(_dirty)
          message(FATAL_ERROR
            "${DEP_NAME}: ${DEP_PATH} is at ${_head} with local changes, but "
            "cmake/Dependencies.cmake pins ${DEP_COMMIT}. Commit or stash "
            "them and re-run cmake to move to the pin, or keep working on "
            "this tree with -D${DEP_OVERRIDE}=${_path}.")
        endif()

        execute_process(
          COMMAND ${GIT_EXECUTABLE} -C "${_path}" cat-file -e
                  "${DEP_COMMIT}^{commit}"
          RESULT_VARIABLE _have ERROR_QUIET)
        if(NOT _have EQUAL 0)
          message(STATUS "${DEP_NAME}: fetching pinned ${DEP_COMMIT}")
          execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${_path}" fetch --quiet origin
            RESULT_VARIABLE _rc)
          execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${_path}" cat-file -e
                    "${DEP_COMMIT}^{commit}"
            RESULT_VARIABLE _have ERROR_QUIET)
          if(NOT _have EQUAL 0)
            execute_process(
              COMMAND ${GIT_EXECUTABLE} -C "${_path}" fetch --quiet origin
                      "${DEP_COMMIT}"
              RESULT_VARIABLE _have)
          endif()
          if(NOT _have EQUAL 0)
            message(FATAL_ERROR
              "${DEP_NAME}: pinned commit ${DEP_COMMIT} is not in ${DEP_PATH} "
              "and could not be fetched from origin. Fetch it by hand, or "
              "point ${DEP_OVERRIDE} at a checkout that has it.")
          endif()
        endif()

        message(STATUS "${DEP_NAME}: moving ${_head} -> pinned ${DEP_COMMIT}")
        execute_process(
          COMMAND ${GIT_EXECUTABLE} -C "${_path}" -c advice.detachedHead=false
                  checkout --quiet "${DEP_COMMIT}"
          RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
          message(FATAL_ERROR
            "${DEP_NAME}: could not check out ${DEP_COMMIT} in ${DEP_PATH}.")
        endif()
      endif()
    endif()
  endif()

  message(STATUS "${DEP_NAME}: ${_path}")
  set(${DEP_RESULT} "${_path}" PARENT_SCOPE)
endfunction()
