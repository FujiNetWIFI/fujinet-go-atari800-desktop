/* The atari800 core's side of the host bridge.

   platform.c implements atari800's PLATFORM_* hooks over this state; the
   C++ host (A800Host.cpp) fills it in on the emulation thread between
   frames. Nothing here is thread-safe: only the emulation thread touches it. */
#ifndef A8HOST_H
#define A8HOST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct a8host_callbacks {
	/* One frame of PCM: signed 16-bit native-endian, Sound_out.channels. */
	void (*sound)(void *user, const void *pcm, unsigned bytes);
	/* A break (breakpoint, step, BRK, CIM): park until the debugger
	   resumes. The CPU registers are already synced to CPU_reg*. */
	void (*brk)(void *user);
	/* Inside Atari800_Initialise, where atari800 would read its config
	   file (PLATFORM_ConfigInit): apply the host's options. */
	void (*config)(void *user);
	void *user;
} a8host_callbacks;

void a8host_set_callbacks(const a8host_callbacks *cb);

/* Joystick nibbles (INPUT_STICK_*, 0x0f centred) and triggers (1 released,
   0 pressed) for ports 1-4, and the eight POT lines (228 = released). */
extern unsigned char a8host_stick[4];
extern unsigned char a8host_trig[4];
extern unsigned char a8host_pot[8];

/* The rate the session wants (Sound_desired.freq is set from it). */
#define A8HOST_AUDIO_RATE 48000

#ifdef __cplusplus
}
#endif

#endif
