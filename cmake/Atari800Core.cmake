# atari800_core: the staged atari800 emulator compiled as a static library
# with core/atari800/config.h and the host layer in core/atari800/host.
#
# a800_add_core(<target> <staged dir> [STOCK]) -- STOCK builds the unpatched
# tree for the A/B tests (deterministic, unthrottled).

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

set(ATARI800_CORE_FILES
  acidtest.c afile.c af80.c antic.c artifact.c atari.c binload.c bit3.c
  cartridge.c cartridge_info.c cassette.c cfg.c colours.c colours_external.c
  colours_ntsc.c colours_pal.c compfile.c cpu.c crc32.c cycle_map.c devices.c
  esc.c filter_ntsc.c gtia.c ide.c img_tape.c input.c log.c memory.c
  monitor.c mzpokeysnd.c pal_blending.c pbi.c pbi_bb.c pbi_mio.c
  pbi_proto80.c pbi_scsi.c pbi_xld.c pia.c pokey.c pokeysnd.c rdevice.c
  remez.c rtime.c screen.c sio.c sound.c statesav.c sysrom.c util.c
  videomode.c voicebox.c votrax.c votraxsnd.c xep80.c xep80_fonts.c
  atari_ntsc/atari_ntsc.c
  roms/altirra_5200_charset.c roms/altirra_5200_os.c roms/altirra_basic.c
  roms/altirraos_800.c roms/altirraos_xl.c)
if(WIN32)
  list(APPEND ATARI800_CORE_FILES netsiowin.c)
else()
  list(APPEND ATARI800_CORE_FILES netsio.c)
endif()

set(ATARI800_HOST_DIR "${CMAKE_SOURCE_DIR}/core/atari800")

function(a800_add_core target gen)
  cmake_parse_arguments(C "STOCK" "" "" ${ARGN})
  set(_src "")
  foreach(_f IN LISTS ATARI800_CORE_FILES)
    list(APPEND _src "${gen}/src/${_f}")
  endforeach()
  list(APPEND _src
    "${ATARI800_HOST_DIR}/host/platform.c"
    "${ATARI800_HOST_DIR}/host/video.c"
    "${ATARI800_HOST_DIR}/host/ui_stub.c"
    "${ATARI800_HOST_DIR}/host/file_export_stub.c")
  add_library(${target} STATIC ${_src})
  set_target_properties(${target} PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
  # config.h must win over any stray one in the staged tree
  target_include_directories(${target} BEFORE PUBLIC
    "${ATARI800_HOST_DIR}" "${ATARI800_HOST_DIR}/host" "${gen}/src")
  target_compile_definitions(${target} PUBLIC _GNU_SOURCE _DEFAULT_SOURCE)
  if(C_STOCK)
    target_compile_definitions(${target} PUBLIC A800_STOCK A800_DETERMINISTIC)
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /w)
  else()
    # upstream code: its own warnings are not ours to fix
    target_compile_options(${target} PRIVATE -w)
  endif()
  target_link_libraries(${target} PUBLIC ZLIB::ZLIB Threads::Threads)
  if(WIN32)
    target_link_libraries(${target} PUBLIC ws2_32 winmm)
  else()
    target_link_libraries(${target} PUBLIC m)
  endif()
endfunction()
