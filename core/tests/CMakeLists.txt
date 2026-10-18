# ---- the core on its own -------------------------------------------------
# AltirraOS + Altirra BASIC are built in: every machine boots with no ROMs.
set(_altirra -xl-rev altirra -800-rev altirra -basic-rev altirra)

add_test(NAME boot_800xl_basic
         COMMAND a800_headless --frames 300 --expect-text Ready -- -xl -basic ${_altirra})
add_test(NAME boot_130xe_basic
         COMMAND a800_headless --frames 300 --expect-text Ready -- -xe -basic ${_altirra})
add_test(NAME boot_800_basic
         COMMAND a800_headless --frames 300 --expect-text Ready -- -atari -basic ${_altirra})
add_test(NAME boot_xegs_basic
         COMMAND a800_headless --frames 300 --expect-text Ready -- -xegs -basic ${_altirra})
add_test(NAME basic_types
         COMMAND a800_headless --frames 600 --type "PRINT 6*7\n" --expect-text 42
                 -- -xl -basic ${_altirra})

# Patched vs unpatched atari800, both deterministic: identical frames and
# audio, so the patches and our config change nothing about emulation.
if(A800_AB_TEST)
  foreach(_m xl xe atari xegs)
    add_test(NAME ab_${_m}
             COMMAND ${CMAKE_COMMAND}
                     -DA=$<TARGET_FILE:a800_headless_det>
                     -DB=$<TARGET_FILE:a800_headless_stock>
                     "-DARGS=--frames|900|--hash|--type|PRINT 6*7\\n|--|-${_m}|-basic|-xl-rev|altirra|-800-rev|altirra|-basic-rev|altirra"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/ab_compare.cmake")
  endforeach()
endif()

# ---- the session layer ----------------------------------------------------
if(TARGET a800_session)
  add_executable(session_test session_test.c)
  target_link_libraries(session_test PRIVATE a800_session)
  add_test(NAME session COMMAND session_test)
  set_tests_properties(session PROPERTIES TIMEOUT 180)
endif()
if(TARGET a800_session)
  add_executable(rom_test rom_test.c)
  target_link_libraries(rom_test PRIVATE a800_session)
  add_test(NAME roms COMMAND rom_test)
endif()
if(TARGET a800_session)
  add_executable(video_test video_test.c)
  target_link_libraries(video_test PRIVATE a800_session)
  add_test(NAME video COMMAND video_test)
  set_tests_properties(video PROPERTIES TIMEOUT 120)
endif()
if(TARGET a800_session)
  add_executable(netboot_test netboot_test.c)
  target_link_libraries(netboot_test PRIVATE a800_session)
  add_test(NAME netboot COMMAND netboot_test)
  set_tests_properties(netboot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)
endif()
if(TARGET a800_session)
  add_executable(watchdog_test watchdog_test.c)
  target_link_libraries(watchdog_test PRIVATE a800_session)
  if(WIN32)
    target_link_libraries(watchdog_test PRIVATE ws2_32)
  endif()
  add_test(NAME watchdog COMMAND watchdog_test)
  set_tests_properties(watchdog PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)
endif()

find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM AND WITH_FUJINET)
  add_test(NAME fujinet_update COMMAND "${BASH_PROGRAM}" "${CMAKE_CURRENT_SOURCE_DIR}/update_test.sh")
  set_tests_properties(fujinet_update PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS network)
endif()
if(TARGET a800_session)
  add_executable(media_test media_test.c)
  target_link_libraries(media_test PRIVATE a800_session)
  add_test(NAME media COMMAND media_test)
  set_tests_properties(media PROPERTIES TIMEOUT 180 RUN_SERIAL ON)
endif()
if(TARGET a800_session)
  foreach(_t gamepad gamepad_hotplug input)
    add_executable(${_t}_test ${_t}_test.c)
    target_link_libraries(${_t}_test PRIVATE a800_session)
    target_include_directories(${_t}_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
    add_test(NAME ${_t} COMMAND ${_t}_test)
    set_tests_properties(${_t} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  endforeach()
endif()
