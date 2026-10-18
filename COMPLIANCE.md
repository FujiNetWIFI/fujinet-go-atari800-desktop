# Compliance

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **atari800** (the emulator: CPU, ANTIC, GTIA, POKEY, PIA, SIO, NetSIO, cartridges, peripherals, monitor) | [atari800/atari800](https://github.com/atari800/atari800), pinned in `cmake/Dependencies.cmake`, plus `tools/atari800/patches/` | GPL-2.0-or-later | `src/` staged and patched by `cmake/StageAtari800.cmake`, compiled with `core/atari800/config.h` |
| **AltirraOS, Altirra BASIC** (built-in OS and BASIC replacements) | atari800's `src/roms/` (built from its `emuos/`) | permissive (notice-preserving), © Avery Lee | compiled into the core; the default ROMs |
| **FujiNet firmware** (`libfujinet`) | [FujiNetWIFI/fujinet-firmware](https://github.com/FujiNetWIFI/fujinet-firmware), PC target `ATARI`, plus `tools/fujinet/patches/` | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`ed at run time |
| SDL3 | libsdl-org | Zlib | audio and gamepads; system package on Linux, static in releases |
| zlib | zlib.net | Zlib | gzipped disk images and save states |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | for `libfujinet` |
| OpenSSL 3, expat | openssl.org, libexpat | Apache-2.0, MIT | for `libfujinet` (its bundled libssh and libsmb2, its HTTP parser); system libraries on Linux and macOS, linked into `fujinet.dll` on Windows |

The combined work is distributed under GPL-3.0.

## What is changed in atari800

Each patch in `tools/atari800/patches/` is small and named for its purpose;
none changes emulation, which the `ab_*` tests check (patched and unpatched
cores, run deterministically, produce identical frames and audio).

## System ROMs — none ship

The Atari OS, BASIC and XEGS game ROMs and the peripheral firmware (XEP80,
1400XL/1450XLD, MIO, Black Box, Austin Franklin 80, Bit3, Proto80) are
copyrighted. **This project distributes none of them, and none is needed:**
every machine boots AltirraOS and Altirra BASIC.

- **Import ▸ System ROMs…** copies a user's own image into the data
  directory, recognised by size and CRC-32; Preferences then offers it.
- **`WITH_ATARI800_ROMS`** (default OFF, and OFF for every published
  artifact) lets a developer's build embed recognised images from
  `tools/roms/`. The `no_embedded_roms_*` tests check every shipped binary.

## Icon and name

The icon is the FujiNet Go family mark on the Atari 800's case beige
(`tools/icons/make-icons.py`). "Atari", "400", "800", "XL", "XE" and "XEGS"
are trademarks of Atari Interactive, used to name the machines emulated; the
project is not affiliated with or endorsed by Atari.
