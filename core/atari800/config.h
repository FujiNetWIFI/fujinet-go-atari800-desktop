/* atari800 build configuration for FujiNet Go Atari 800.
   Hand-written in place of autoconf's: one file for Linux, macOS and
   MinGW-w64. See COMPLIANCE.md for what is enabled and why. */
#ifndef A800_CONFIG_H
#define A800_CONFIG_H

#define PACKAGE "atari800"
#define PACKAGE_NAME "Atari800"
#define PACKAGE_VERSION "7.2.1"
#define PACKAGE_STRING "Atari800 7.2.1"
#define PACKAGE_TARNAME "atari800"
#define PACKAGE_BUGREPORT "pstehlik@sophics.cz"
#define PACKAGE_URL ""
#define VERSION "7.2.1"

/* ---- embedding ---------------------------------------------------------- */
#define ATARI800_EMBEDDED 1
#define PLATFORM_POT 1
#define PLATFORM_MAP_PALETTE 1
#define SUPPORTS_CHANGE_VIDEOMODE 1
#define SUPPORTS_PLATFORM_PALETTEUPDATE 1
#define SUPPORTS_PLATFORM_SLEEP 1
#define SUPPORTS_PLATFORM_TIME 1
#define BUFFERED_LOG 1

/* ---- emulation ---------------------------------------------------------- */
#define NEW_CYCLE_EXACT 1
#define EMUOS_ALTIRRA 1
#define NETSIO 1
#define XEP80_EMULATION 1
#define NTSC_FILTER 1
#define PAL_BLENDING 1
#define AF80 1
#define BIT3 1
#define PBI_PROTO80 1
#define PBI_MIO 1
#define PBI_BB 1
#define PBI_XLD 1
#define VOICEBOX 1
#define IDE 1
#define R_IO_DEVICE 1
#define R_NETWORK 1

/* ---- sound -------------------------------------------------------------- */
#define SOUND 1
#define STEREO_SOUND 1
#define CONSOLE_SOUND 1
#define INTERPOLATE_SOUND 1
#define NONLINEAR_MIXING 1

/* ---- monitor (the debugger engine) -------------------------------------- */
#define MONITOR_BREAK 1
#define MONITOR_BREAKPOINTS 1
#define MONITOR_HINTS 1
#define MONITOR_ASSEMBLER 1
#define MONITOR_TRACE 1
#define MONITOR_EMBEDDED 1

/* ---- C library ---------------------------------------------------------- */
#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_ERRNO_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_TIME_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_FCNTL_H 1
#define HAVE_DIRENT_H 1
#define HAVE_UINTPTR_T 1
#define HAVE_LIBZ 1
#define HAVE_ATEXIT 1
#define HAVE_CHMOD 1
#define HAVE_CLOCK 1
#define HAVE_FDOPEN 1
#define HAVE_FFLUSH 1
#define HAVE_FLOOR 1
#define HAVE_FSTAT 1
#define HAVE_GETCWD 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_LOCALTIME 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_MKDIR 1
#define HAVE_MODF 1
#define HAVE_OPENDIR 1
#define HAVE_RENAME 1
#define HAVE_REWIND 1
#define HAVE_RMDIR 1
#define HAVE_SNPRINTF 1
#define HAVE_STAT 1
#define HAVE_STRCASECMP 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRNCPY 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRTOL 1
#define HAVE_TMPFILE 1
#define HAVE_UNLINK 1
#define HAVE_VPRINTF 1
#define HAVE_VSNPRINTF 1
#ifndef A800_DETERMINISTIC
/* seeds POKEY's RANDOM from the clock; tests build without it */
#define HAVE_TIME 1
#endif

#if defined(_WIN32)
#define HAVE_WINDOWS_H 1
#define HAVE_WINSOCK2_H 1
#define HAVE_DIRECT_H 1
#define DIR_SEP_BACKSLASH 1
#define DOS_DRIVES 1
#define HAVE_UNISTD_H 1      /* mingw-w64 has one */
#else
#define HAVE_UNISTD_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_ARPA_INET_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_LIBPTHREAD 1
#define HAVE_FSEEKO 1
#define HAVE_MKSTEMP 1
#define HAVE_NANOSLEEP 1
#define HAVE_USLEEP 1
#define HAVE_SELECT 1
#define HAVE_SOCKET 1
#define HAVE_GETHOSTBYNAME 1
#define HAVE_GETHOSTBYADDR 1
#define HAVE_INET_NTOA 1
#define SELECT_TYPE_ARG1 int
#define SELECT_TYPE_ARG234 (fd_set *)
#define SELECT_TYPE_ARG5 (struct timeval *)
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)
#define WORDS_UNALIGNED_OK 1
#endif

#endif /* A800_CONFIG_H */
