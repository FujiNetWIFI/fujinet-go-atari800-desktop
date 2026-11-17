# The KDE frontend: plain Qt6 Widgets.
#
# Deliberately NOT KDE Frameworks. It picks up Breeze through the platform
# theme anyway, and staying framework-free keeps this frontend usable outside
# a KDE session -- and keeps the door open to reusing it elsewhere.
#
# Found-or-skipped, like the GNOME one: a machine without Qt6 development
# packages still builds the core, the tests and the other frontend.

set(CMAKE_AUTOMOC ON)

# Qt 6.7 for QRhiWidget (the GPU display); ShaderTools bakes its shaders.
find_package(Qt6 6.7 COMPONENTS Widgets GuiPrivate ShaderTools QUIET)
if(NOT Qt6_FOUND)
  message(STATUS "kde frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

add_executable(fujinet-go-atari800-kde
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  SoftDisplayWidget.cpp
  FrameFeed.cpp
  KeyForward.cpp
  SettingsDialog.cpp
  FujiNetWindows.cpp
  help/HelpWindow.cpp
  controllers/ControllersWindow.cpp
  debugger/DebuggerWindow.cpp
)
target_link_libraries(fujinet-go-atari800-kde PRIVATE a800_session Qt6::Widgets Qt6::GuiPrivate)
# The display's shaders, baked for every backend and embedded as resources.
# PREFIX "/", not "/shaders": the files are listed as shaders/display.*, so
# the resource path is :/shaders/display.*.qsb (the display loads that).
qt_add_shaders(fujinet-go-atari800-kde "display_shaders"
  PREFIX "/"
  FILES shaders/display.vert shaders/display.frag)
target_compile_options(fujinet-go-atari800-kde PRIVATE -Wall -Wextra)
target_include_directories(fujinet-go-atari800-kde PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fujinet-go-atari800-kde PRIVATE
  A800_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

install(TARGETS fujinet-go-atari800-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The Controllers window, offscreen, against a running machine (the built-in OS: no
# ROMs, no FujiNet) and an SDL virtual gamepad: hidden at start, fixed size, the
# mouse holding the fire button and dragging the stick, the console keys, the
# paddles' knobs, the CX85, a mouse's mat, the Koala Pad, Map taking a key and a
# gamepad button, the keyboard's check box, F9, and a pad's row with its port
# picker. SKIPs (77) where SDL has no virtual joysticks. SDL3 is the session
# library's dependency, so the test finds it too.
find_package(Qt6 COMPONENTS Test QUIET)
find_package(SDL3 CONFIG QUIET)
if(Qt6Test_FOUND AND SDL3_FOUND)
  add_executable(controllers_test_kde controllers_test.cpp
                 controllers/ControllersWindow.cpp KeyForward.cpp
                 FujiNetWindows.cpp help/HelpWindow.cpp)
  set_target_properties(controllers_test_kde PROPERTIES CXX_STANDARD 17)
  target_link_libraries(controllers_test_kde PRIVATE a800_session Qt6::Widgets
                        Qt6::GuiPrivate Qt6::Test SDL3::SDL3)
  target_include_directories(controllers_test_kde PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(controllers_test_kde PRIVATE -Wall -Wextra)
  target_compile_definitions(controllers_test_kde PRIVATE
    A800_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")
  add_test(NAME controllers_kde COMMAND controllers_test_kde)
  set_tests_properties(controllers_kde PROPERTIES
                       SKIP_RETURN_CODE 77 TIMEOUT 900
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

configure_file(data/online.fujinet.go.atari800.kde.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari800.kde.desktop
               @ONLY)
configure_file(data/online.fujinet.go.atari800.kde.metainfo.xml.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari800.kde.metainfo.xml
               @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari800.kde.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari800.kde.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_file_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari800.kde.desktop)
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.atari800.kde.metainfo.xml)
endif()

# Guard: the baked shaders must sit at :/shaders/display.{vert,frag}.qsb. A
# wrong prefix leaves them empty at runtime and the display silently falls
# back to software.
add_test(NAME kde_shader_resource_path
         COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_shader_qrc.cmake)
