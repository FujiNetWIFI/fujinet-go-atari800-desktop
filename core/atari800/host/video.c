/* The Atari's picture as XRGB8888: the palette, the NTSC filter, PAL
   blending and the 80-column cards (after atari800's sdl/video_sw.c). */
#include "config.h"
#include <string.h>

#include "a8video.h"
#include "af80.h"
#include "artifact.h"
#include "atari.h"
#include "bit3.h"
#include "colours.h"
#include "filter_ntsc.h"
#include "pal_blending.h"
#include "pbi_proto80.h"
#include "screen.h"
#include "videomode.h"
#include "xep80.h"

static uint32_t lut[256];
/* the last render's crop, for a8video_to_screen */
static int last_x0, last_y0 = 8, last_srcw = 336, last_outw = 336, last_card;
static int pal_cache[256];
static int have_lut;
static unsigned frame_no;

static void update_lut(void)
{
	int i;
	if (have_lut && !memcmp(pal_cache, Colours_table, sizeof pal_cache))
		return;
	memcpy(pal_cache, Colours_table, sizeof pal_cache);
	for (i = 0; i < 256; i++)
		lut[i] = 0xff000000u | (uint32_t)(Colours_table[i] & 0xffffff);
#ifdef PAL_BLENDING
	PAL_BLENDING_UpdateLookup();
#endif
	have_lut = 1;
}

void a8video_invalidate(void)
{
	have_lut = 0;
}

uint32_t a8video_colour(int index)
{
	update_lut();
	return lut[index & 0xff];
}

int a8video_80col_card(void)
{
#ifdef XEP80_EMULATION
	if (XEP80_enabled)
		return A8VIDEO_XEP80;
#endif
#ifdef PBI_PROTO80
	if (PBI_PROTO80_enabled)
		return A8VIDEO_PROTO80;
#endif
#ifdef AF80
	if (AF80_enabled)
		return A8VIDEO_AF80;
#endif
#ifdef BIT3
	if (BIT3_enabled)
		return A8VIDEO_BIT3;
#endif
	return A8VIDEO_NONE;
}

/* 8 pixels a byte, bit order per card, two colours or a palette index. */
typedef UBYTE (*pixels_fn)(int line, int column, int *colour, int blink);

#ifdef PBI_PROTO80
static UBYTE proto80_pixels(int line, int column, int *colour, int blink)
{
	(void)blink;
	*colour = 15;
	return PBI_PROTO80_GetPixels(line, column);
}
#endif

static void blit_cells(uint32_t *dst, int w, int h, pixels_fn fn, int msb_first)
{
	int blink = (frame_no % 60) >= 30;
	int line, col, i;
	uint32_t black = lut[0];
	for (line = 0; line < h; line++) {
		uint32_t *p = dst + (size_t)line * w;
		for (col = 0; col < w / 8; col++) {
			int colour = 15;
			UBYTE px = fn(line, col, &colour, blink);
			uint32_t on = lut[colour & 0xff];
			for (i = 0; i < 8; i++) {
				int bit = msb_first ? (px & 0x80) : (px & 0x01);
				*p++ = bit ? on : black;
				px = msb_first ? (UBYTE)(px << 1) : (UBYTE)(px >> 1);
			}
		}
	}
}

int a8video_render(uint32_t *dst, int maxw, int maxh, int *w_out, int *h_out,
                   int *par_out, const a8video_opts *o)
{
	int card = o->show_80col ? a8video_80col_card() : A8VIDEO_NONE;
	int x0, w, y0, h, x, y;

	frame_no++;
	update_lut();
	last_card = card;

	switch (card) {
#ifdef XEP80_EMULATION
	case A8VIDEO_XEP80: {
		const UBYTE *src = (frame_no % 60) >= 30 ? XEP80_screen_1 : XEP80_screen_2;
		w = XEP80_SCRN_WIDTH;
		h = XEP80_scrn_height;
		if (w > maxw || h > maxh)
			return 0;
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				dst[(size_t)y * w + x] = lut[src[(size_t)y * XEP80_SCRN_WIDTH + x]];
		*par_out = 500;
		goto done;
	}
#endif
#ifdef PBI_PROTO80
	case A8VIDEO_PROTO80:
		w = 640;
		h = 192;
		blit_cells(dst, w, h, proto80_pixels, 1);
		*par_out = 500;
		goto done;
#endif
#ifdef AF80
	case A8VIDEO_AF80:
		w = 640;
		h = 250;
		blit_cells(dst, w, h, AF80_GetPixels, 0);
		*par_out = 500;
		goto done;
#endif
#ifdef BIT3
	case A8VIDEO_BIT3:
		w = 640;
		h = 240;
		blit_cells(dst, w, h, BIT3_GetPixels, 0);
		*par_out = 500;
		goto done;
#endif
	default:
		break;
	}

	/* the Atari's own picture: 384x240 of colour indices, cropped */
	switch (o->area) {
	case A8VIDEO_AREA_NARROW:
		x0 = 32, w = 320, y0 = 20, h = 200;
		break;
	case A8VIDEO_AREA_FULL:
		x0 = 0, w = Screen_WIDTH, y0 = 0, h = Screen_HEIGHT;
		break;
	default:
		x0 = Screen_visible_x1, w = Screen_visible_x2 - Screen_visible_x1;
		if (Atari800_tv_mode == Atari800_TV_PAL)
			y0 = 0, h = Screen_HEIGHT;
		else
			y0 = 8, h = 224;
		break;
	}
	last_x0 = x0;
	last_y0 = y0;
	last_srcw = w;
	last_outw = w;
	{
		const UBYTE *src = (const UBYTE *)Screen_atari + (size_t)y0 * Screen_WIDTH + x0;
#ifdef NTSC_FILTER
		if (ARTIFACT_mode == ARTIFACT_NTSC_FULL && FILTER_NTSC_emu != NULL) {
			int ow = ATARI_NTSC_OUT_WIDTH(w);
			if (ow > maxw || h > maxh)
				return 0;
			atari_ntsc_blit_argb32(FILTER_NTSC_emu, (ATARI_NTSC_IN_T const *)src,
			                       Screen_WIDTH, w, h, dst, ow * 4);
			for (y = 0; y < h; y++)
				for (x = 0; x < ow; x++)
					dst[(size_t)y * ow + x] |= 0xff000000u;
			*par_out = (int)(1000.0 * w / ow + 0.5);
			last_outw = ow;
			w = ow;
			goto done;
		}
#endif
		if (w > maxw || h > maxh)
			return 0;
#ifdef PAL_BLENDING
		if (ARTIFACT_mode == ARTIFACT_PAL_BLEND) {
			PAL_BLENDING_Blit32((ULONG *)dst, (UBYTE *)src, w, w, h, y0 % 2);
			*par_out = 1000;
			goto done;
		}
#endif
		for (y = 0; y < h; y++) {
			const UBYTE *s = src + (size_t)y * Screen_WIDTH;
			uint32_t *d = dst + (size_t)y * w;
			for (x = 0; x < w; x++)
				d[x] = lut[s[x]];
		}
		*par_out = 1000;
	}
done:
	*w_out = w;
	*h_out = h;
	return 1;
}

int a8video_to_screen(int fx, int fy, int *sx, int *sy)
{
	if (last_card != A8VIDEO_NONE || last_outw <= 0)
		return 0;
	*sx = last_x0 + fx * last_srcw / last_outw;
	*sy = last_y0 + fy;
	return 1;
}
