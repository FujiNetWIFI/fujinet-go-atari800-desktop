# The emulator core (staged atari800 + host layer).
a800_add_core(atari800_core "${ATARI800_GEN}")

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

add_subdirectory(tests)
