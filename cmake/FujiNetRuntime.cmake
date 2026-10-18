# The FujiNet runtime (libfujinet), built from fujinet-firmware and bundled.
#
# tools/fujinet/build-fujinet-desktop.sh stages, patches and builds the ATARI
# target into tools/fujinet/work/out, where an uninstalled run and the
# install rules find it. It runs on every build (it is incremental); with
# FUJINET_AUTO_UPDATE it first moves the pin to fujinet-firmware's newest
# commit when every patch still applies (tools/fujinet/update-fujinet-firmware.sh).

option(WITH_FUJINET "Build and bundle the FujiNet runtime" ON)

# Local builds follow upstream; CI, flatpak and release builds use the pin.
set(_fn_auto_default ON)
if(DEFINED ENV{CI} OR DEFINED ENV{FLATPAK_ID} OR DEFINED ENV{FLATPAK_BUILDER_BUILDDIR}
   OR FUJINET_SRC OR DEFINED ENV{FUJINET_SRC})
  set(_fn_auto_default OFF)
endif()
option(FUJINET_AUTO_UPDATE "Move the FujiNet pin to fujinet-firmware's newest commit when building"
       ${_fn_auto_default})

set(FUJINET_OUT "${CMAKE_SOURCE_DIR}/tools/fujinet/work/out")
if(APPLE)
  set(FUJINET_LIB_NAME "libfujinet.dylib")
elseif(WIN32)
  set(FUJINET_LIB_NAME "fujinet.dll")
else()
  set(FUJINET_LIB_NAME "libfujinet.so")
endif()
set(FUJINET_LIB "${FUJINET_OUT}/${FUJINET_LIB_NAME}")

if(WITH_FUJINET)
  a800_provide_dependency(
    NAME fujinet-firmware
    PATH third_party/fujinet-firmware
    URL "${FUJINET_URL}"
    COMMIT "${FUJINET_COMMIT}"
    SENTINEL build.sh
    OVERRIDE FUJINET_SRC
    RESULT FUJINET_DIR)

  set(FUJINET_SCRIPT "${CMAKE_SOURCE_DIR}/tools/fujinet/build-fujinet-desktop.sh")
  set(FUJINET_UPDATE "${CMAKE_SOURCE_DIR}/tools/fujinet/update-fujinet-firmware.sh")

  # a native Windows cmake would hand a bare .sh to cmd.exe
  find_program(BASH_EXECUTABLE bash)
  if(NOT BASH_EXECUTABLE)
    message(FATAL_ERROR "bash is required to build the FujiNet runtime (-DWITH_FUJINET=OFF "
                        "builds without it). On Windows, build from an MSYS2 UCRT64 shell.")
  endif()

  set(_fn_steps "")
  if(FUJINET_AUTO_UPDATE)
    list(APPEND _fn_steps COMMAND "${BASH_EXECUTABLE}" "${FUJINET_UPDATE}" --auto)
  endif()
  list(APPEND _fn_steps COMMAND ${CMAKE_COMMAND} -E env "FUJINET_SRC=${FUJINET_DIR}"
                                "${BASH_EXECUTABLE}" "${FUJINET_SCRIPT}")
  # always runs: the script decides what is out of date
  add_custom_target(fujinet-runtime ALL
    ${_fn_steps}
    BYPRODUCTS "${FUJINET_LIB}"
    COMMENT "FujiNet runtime (the first build takes a few minutes)"
    USES_TERMINAL
    VERBATIM)

  add_custom_target(fujinet-runtime-refresh
    COMMAND ${CMAKE_COMMAND} -E env "FUJINET_SRC=${FUJINET_DIR}" "FN_REFRESH=1"
            "${BASH_EXECUTABLE}" "${FUJINET_SCRIPT}"
    COMMENT "Re-staging and rebuilding the FujiNet runtime"
    USES_TERMINAL
    VERBATIM)
endif()

if(WITH_FUJINET OR EXISTS "${FUJINET_LIB}")
  if(WIN32)
    install(FILES "${FUJINET_LIB}" DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES "${FUJINET_OUT}/fnconfig.ini" DESTINATION ${CMAKE_INSTALL_BINDIR}/fujinet)
    install(DIRECTORY "${FUJINET_OUT}/data" "${FUJINET_OUT}/SD"
            DESTINATION ${CMAKE_INSTALL_BINDIR}/fujinet)
  else()
    install(FILES "${FUJINET_LIB}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/fujinet-go-atari800)
    install(FILES "${FUJINET_OUT}/fnconfig.ini"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/fujinet-go-atari800/fujinet)
    install(DIRECTORY "${FUJINET_OUT}/data" "${FUJINET_OUT}/SD"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/fujinet-go-atari800/fujinet)
  endif()
endif()
