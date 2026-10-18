/* Menu command and resource IDs.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#define IDI_APPICON        101

#define IDM_OPEN           1001
#define IDM_POWER          1002
#define IDM_RESET          1003
#define IDM_REBOOT_CONFIG  1004
#define IDM_WARP           1005
#define IDM_STATE_SAVE     1006
#define IDM_STATE_LOAD     1007
#define IDM_IMPORT_ROMS    1008
#define IDM_SETTINGS       1009
#define IDM_EXIT           1010

#define IDM_IMPORT_SD      1020
#define IDM_SD_FOLDER      1021
#define IDM_MEDIA_FOLDER   1022

#define IDM_CART_INSERT    1030
#define IDM_CART_PIGGYBACK 1031
#define IDM_CART_REMOVE    1032
#define IDM_CART_REMOVE_PB 1033
#define IDM_CART_INFO      1034

#define IDM_TAPE_INSERT    1040
#define IDM_TAPE_RECORD    1041
#define IDM_TAPE_REWIND    1042
#define IDM_TAPE_BOOT      1043
#define IDM_TAPE_EJECT     1044
#define IDM_TAPE_INFO      1045

#define IDM_COPY_SCREEN    1050
#define IDM_PASTE          1051

#define IDM_CONTROLLERS    1060
#define IDM_DEBUGGER       1061
#define IDM_COL80          1062
#define IDM_TV_ASPECT      1063
#define IDM_SMOOTH         1064
#define IDM_FULLSCREEN     1065

#define IDM_FUJINET_CONFIG 1070
#define IDM_FUJINET_LOG    1071
#define IDM_FUJINET_RESTART 1072
#define IDM_ABOUT          1080

/* Ranges: + the drive (0-7); + drive * 4 + density; + the console key. */
#define IDM_DRIVE_INSERT   1100
#define IDM_DRIVE_EJECT    1110
#define IDM_DRIVE_PROTECT  1120
#define IDM_DRIVE_BLANK    1130
#define IDM_CONSOLE        1170

#define IDC_STATUSBAR      2200
#define IDC_LOG_EDIT       2100
#define IDT_LOG_REFRESH    1
#define IDT_STATUS         2
#define IDT_SYSACT         3
#define IDT_TAP            5
