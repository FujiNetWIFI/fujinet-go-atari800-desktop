# FujiNet Go — Atari 800

The Atari 400, 800, XL and XE computers with a built-in
[FujiNet](https://fujinet.online/) on the SIO port: power on into FujiNet's
CONFIG, mount disks from network hosts or the SD card, and run network
programs, all in one desktop app. A member of the FujiNet Go desktop family.

| | |
|---|---|
| **Emulator** | [atari800](https://github.com/atari800/atari800), pinned and staged with small patches (`tools/atari800/patches/`), driven by its own platform layer. |
| **Machines** | 400, 800, 1200XL, 600XL, 800XL, 65XE, 130XE, 320XE (Compy, Rambo), 576XE, 1088XE, XEGS, 1400XL, 1450XLD. No 5200. |
| **ROMs** | None needed: AltirraOS and Altirra BASIC are built in. Atari's own OS, BASIC and peripheral ROMs can be imported (ROM images or MAME sets). |
| **Peripherals** | XEP80, Austin Franklin and Bit 3 80-column cards, Proto80, MIO, Black Box, 1450XLD, Voice Box, MyIDE, R-Time 8, H: devices, Axlon/Mosaic/MapRAM, stereo POKEY. |
| **FujiNet** | The firmware's `ATARI` PC target, in-process as `libfujinet`, on atari800's NetSIO hub: UDP **11514**, web UI **11515** (loopback). |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/GDI): display, Controllers panel, Preferences, FujiNet log, debugger. |
| **Packaging** | DEB/RPM/TGZ per Linux frontend, two Flatpaks, Windows zip and NSIS installer, macOS arm64 and x86_64 bundles. |

## Using it

- **Open…** routes by file: a disk boots (with FujiNet on it is copied to
  FujiNet's SD card and mounted), a program runs, a cartridge goes in (a raw
  image asks for its board), a tape loads, a ROM is imported. Dropping a file
  on the window does the same.
- **Drives** are atari800's own with FujiNet off, FujiNet's slots with it on.
  **Reboot to CONFIG** ejects what Open mounted and power-cycles.
- **Save states** need FujiNet off (it would replay SIO traffic). The
  debugger's snapshot slots always work.
- **Preferences** are one table (`core/src/prefs.c`) every frontend builds its
  dialog from. Live settings apply at once; machine settings apply together
  when the dialog closes (one power cycle).

### Keys

| Key | |
|---|---|
| F2 F3 F4 | Option, Select, Start |
| F5 / Shift+F5 | Reset / Power Cycle |
| F6 | Help |
| F7, Pause (F13 on a Mac) | Break |
| Keypad 8 4 6 2, 0 | Joystick 1, fire |
| F9 / F11 / F12 | Controllers / Fullscreen / Debugger |
| Ctrl+Shift+O, C, V, W, Q (Cmd on a Mac) | Open, Copy Screen Text, Paste, Warp, Quit |

CONTROL+letter stays the Atari's. Keys type the character the host keyboard
makes; Preferences ▸ Input ▸ Keys switches to the Atari's own key positions.
Gamepads are picked up as they are plugged in and out; the Controllers panel
assigns them to ports and maps keys and buttons.

### Debugger (F12)

Stops the machine. Run/Stop F5, Step F7, Step Over F8, Step Out Shift+F8,
Frame F9. Tabs: atari800's monitor as a console (including the `A`
assembler), CPU and memory, disassembly with OS labels and symbol files
(ld65 `-Ln`, VICE, MADS, atari800), ANTIC and the display list, GTIA and the
palette, POKEY and PIA, the memory map and cartridge, breakpoints and
watchpoints with conditions (atari800's table holds 20 conditions), and every
SIO command. A CIM stops into it.

## Building

    cmake -B build -G Ninja
    cmake --build build
    ctest --test-dir build

Needs CMake 3.20+, a C11/C++17 compiler, SDL3, zlib, and a frontend toolkit
(GTK 4.12 + libadwaita 1.5, or Qt 6). The FujiNet runtime build also needs
Python 3 with PyYAML and Jinja2, OpenSSL and expat; `-DWITH_FUJINET=OFF`
skips it. atari800 and fujinet-firmware are fetched at their pins
(`cmake/Dependencies.cmake`).

- **Flatpak:** `flatpak-builder --user --install build-flatpak
  build-aux/flatpak/online.fujinet.go.atari800.gnome.yml` (or `.kde.yml`).
- **Windows:** natively in MSYS2 UCRT64, or cross-compiled with
  `cmake/toolchains/mingw-w64.cmake` and run under Wine.
- **macOS:** built on CI (`.github/workflows/macos.yml`).

**FujiNet updates itself.** A local build checks fujinet-firmware's `master`
(at most every ten minutes). When it is newer and every patch still applies,
the build moves all three pins (`cmake/Dependencies.cmake` and both flatpak
manifests) for you to commit; when a patch no longer applies it keeps the old
pin and says which. CI, flatpak and release builds use the committed pin.
`tools/fujinet/update-fujinet-firmware.sh` does it by hand.

## Licences

GPL-3.0-or-later (`LICENSE`). atari800 is GPL-2.0-or-later; AltirraOS and
Altirra BASIC are Avery Lee's, freely redistributable. No Atari ROM is in any
published build (`COMPLIANCE.md`).
