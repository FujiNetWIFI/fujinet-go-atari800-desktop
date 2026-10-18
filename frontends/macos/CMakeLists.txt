# The macOS frontend: AppKit, no toolkit dependencies beyond the system
# frameworks.
#
# Written on Linux and never compiled on a Mac -- .github/workflows/macos.yml
# is the first thing that will actually try it, on both an arm64 and an
# x86_64 runner, and a failure there is real signal about this code.

if(NOT APPLE)
  message(STATUS "macos frontend: not a macOS build -- skipping")
  return()
endif()

enable_language(OBJC)

# The .icns has to be added as a SOURCE with MACOSX_PACKAGE_LOCATION so CMake
# copies it into the bundle, and MACOSX_BUNDLE_ICON_FILE names it for
# Info.plist.in's ${MACOSX_BUNDLE_ICON_FILE}. Neither alone is enough.
set(A800_ICNS "${CMAKE_SOURCE_DIR}/data/icons/fujinet-go-atari800.icns")

# The licences, in the bundle's Resources: the GPL, and atari800's.
configure_file("${ATARI800_DIR}/COPYING"
               "${CMAKE_CURRENT_BINARY_DIR}/COPYING.atari800" COPYONLY)
set(A800_BUNDLE_LICENSES
  "${CMAKE_SOURCE_DIR}/LICENSE"
  "${CMAKE_SOURCE_DIR}/COMPLIANCE.md"
  "${CMAKE_CURRENT_BINARY_DIR}/COPYING.atari800")

add_executable(fujinet-go-atari800-macos MACOSX_BUNDLE
  main.m
  AppDelegate.m
  DisplayView.m
  KeyForward.m
  SettingsWindow.m
  controllers/ControllersWindow.m
  debugger/DebuggerWindow.m
  "${A800_ICNS}"
  ${A800_BUNDLE_LICENSES}
)
set_source_files_properties("${A800_ICNS}" ${A800_BUNDLE_LICENSES} PROPERTIES
  MACOSX_PACKAGE_LOCATION "Resources")

set_target_properties(fujinet-go-atari800-macos PROPERTIES
  # The bundle is named for the product, not the target: "FujiNet Go Atari
  # 800.app", which is what the packaging workflow refers to. It has to
  # agree with MACOSX_BUNDLE_EXECUTABLE_NAME, which names the binary inside.
  OUTPUT_NAME "FujiNet Go Atari 800"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_EXECUTABLE_NAME "FujiNet Go Atari 800"
  MACOSX_BUNDLE_ICON_FILE "fujinet-go-atari800.icns"
  MACOSX_BUNDLE_BUNDLE_NAME "FujiNet Go Atari 800"
  MACOSX_BUNDLE_GUI_IDENTIFIER "online.fujinet.go.atari800.macos"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")

target_include_directories(fujinet-go-atari800-macos PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fujinet-go-atari800-macos PRIVATE a800_session
  "-framework Cocoa"
  "-framework CoreVideo"    # CVDisplayLink: this platform's frame clock
  "-framework CoreGraphics"
  "-framework QuartzCore"
  "-framework UniformTypeIdentifiers"   # UTType: the open panels' filters
  "-framework Carbon")      # kVK_* virtual key codes
target_compile_options(fujinet-go-atari800-macos PRIVATE -Wall -Wextra
  -fobjc-arc)

install(TARGETS fujinet-go-atari800-macos BUNDLE DESTINATION .)

# Pack the FujiNet runtime into the bundle so the .app is self-contained.
# Everything goes BESIDE THE EXECUTABLE in Contents/MacOS, not into the
# Frameworks/Resources layout a Mac app would normally use, because that is
# where core/src/paths.c looks: it probes the executable's directory for
# libfujinet.dylib and takes the asset tree from its "fujinet" subdir (which
# main.m also hands the session as fujinet_runtime_src).
if(TARGET fujinet-runtime)
  add_dependencies(fujinet-go-atari800-macos fujinet-runtime)
  set(_exedir "$<TARGET_FILE_DIR:fujinet-go-atari800-macos>")
  add_custom_command(TARGET fujinet-go-atari800-macos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FUJINET_LIB}" "${_exedir}/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FUJINET_OUT}/data" "${_exedir}/fujinet/data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FUJINET_OUT}/SD" "${_exedir}/fujinet/SD"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FUJINET_OUT}/fnconfig.ini" "${_exedir}/fujinet/"
    COMMENT "Packing the FujiNet runtime into the bundle"
    VERBATIM)
endif()
