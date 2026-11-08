This is the text of FujiNet Go Atari 800's built-in Help (Help ▸ FujiNet Go
Atari 800 Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only
to the frontend it is about; here you see them all. Every topic below is one
page in the app.

# Getting started

FujiNet Go Atari 800 is an Atari 400, 800, XL or XE computer with a FujiNet
on its SIO port. The computer is the atari800 emulator; the FujiNet is the
real FujiNet firmware running inside the app, so FujiNet's CONFIG comes up at
once and mounts disks from network hosts or the SD card.

1. **Start the app.** The computer powers on into CONFIG (a 130XE by default).
   It needs no ROMs: AltirraOS and Altirra BASIC are built in. See
   [System ROMs](#system-roms).
2. **Pick a disk in CONFIG.** Choose a host (the SD folder or a server on the
   network) and a disk or program on it, and boot. See [FujiNet](#fujinet).
3. **Or open a file of your own.** **Open…** takes a disk, a program, a
   cartridge, a tape or a ROM and does the right thing with it. See
   [Opening files](#opening-files).
4. **Play.** The keys, the gamepads and the Controllers window
   (<kbd>F9</kbd>) work the joysticks and the keyboard. See
   [Controllers and keys](#controllers-and-keys).
5. **Change the computer.** [Preferences](#preferences) picks the model,
   memory, ROMs, peripherals, picture, sound, controls and FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers window and the FujiNet
console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.
(<kbd>F6</kbd> is the Atari's own HELP key, not this Help.)

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-atari800-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The computer's picture fills the window. The menus have:

- **Open…**, **Power Cycle** (<kbd>Shift+F5</kbd>), **Reset** (<kbd>F5</kbd>),
  **Reboot to CONFIG**, **Warp Speed**, **Save State…**, **Load State…** and
  **Import System ROMs…**.
- **Drives**, **Cartridge** and **Program Recorder**: the media. See
  [Drives and disks](#drives-and-disks), [Cartridges](#cartridges) and
  [Tapes](#tapes).
- **Console Keys**: Option, Select, Start, Help and Break.
- **Copy Screen Text** and **Paste**.
- **Controllers** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **80-Column Display**, **TV Aspect**, **Smooth Scaling** and
  **Fullscreen** (<kbd>F11</kbd>).
- **FujiNet Web UI**, **Console Log** and **Restart FujiNet**.
- **Preferences** (<kbd>Ctrl+,</kbd>), **Help**, **About** and **Quit**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde windows -->

The menu bar has Machine, Drives, Cartridge, Program Recorder, Console, Edit,
View, FujiNet and Help. The Machine menu has Open, Power Cycle, Reset, Reboot
to CONFIG, Warp Speed, the states, Import System ROMs, Preferences (Settings
on Windows) and Quit (Exit on Windows).

<!-- end -->

<!-- only: macos -->

On a Mac, Preferences (called Settings) and Quit are in the app menu, and
the rest are in the Machine, Drives, Cartridge, Program Recorder, Console,
Edit, View and FujiNet menus.

<!-- end -->

## Reset and power

- **Reset** (<kbd>F5</kbd>) presses the Atari's RESET key: the program
  starts again with its memory as it was.
- **Power Cycle** (<kbd>Shift+F5</kbd>) is the power switch: memory is
  cleared and the computer boots again. With FujiNet on, FujiNet restarts
  too (Preferences ▸ FujiNet ▸ *Restart FujiNet on power cycle*).
- **Reboot to CONFIG** ejects what Open mounted and power-cycles, so CONFIG
  comes up. It is there when FujiNet is on.
- **Warp Speed** runs the computer as fast as the host allows.

<!-- only: gnome kde windows -->

The Atari keys **Option**, **Select**, **Start**, **Help** and **Break** are
<kbd>F2</kbd>, <kbd>F3</kbd>, <kbd>F4</kbd>, <kbd>F6</kbd> and <kbd>F7</kbd>
(or <kbd>Pause</kbd>). They are also in the Console menu.

<!-- end -->

<!-- only: macos -->

The Atari keys **Option**, **Select**, **Start**, **Help** and **Break** are
<kbd>F2</kbd>, <kbd>F3</kbd>, <kbd>F4</kbd>, <kbd>F6</kbd> and <kbd>F7</kbd>
(F13 on a Mac keyboard that has no Pause). They are also in the Console menu.

<!-- end -->

## The picture

**TV Aspect** shows the picture 4:3 as a television does, or with square
pixels. **Smooth Scaling** blends the pixels when the picture is stretched.
Preferences ▸ Video picks how much of the picture shows, and its colours. With
an 80-column card chosen in Preferences ▸ Peripherals, **80-Column Display**
shows it in place of the 40-column picture.

## Copy and paste

**Copy Screen Text** copies the text on the Atari's screen to the clipboard.
**Paste** types the clipboard into the Atari, a key at a time.

<!-- only: gnome kde windows -->

The shortcuts are <kbd>Ctrl+Shift+C</kbd> and <kbd>Ctrl+Shift+V</kbd>, so
<kbd>Ctrl+C</kbd> and <kbd>Ctrl+V</kbd> stay the Atari's.

<!-- end -->

<!-- only: macos -->

The shortcuts are ⌘C and ⌘V.

<!-- end -->

# Opening files

**Open…** (<kbd>Ctrl+Shift+O</kbd>) works out what a file is, and dropping a
file on the window does the same:

- **A disk** boots. With FujiNet on it is copied to FujiNet's SD card and
  mounted there; with FujiNet off it goes in drive 1.
- **A program** (an Atari executable) runs.
- **A cartridge** goes in. A raw image asks which board it is. See
  [Cartridges](#cartridges).
- **A tape** loads in the program recorder. See [Tapes](#tapes).
- **A ROM** is imported. See [System ROMs](#system-roms).

If the file cannot be used, a message says why and what was running goes on.

# Drives and disks

With FujiNet off, the drives are atari800's own: **Drives** lists D1: to D8:,
each with **Insert…**, **New Blank Disk** (single density 90K, enhanced
density 130K or double density 180K), **Eject** and **Write Protect**. With
FujiNet on, the drives are FujiNet's slots: **Mount from the SD Card…** puts
a disk in a slot, and CONFIG mounts the rest from its hosts.

- **Import to SD Card…** copies a disk or program into FujiNet's SD folder.
- **Open SD Folder** and **Open Media Folder** show those folders in the file
  manager.

**Fast disk access** (Preferences ▸ Machine) speeds up the internal drives
only, not FujiNet's. **Load programs at disk speed** makes programs load at
the real drive's speed. The H: devices (Preferences ▸ Peripherals) give the
Atari folders of the host as H1: to H4:.

<!-- context: import-sd -->
## Import to SD Card

**Import to SD Card…** copies a disk or program into FujiNet's SD folder, so
CONFIG can list it and mount it. It is there when FujiNet is on; without the
FujiNet runtime there is no SD folder to copy to.

# Cartridges

**Cartridge ▸ Insert Cartridge…** takes a cartridge image (`.car` or a raw
`.rom`/`.bin`, or a zip of one). A `.car` file says what board it is; for a
raw image the app asks, listing the boards that size can be. **Insert
Piggyback…** adds a second cartridge on a piggyback. **Remove Cartridge** and
**Remove Piggyback** take them out. The menu shows what is in the slot.

# Tapes

**Program Recorder** works the cassette: **Load Tape…** puts in a tape image,
**Record New Tape…** starts an empty one, **Rewind** goes to the start,
**Boot Tape (Start held at power-on)** power-cycles with Start held so the
tape boots, and **Eject** takes it out. The menu shows the block the tape is
at.

# Save states

**Save State…** and **Load State…** keep the whole computer in a file. They
work only with FujiNet off, because FujiNet would replay the SIO traffic
since the state was taken, which it has already acted on. The Debugger's
snapshot slots always work. States are kept in the `states` folder (see
[Files and folders](#files-and-folders)).

# Controllers and keys

The Atari has a keyboard, four controller ports and a row of console keys
(Start, Select, Option, Reset, Help and Break). Up to four joysticks can be
plugged in on a 400 or 800, two on an XL or XE (four with the MultiJoy4
interface in Preferences ▸ Input).

## Default keys

Keys go by where they are on the keyboard, whatever your layout.

| Does | Key |
|---|---|
| Joystick 1 directions | <kbd>Keypad 8</kbd>, <kbd>Keypad 2</kbd>, <kbd>Keypad 4</kbd>, <kbd>Keypad 6</kbd> |
| Joystick 1 fire | <kbd>Keypad 0</kbd> |
| Option | <kbd>F2</kbd> |
| Select | <kbd>F3</kbd> |
| Start | <kbd>F4</kbd> |
| Reset | <kbd>F5</kbd> |
| Help | <kbd>F6</kbd> |
| Break | <kbd>F7</kbd> |

The other keys type on the Atari's keyboard. By default a key types the
character the host keyboard makes; Preferences ▸ Input ▸ Keys switches to the
Atari's own key positions. <kbd>Ctrl</kbd> plus a letter stays the Atari's
CONTROL.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Shift+F5</kbd> | Power Cycle |
| <kbd>F9</kbd> | Controllers |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |
| <kbd>Ctrl+,</kbd> | Preferences |
| <kbd>Ctrl+Shift+O</kbd> | Open |
| <kbd>Ctrl+Shift+C</kbd> | Copy Screen Text |
| <kbd>Ctrl+Shift+V</kbd> | Paste |
| <kbd>Ctrl+Shift+Q</kbd> | Quit |
| <kbd>Ctrl+Shift+H</kbd> | Help |

<!-- only: macos -->

On a Mac the shortcuts use ⌘ instead: ⌘O, ⌘C, ⌘V, ⌘Q, ⌘D for the Debugger
and ⌘K for the Controllers window.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. The Controllers window assigns each to a controller port, and maps keys
and buttons. With **Gamepad sticks steer the joystick** on (Preferences ▸
Input), a gamepad's analog stick steers the joystick as its D-pad does. A
gamepad's buttons can be given the console keys and fire in the Controllers
window.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, then click a joystick control or a console key (Reset, Option,
Select, Start, Help, Break), then press the key or the gamepad button to give
it. The control's old key or button is taken from it. **Defaults** puts every
control back. The map is kept in the settings, and only what differs from the
defaults is written.

<!-- context: controllers -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) draws the Atari's keyboard for the
computer you chose (HELP on the XL/XE, F1 to F4 on the 1200XL), the
controller ports with what is plugged into each (a stick and fire for a
joystick, two dials for paddles), the gamepads and their ports, and Map mode.
It floats on its own at a fixed size. Whatever the Atari sees held lights up
in the accent colour, whoever holds it: the host keyboard, a gamepad or the
mouse, so the window is also an input tester for a new gamepad.

| With the mouse | Does |
|---|---|
| Press a key | Holds it for as long as the button is down |
| Press Shift or Control | Latches it for the next key |
| Press a stick control | Holds it |
| Press a console key | Holds it |

The Atari reads its keyboard and sticks once a frame, so a key is held for as
long as the mouse button is, as a real one is. Switching to another window
lets go of everything the mouse holds. Keys typed in the Controllers window go
to the Atari. **Map** and **Defaults** change the controls (see
[Changing the controls](#changing-the-controls)).

# Machines

Preferences ▸ Machine ▸ Model picks the computer:

| Model | Is |
|---|---|
| Atari 400 | 8 to 48 KB, 400/800 OS, four controller ports |
| Atari 800 | 8 to 52 KB, 400/800 OS, four controller ports |
| Atari 1200XL | 64 KB, the 1200XL OS and its function keys |
| Atari 600XL | 16 or 64 KB, built-in BASIC |
| Atari 800XL | 64 KB, built-in BASIC |
| Atari 65XE | 64 KB, built-in BASIC |
| Atari 130XE | 128 KB, built-in BASIC (the default) |
| Atari 320XE (Compy Shop) | 320 KB |
| Atari 320XE (Rambo) | 320 KB |
| Atari 576XE | 576 KB |
| Atari 1088XE | 1088 KB |
| Atari XEGS | 64 KB game system, with or without its keyboard |
| Atari 1400XL | 64 KB, with the voice and modem ROM |
| Atari 1450XLD | 64 KB, with the voice ROM and its disk drive |

The 5200 is a separate app. A new model takes effect with a power cycle when
Preferences closes.

# System ROMs

None are needed: AltirraOS and Altirra BASIC are built in, and run CONFIG and
nearly every program. The ROMs of the real machines can be imported, and the
Atari revisions then appear in Preferences ▸ Machine ▸ *Operating system* and
*BASIC*. The ROMs are copyrighted, so none is part of the app.

- **Import System ROMs…** (in the Machine menu and on the ROMs page of
  Preferences) takes a ROM image, a zip, or MAME's ROM sets.
- Dropping a ROM file or zip on the window, or choosing it with Open,
  imports it too.

Files are recognised by their size and checksum, whatever they are called.
The message says what was imported. Imported files are copied into the app's
`roms` folder (see [Files and folders](#files-and-folders)), so the originals
can go. A new ROM takes effect at the next power cycle.

The ROM slots, by group:

- **400/800 OS**: rev. A (NTSC), rev. A (PAL), rev. B (NTSC), and a custom OS.
- **XL/XE OS**: the 1200XL revs. 10 and 11, the 600XL rev. 1, the 800XL
  rev. 2, the 65XE/130XE rev. 3, the XEGS rev. 4, the 1450XLD rev. 3 and 3 v4,
  rev. 5 (from source), the Arabic 65XE revs. 59 and 59 alt., and a custom OS.
- **BASIC**: Atari BASIC revs. A, B and C, and a custom BASIC.
- **XEGS**: the Missile Command game, and a custom game.
- **Peripherals**: the 1400XL/1450XLD voice and modem ROM, the 1450XLD disk
  ROM, the ICD MIO, CSS Black Box, Proto80, Austin Franklin 80 and Bit3
  Full-View 80 ROMs, their character sets, and the XEP80 character set.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, on the Atari's
SIO port. The Atari and FujiNet talk NetSIO over UDP: atari800 is the hub and
listens, and FujiNet connects.

## CONFIG

CONFIG is FujiNet's program for choosing what to run:

- **Hosts** are where disks come from: the SD folder, and servers on the
  network.
- Pick a host and a disk on it, mount it in a slot, and boot.
- **Reboot to CONFIG** (Machine menu) brings CONFIG back.

## The FujiNet menu

- **FujiNet Web UI** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11515/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).
- **Restart FujiNet** restarts the built-in FujiNet.

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom. It is also kept in
`fujinet-console.log` in the app's `fujinet` folder.

## Turning it off

Preferences ▸ FujiNet ▸ **FujiNet** turns the built-in FujiNet off. The
drives are then atari800's own.

## An external FujiNet

Preferences ▸ FujiNet ▸ **Use external FujiNet** skips the built-in FujiNet
and makes the Atari listen on the address you give, for example
`0.0.0.0:11514`; point the other FujiNet instance's NetSIO host and port at
it. `0.0.0.0` accepts connections from the network, `127.0.0.1` only from this
computer. FujiNet Web UI and Console Log belong to the built-in FujiNet, so
they say the FujiNet is external; use the other instance's own. The
Debugger's FujiNet tab still shows the traffic.

## Behind the scenes

- NetSIO is on UDP port 11514, and the web UI on port 11515, loopback only.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder. It is
  written once and then left for FujiNet and you.

# Preferences

Preferences (<kbd>Ctrl+,</kbd>) set up the computer. Each setting says when
it takes effect: at once, with a power cycle (done when Preferences closes,
once for all of them), or by restarting the session. The settings are one
file, shared by the GNOME and KDE apps on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (in the app menu), and its
pages are tabs.

<!-- end -->

<!-- only: windows -->

On Windows this window is called **Settings** (in the Machine menu).

<!-- end -->

The pages are Machine, Peripherals, Video, Audio, Input, FujiNet and ROMs.

<!-- context: prefs/Machine -->
## Machine settings

The computer, its memory and ROMs, and how fast it loads. These apply together with one power cycle when Preferences closes, except where noted.


### Computer

- **Model**: which computer (see [Machines](#machines)) (setting `machine`)
- **Memory**: how much memory; the choices depend on the model (setting `ram`)
- **TV system**: *NTSC* (60 Hz) or *PAL* (50 Hz) (setting `tv`)
- **BASIC**: start in BASIC. Off holds OPTION at power-on, as FujiNet's CONFIG wants (setting `basic`)

### Choosing the ROMs

- **Operating system**: which operating system ROM. *Automatic* picks one that is imported for the model, else AltirraOS, which is built in; the Atari revisions are listed once imported (see [System ROMs](#system-roms)). *Custom* uses a ROM you imported that matched no revision (setting `os_rev`)
- **BASIC**: which BASIC ROM, the same way: *Altirra BASIC (built in)*, or Atari BASIC rev. A, B or C once imported, or *Custom* (setting `basic_rev`)

### XEGS

- **Keyboard attached**: whether the XEGS has its keyboard plugged in (setting `xegs_keyboard`)

### 1200XL

- **J1 jumper fitted**: disables the 1200XL's LEDs and changes the OS's keyboard handling (setting `jumper_1200`)

### Memory expansions

- **Axlon RAM banks**: Axlon RAM banks, 0 for none. 400/800 only (setting `axlon_banks`)
- **Mosaic RAM banks**: Mosaic RAM banks, 0 for none. 400/800 only (setting `mosaic_banks`)
- **MapRAM**: MapRAM: RAM under the XL/XE OS. XL/XE only (setting `mapram`)

### Speed

- **Fast disk access**: patches the OS's SIO routine so disk access is fast. For the internal drives only, not FujiNet (setting `sio_patch`)
- **Load programs at disk speed**: loads programs at the disk's real speed (setting `slow_xex`)

<!-- context: prefs/Peripherals -->
## Peripherals settings

What is plugged into the computer. These apply with a power cycle when Preferences closes, except the R-Time 8 and the H: devices, which apply at once.


### 80-column display

- **Card**: an 80-column card: *None*, *XEP80 (joystick port 2)*, *Austin Franklin 80*, *Bit3 Full-View 80* or *Proto80*. View ▸ 80-Column Display shows it (setting `col80`)
- **XEP80 port**: which joystick port the XEP80 is on, *Port 1* or *Port 2* (setting `xep80_port`)

### Parallel bus

- **Device**: a parallel-bus device: *None*, *ICD MIO* or *CSS Black Box*. XL/XE only (setting `pbi`)
- **SCSI hard disk image**: the SCSI hard disk image the MIO or Black Box uses (setting `pbi_disk`)

### Speech

- **Voice Box**: a speech box: *None*, *Alien Group Voice Box* or *Voice Box II* (setting `voicebox`)

### Hard disk

- **IDE image (MyIDE)**: an IDE hard disk image for the MyIDE interface (setting `ide_image`)
- **CompactFlash**: treats the IDE image as a CompactFlash card (setting `ide_cf`)

### Clock

- **R-Time 8 cartridge**: the R-Time 8 clock cartridge (setting `rtime`)

### H: devices

- **H1:**: the folder the Atari sees as H1: (setting `h1_dir`)
- **H2:**: the folder the Atari sees as H2: (setting `h2_dir`)
- **H3:**: the folder the Atari sees as H3: (setting `h3_dir`)
- **H4:**: the folder the Atari sees as H4: (setting `h4_dir`)
- **Read only**: the H: devices cannot be written to (setting `h_read_only`)

<!-- context: prefs/Video -->
## Video settings

The picture. Everything here applies at once.


### Colour

- **NTSC artifacting**: NTSC artifact colour: *Off*, *Artifacts (old)*, *Artifacts (new)* or the *NTSC filter* (setting `artifact_ntsc`)
- **PAL blending**: PAL colour blending: *Off*, *Simple blending* or *Accurate blending* (setting `artifact_pal`)
- **Palette**: the colour palette: *Standard*, *Deep black* or *Vibrant* (setting `palette_preset`)
- **Saturation**: colour saturation, -100 to 100 (setting `saturation`)
- **Contrast**: contrast, -100 to 100 (setting `contrast`)
- **Brightness**: brightness, -100 to 100 (setting `brightness`)
- **Gamma**: gamma, -100 to 100 (setting `gamma`)
- **Tint**: tint (hue), -100 to 100 (setting `hue`)
- **External palette (.act)**: an external palette, an `.act` file, in place of the preset (setting `palette_file`)

### NTSC filter

- **Signal**: the NTSC filter's signal: *Composite*, *S-Video*, *RGB* or *Monochrome* (setting `ntsc_preset`)
- **Sharpness**: the NTSC filter's sharpness, -100 to 100 (setting `ntsc_sharpness`)
- **Resolution**: the NTSC filter's resolution, -100 to 100 (setting `ntsc_resolution`)
- **Artifacts**: the NTSC filter's artifacts, -100 to 100 (setting `ntsc_artifacts`)
- **Fringing**: the NTSC filter's colour fringing, -100 to 100 (setting `ntsc_fringing`)
- **Bleed**: the NTSC filter's colour bleed, -100 to 100 (setting `ntsc_bleed`)

### Picture

- **Visible area**: how much of the picture shows: *Narrow (320 × 200)*, *TV (336 × 224/240)* or *Overscan (384 × 240)* (setting `area`)
- **Aspect**: *TV (4:3)* or *Square pixels* (setting `aspect`)
- **Smooth scaling**: blends the pixels when the picture is stretched; off keeps them sharp (setting `smooth`)

<!-- context: prefs/Audio -->
## Audio settings

The sound. The volume and speaker apply at once; the POKEY options with a power cycle.


### Sound

- **Volume**: the output level, 0 to 100 (setting `volume`)
- **Stereo POKEY**: a second POKEY at $D210, for stereo (setting `stereo`)
- **Console speaker**: the console speaker: key clicks and the 400/800's buzzer (setting `speaker`)
- **High-fidelity POKEY**: the high-fidelity POKEY sound (setting `mz_pokey`)

<!-- context: prefs/Input -->
## Input settings

What is in the controller ports, and how the sticks and the keyboard behave. Applies at once.


### Controller ports

- **Port 1**: what is plugged into controller port 1 (see the devices below) (setting `port1_device`)
- **Port 2**: what is plugged into controller port 2 (setting `port2_device`)
- **Port 3**: what is plugged into controller port 3. 400/800 only (setting `port3_device`)
- **Port 4**: what is plugged into controller port 4. 400/800 only (setting `port4_device`)
- **MultiJoy4 interface**: the MultiJoy4 interface: four joysticks on an XL/XE (setting `multijoy`)

### Joysticks

- **Autofire**: *Off*, *While fire is held* or *Always* (setting `autofire`)
- **Gamepad sticks steer the joystick**: a gamepad's analog stick steers the joystick, as the D-pad does (setting `analog_stick`)

### Mice and paddles

- **Speed**: how fast a mouse or paddle moves, 1 to 9 (setting `mouse_speed`)

### Keyboard

- **Keys**: *Characters (type what you see)* sends the character the host key makes; *Positions (like the Atari's own keys)* sends the Atari key in the same place (setting `kbd_mode`)

The devices a controller port can hold: *Joystick*, *Paddles*, *Koala Pad*,
*Atari ST mouse*, *Amiga mouse*, *Trak-Ball (CX22)*, *Light pen*, *Light gun
(XG-1)*, *Touch tablet (CX77)*, *Numeric keypad (CX85)* and *Nothing*. A mouse
or a paddle follows the host mouse in the picture.

<!-- context: prefs/FujiNet -->
## FujiNet settings

FujiNet and the host's audio and gamepads. These restart the session when Preferences closes, except *Restart FujiNet on power cycle*.


### The built-in FujiNet

- **FujiNet**: the built-in FujiNet, on the Atari's SIO port (setting `enable_fujinet`)
- **Restart FujiNet on power cycle**: restarts FujiNet whenever the Atari is power-cycled, as unplugging both would (setting `fujinet_power`)

### External FujiNet

- **Use external FujiNet**: skips the built-in FujiNet and waits for another instance to connect (setting `fujinet_external`)
- **Listen address:port**: the address and port the Atari listens on for it. Set that instance's NetSIO host and port to this address. `0.0.0.0` accepts connections from the network; `127.0.0.1` only from this computer (setting `fujinet_external_addr`)

### Host

- **Sound output**: opens the system audio device (setting `enable_audio`)
- **Gamepads**: polls USB and Bluetooth gamepads (setting `enable_gamepad`)

<!-- context: prefs/ROMs -->
## The ROMs page

Preferences ▸ ROMs lists every ROM the app can use, *Imported* or *Not
imported*, in groups, with import buttons (see [System ROMs](#system-roms)).

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The keys are stored one per line, and a
setting that has its default is still written once changed.

# Debugger

The Debugger (<kbd>F12</kbd>) stops the computer and shows what it is doing.
It has ten tabs; closing it lets the computer go on as it was.

| Button | Does |
|---|---|
| Stop or Run (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a JSR, to the instruction after it |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Frame (<kbd>F9</kbd>) | On to the next frame |

Beside them, the Debugger says why the computer stopped: a step, a
breakpoint, a watchpoint, or a CIM (an illegal instruction that halts the
6502).

<!-- context: debugger/console -->
## Console

atari800's monitor, as a console, including the `A` assembler. Type `HELP` in
it for atari800's commands.

<!-- context: debugger/cpu -->
## CPU and memory

The 6502's registers and flags, an editable hex view of memory, and the
history of the last instructions. Click a value to edit it while stopped.

<!-- context: debugger/disassembly -->
## Disassembly

The 6502 disassembled at the PC, with the OS's labels, the opcode bytes and
the breakpoints in the margin. Click the left edge of a line for a
breakpoint. Symbol files (ld65 `-Ln`, VICE, MADS and atari800 formats) can be
loaded.

<!-- context: debugger/antic -->
## ANTIC

ANTIC's registers, the display list decoded line by line, and where the beam
is.

<!-- context: debugger/gtia -->
## GTIA

GTIA's registers, the colour palette as swatches, and the collisions.

<!-- context: debugger/pokey -->
## POKEY and PIA

POKEY's and PIA's registers, decoded.

<!-- context: debugger/map -->
## Memory map

The memory map: where RAM, ROM, the OS, BASIC, the cartridge and the I/O
chips are now, and what the cartridge is.

<!-- context: debugger/breakpoints -->
## Breakpoints

The list of breakpoints and watchpoints with their conditions (atari800's
table holds 20 conditions). **Add**, **Remove**, and the check box to turn one
off.

<!-- context: debugger/sio -->
## SIO

Every SIO command the Atari sends: the device, the command, the sector and
what came back.

<!-- context: debugger/fujinet -->
## FujiNet packets

Everything between the Atari and FujiNet as SIO packets (frame, ACK,
COMPLETE, data and checksums, with hex), recorded from start-up. **Clear**
empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-atari800/settings.ini` and
the data is in `~/.local/share/fujinet-go-atari800` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-atari800\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-atari800`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-atari800/settings.ini` and
the data is in `~/.local/share/fujinet-go-atari800`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported ROMs |
| `media` | disks, cartridges and tapes opened or dropped on the window |
| `states` | saved states |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`), and the console log |

# Troubleshooting

- **Cannot Start the Atari**: the message says what the chosen model or ROMs
  lack; choose *Automatic* for the operating system, or import the ROM.
- **A ROM was not taken**: ROMs are recognised by size and checksum; one that
  is not a known dump is refused.
- **Save State is greyed or refused**: states need FujiNet off.
- **A cartridge does nothing**: a raw image needs the right board; remove it
  and insert it again, choosing another.
- **The program does not load**: with FujiNet off, check the drive; with it
  on, the disk must be mounted from CONFIG or the SD card.
- **Keys type the wrong characters**: Preferences ▸ Input ▸ Keys switches
  between typing characters and the Atari's key positions.
- **A gamepad does nothing**: it may be on a port with no joystick, or its
  buttons given other jobs in the Controllers window.
- **No sound**: the audio device could not be opened (Preferences ▸ FujiNet ▸
  Sound output), or the volume is 0.
- **A port is taken**: another program is using 11514 or 11515. Close the
  other FujiNet, or choose **Use external FujiNet**.

# About and licences

FujiNet Go Atari 800 is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **atari800** (GPL-2.0-or-later) is the emulator.
- **AltirraOS and Altirra BASIC** (Avery Lee, freely redistributable) are
  built in.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app.
- No Atari ROM is part of the app; you import your own.

[fujinet.online](https://fujinet.online/) ·
[Report an issue](https://github.com/FujiNetWIFI/fujinet-go-atari800-desktop/issues)
