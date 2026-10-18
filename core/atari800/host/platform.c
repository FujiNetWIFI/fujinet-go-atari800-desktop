/* atari800 PLATFORM_* hooks for FujiNet Go Atari 800: no window, no audio
   device, no input device -- the session owns those (see a8host.h). */
#include "config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "akey.h"
#include "atari.h"
#include "colours.h"
#include "input.h"
#include "platform.h"
#include "sound.h"
#include "util.h"
#include "videomode.h"

#include "a8host.h"

unsigned char a8host_stick[4] = {INPUT_STICK_CENTRE, INPUT_STICK_CENTRE,
                                 INPUT_STICK_CENTRE, INPUT_STICK_CENTRE};
unsigned char a8host_trig[4] = {1, 1, 1, 1};
unsigned char a8host_pot[8] = {228, 228, 228, 228, 228, 228, 228, 228};

static a8host_callbacks cbs;

void a8host_set_callbacks(const a8host_callbacks *cb)
{
	if (cb)
		cbs = *cb;
	else
		memset(&cbs, 0, sizeof cbs);
}

int PLATFORM_Initialise(int *argc, char *argv[])
{
	(void)argc;
	(void)argv;
	return TRUE;
}

/* atari800 calls this with run_monitor TRUE to break into the monitor (a
   breakpoint, a step, BRK, a CIM); 0 would make it tear down and exit(0). */
int PLATFORM_Exit(int run_monitor)
{
	if (run_monitor) {
		if (cbs.brk)
			cbs.brk(cbs.user);
		return 1;
	}
	return 0;
}

int PLATFORM_Keyboard(void) { return AKEY_NONE; }
void PLATFORM_DisplayScreen(void) {}

int PLATFORM_PORT(int num)
{
	if (num == 0)
		return (a8host_stick[1] << 4) | (a8host_stick[0] & 0x0f);
	if (num == 1)
		return (a8host_stick[3] << 4) | (a8host_stick[2] & 0x0f);
	return 0xff;
}

int PLATFORM_TRIG(int num)
{
	return num >= 0 && num < 4 ? a8host_trig[num] : 1;
}

#ifndef Atari_POT /* a macro returning 228 in unpatched atari800 */
int Atari_POT(int num)
{
	return num >= 0 && num < 8 ? a8host_pot[num] : 228;
}
#endif

void PLATFORM_PaletteUpdate(void) {}

void PLATFORM_ConfigInit(void)
{
	if (cbs.config)
		cbs.config(cbs.user);
}

double PLATFORM_Time(void)
{
#ifdef _WIN32
	static LARGE_INTEGER freq;
	LARGE_INTEGER now;
	if (freq.QuadPart == 0)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart / (double)freq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

void PLATFORM_Sleep(double s)
{
#ifdef A800_STOCK
	/* unpatched atari800 paces itself; the A/B tests run flat out */
	(void)s;
	return;
#endif
	if (s <= 0.0)
		return;
#ifdef _WIN32
	Sleep((DWORD)(s * 1000.0 + 0.5));
#else
	{
		struct timespec ts;
		ts.tv_sec = (time_t)s;
		ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
		nanosleep(&ts, NULL);
	}
#endif
}

/* ---- video modes: the frontends scale; atari800 only tracks 80-column ---- */

int PLATFORM_SupportsVideomode(VIDEOMODE_MODE_t mode, int stretch, int rotate90)
{
	(void)mode;
	(void)stretch;
	return !rotate90;
}

void PLATFORM_SetVideoMode(VIDEOMODE_resolution_t const *res, int windowed,
                           VIDEOMODE_MODE_t mode, int rotate90)
{
	(void)res;
	(void)windowed;
	(void)mode;
	(void)rotate90;
}

VIDEOMODE_resolution_t *PLATFORM_AvailableResolutions(unsigned int *size)
{
	VIDEOMODE_resolution_t *r = Util_malloc(sizeof *r);
	r->width = 1344;
	r->height = 960;
	*size = 1;
	return r;
}

VIDEOMODE_resolution_t *PLATFORM_DesktopResolution(void)
{
	static VIDEOMODE_resolution_t r = {1344, 960};
	return &r;
}

int PLATFORM_WindowMaximised(void) { return FALSE; }

void PLATFORM_GetPixelFormat(PLATFORM_pixel_format_t *format)
{
	format->bpp = 32;
	format->rmask = 0x00ff0000;
	format->gmask = 0x0000ff00;
	format->bmask = 0x000000ff;
}

void PLATFORM_MapRGB(void *dest, int const *palette, int size)
{
	ULONG *d = (ULONG *)dest;
	int i;
	for (i = 0; i < size; i++)
		d[i] = 0xff000000u | (ULONG)(palette[i] & 0xffffff);
}

/* ---- sound: exactly one frame of samples per Atari800_Frame (push) ------ */

static unsigned int frame_bytes;
static double sample_diff, sample_residual;

int PLATFORM_SoundSetup(Sound_setup_t *setup)
{
	double fps = Atari800_tv_mode == Atari800_TV_PAL ? Atari800_FPS_PAL
	                                                 : Atari800_FPS_NTSC;
	double per_frame = setup->freq / fps;
	setup->sample_size = 2;
	setup->buffer_frames = (unsigned int)ceil(per_frame);
	frame_bytes = setup->buffer_frames * setup->sample_size * setup->channels;
	sample_diff = (double)setup->buffer_frames - per_frame;
	sample_residual = 0.0;
	return frame_bytes != 0;
}

void PLATFORM_SoundExit(void) {}
void PLATFORM_SoundPause(void) {}
void PLATFORM_SoundContinue(void) {}

/* Mostly ceil(rate/fps) frames, one fewer often enough to average the
   fractional rate (as libatari800 does). */
unsigned int PLATFORM_SoundAvailable(void)
{
	unsigned int n = frame_bytes;
	sample_residual += sample_diff;
	if (sample_residual > 1.0) {
		sample_residual -= 1.0;
		n -= Sound_out.sample_size * Sound_out.channels;
	}
	return n;
}

void PLATFORM_SoundWrite(UBYTE const *buffer, unsigned int size)
{
	if (cbs.sound)
		cbs.sound(cbs.user, buffer, size);
}
