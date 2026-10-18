# FujiNet Go Atari 800

A self-contained Atari 400/800/XL/XE with FujiNet built in, for Linux (GNOME
and KDE), macOS and Windows. The emulator is
[atari800](https://github.com/atari800/atari800); FujiNet runs in-process and
talks to it over NetSIO.

## Building

    cmake -B build && cmake --build build && ctest --test-dir build

Work in progress; see `TODO`.
