# The emulator core (staged atari800 + host layer) and the session layer.
a800_add_core(atari800_core "${ATARI800_GEN}")

# the host: emulation thread, pacing, audio ring, frames (no SDL)
add_library(a800_host STATIC atari800/A800Host.cpp)
set_target_properties(a800_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(a800_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/atari800")
target_link_libraries(a800_host PUBLIC atari800_core)
target_compile_options(a800_host PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>)

add_executable(a800_headless tools/a800_headless.c)
target_link_libraries(a800_headless PRIVATE atari800_core)

if(A800_AB_TEST)
  a800_add_core(atari800_core_stock "${ATARI800_STOCK}" STOCK)
  add_executable(a800_headless_stock tools/a800_headless.c)
  target_link_libraries(a800_headless_stock PRIVATE atari800_core_stock)
  # the A/B build of the patched core: deterministic too
  a800_add_core(atari800_core_det "${ATARI800_GEN}")
  target_compile_definitions(atari800_core_det PUBLIC A800_DETERMINISTIC)
  add_executable(a800_headless_det tools/a800_headless.c)
  target_link_libraries(a800_headless_det PRIVATE atari800_core_det)
endif()

# ---- ROM images a developer build embeds (never shipped; COMPLIANCE.md)
option(WITH_ATARI800_ROMS "Embed the ROM images in tools/roms (NOT redistributable)" OFF)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(A800_ROMS_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
if(WITH_ATARI800_ROMS)
  file(GLOB _a800_rom_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/*")
  add_custom_command(OUTPUT "${A800_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            "${A800_ROMS_C}" "${CMAKE_SOURCE_DIR}/tools/roms"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py" ${_a800_rom_files} VERBATIM)
else()
  add_custom_command(OUTPUT "${A800_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            --none "${A800_ROMS_C}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py" VERBATIM)
endif()

# ---- a800_session: the frontend contract (needs SDL3)
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
  message(STATUS "a800_session: SDL3 not found -- core and its tests only")
  add_subdirectory(tests)
  return()
endif()

set(A800_SESSION_C_SOURCES
  src/settings.c src/paths.c src/prefs.c src/roms.c src/zipread.c src/keymap.c
  src/bindings.c src/hid_keys.c src/audio_sdl.c src/gamepad_sdl.c src/fujinet_runtime.c
  src/fujinet_api.c
  "${A800_ROMS_C}")
set(A800_SESSION_CXX_SOURCES src/session.cpp src/input.cpp src/media.cpp src/debug.cpp)

add_library(a800_session STATIC ${A800_SESSION_C_SOURCES} ${A800_SESSION_CXX_SOURCES})
set_target_properties(a800_session PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(a800_session PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(a800_session PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(a800_session PUBLIC a800_host SDL3::SDL3 ${CMAKE_DL_LIBS})
set_source_files_properties(${A800_SESSION_C_SOURCES} ${A800_SESSION_CXX_SOURCES}
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
set_source_files_properties("${A800_ROMS_C}" PROPERTIES COMPILE_OPTIONS "-w")
target_compile_definitions(a800_session PRIVATE
  A800_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  A800_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-atari800"
  A800_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-atari800")

add_subdirectory(tests)
