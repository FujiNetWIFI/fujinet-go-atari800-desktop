cmake_minimum_required(VERSION 3.20)

# Before project(): otherwise the bundle's minimum macOS is the CI SDK's.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")
endif()

# The only place the version lives (About boxes, bundle, release checks).
project(fujinet-go-atari800-desktop VERSION 0.1.0 LANGUAGES C CXX)

add_compile_definitions(A800_VERSION_STRING="${PROJECT_VERSION}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(FRONTEND "all" CACHE STRING
    "Frontend to build: gnome, kde, macos, windows, all, none")

# At the top so every subdirectory sees CMAKE_INSTALL_DATADIR (a cache var
# that is empty when GNUInstallDirs is first included below the top).
include(GNUInstallDirs)

include(cmake/Dependencies.cmake)
include(cmake/StageAtari800.cmake)
include(cmake/Atari800Core.cmake)

enable_testing()
add_subdirectory(core)

# Set up before the frontends so the macOS bundle can depend on it.
include(cmake/FujiNetRuntime.cmake)

function(add_frontend name)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/frontends/${name}/CMakeLists.txt")
    if(FRONTEND STREQUAL name)
      message(FATAL_ERROR "The ${name} frontend is not implemented yet.")
    endif()
    message(STATUS "${name} frontend: not implemented yet -- skipping")
    return()
  endif()
  add_subdirectory("frontends/${name}")
endfunction()

if(FRONTEND STREQUAL "gnome" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(gnome)
endif()
if(FRONTEND STREQUAL "kde" OR (FRONTEND STREQUAL "all" AND UNIX AND NOT APPLE))
  add_frontend(kde)
endif()
if(FRONTEND STREQUAL "macos" OR (FRONTEND STREQUAL "all" AND APPLE))
  add_frontend(macos)
endif()
if(FRONTEND STREQUAL "windows" OR (FRONTEND STREQUAL "all" AND WIN32))
  add_frontend(windows)
endif()

# Application icons (rendered from source art by tools/icons/make-icons.py).
# Desktop environments look the icon up by the desktop-entry id, so the same
# art is installed under each frontend's id, at every hicolor size. Only for
# frontends built here: a package should not ship files for an app it does
# not contain.
set(A800_ICON_SIZES 16 24 32 48 64 128 192 256 512)
set(A800_ICON_TARGETS_AND_IDS
  fujinet-go-atari800-gnome online.fujinet.go.atari800.gnome
  fujinet-go-atari800-kde online.fujinet.go.atari800.kde)
list(LENGTH A800_ICON_TARGETS_AND_IDS _a800_icon_n)
math(EXPR _a800_icon_last "${_a800_icon_n} - 1")
foreach(_i RANGE 0 ${_a800_icon_last} 2)
  list(GET A800_ICON_TARGETS_AND_IDS ${_i} _target)
  math(EXPR _j "${_i} + 1")
  list(GET A800_ICON_TARGETS_AND_IDS ${_j} _id)
  if(TARGET ${_target})
    foreach(_size IN LISTS A800_ICON_SIZES)
      install(FILES data/icons/hicolor/${_size}x${_size}/apps/fujinet-go-atari800.png
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
              RENAME ${_id}.png)
    endforeach()
  endif()
endforeach()
