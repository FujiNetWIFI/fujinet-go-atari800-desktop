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
