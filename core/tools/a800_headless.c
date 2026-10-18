/* a800_headless: atari800 with no window, for the boot and A/B tests.

   a800_headless [options] [-- atari800 options and files]
     --frames N          run N frames (default 300)
     --hash              print one CRC32 over every frame's screen, and one
                         over all audio
     --screen-text       print the GR.0 text screen at the end
     --expect-text STR   exit 1 unless STR is on the text screen at the end
     --type STR          type STR (\n = Return) from frame 120
     --log               print atari800's log

   Built twice: against the patched core (a800_headless) and the stock one
   (a800_headless_stock), from the same source. */
#include "config.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "akey.h"
#include "atari.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "screen.h"
#include "sound.h"

#include "a8host.h"

static uint32_t crc_table[256];

static void crc_init(void)
{
	uint32_t i, j, c;
	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
	const uint8_t *p = data;
	crc = ~crc;
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static uint32_t audio_crc;
static unsigned long audio_bytes;

static void on_sound(void *user, const void *pcm, unsigned bytes)
{
	(void)user;
	audio_crc = crc32_update(audio_crc, pcm, bytes);
	audio_bytes += bytes;
}

static void on_break(void *user)
{
	(void)user;
}

/* Internal screen code -> printable ASCII (inverse dropped). */
static char screen_char(UBYTE c)
{
	c &= 0x7f;
	if (c < 0x40)
		c += 0x20;
	else if (c < 0x60)
		c -= 0x40;
	return c >= 0x20 && c < 0x7f ? (char)c : '.';
}

static void screen_text(char *out, size_t n)
{
	UWORD savmsc = MEMORY_dGetByte(0x58) | (MEMORY_dGetByte(0x59) << 8);
	size_t k = 0;
	int row, col;
	for (row = 0; row < 24 && k + 42 < n; row++) {
		for (col = 0; col < 40; col++)
			out[k++] = screen_char(MEMORY_dGetByte((UWORD)(savmsc + row * 40 + col)));
		while (k > 0 && out[k - 1] == ' ')
			k--;
		out[k++] = '\n';
	}
	out[k] = 0;
}

/* ATASCII -> AKEY for the --type subset (letters, digits, space, a few marks). */
static int akey_for(char ch)
{
	static const struct { char c; int k; } map[] = {
		{'a', AKEY_a}, {'b', AKEY_b}, {'c', AKEY_c}, {'d', AKEY_d}, {'e', AKEY_e},
		{'f', AKEY_f}, {'g', AKEY_g}, {'h', AKEY_h}, {'i', AKEY_i}, {'j', AKEY_j},
		{'k', AKEY_k}, {'l', AKEY_l}, {'m', AKEY_m}, {'n', AKEY_n}, {'o', AKEY_o},
		{'p', AKEY_p}, {'q', AKEY_q}, {'r', AKEY_r}, {'s', AKEY_s}, {'t', AKEY_t},
		{'u', AKEY_u}, {'v', AKEY_v}, {'w', AKEY_w}, {'x', AKEY_x}, {'y', AKEY_y},
		{'z', AKEY_z}, {'0', AKEY_0}, {'1', AKEY_1}, {'2', AKEY_2}, {'3', AKEY_3},
		{'4', AKEY_4}, {'5', AKEY_5}, {'6', AKEY_6}, {'7', AKEY_7}, {'8', AKEY_8},
		{'9', AKEY_9}, {' ', AKEY_SPACE}, {'\n', AKEY_RETURN}, {'+', AKEY_PLUS},
		{'-', AKEY_MINUS}, {'*', AKEY_ASTERISK}, {'=', AKEY_EQUAL},
		{'"', AKEY_DBLQUOTE}, {',', AKEY_COMMA}, {'.', AKEY_FULLSTOP},
		{';', AKEY_SEMICOLON}, {':', AKEY_COLON},
	};
	size_t i;
	if (ch >= 'A' && ch <= 'Z')
		return akey_for((char)(ch - 'A' + 'a')) | AKEY_SHFT;
	for (i = 0; i < sizeof map / sizeof map[0]; i++)
		if (map[i].c == ch)
			return map[i].k;
	return AKEY_NONE;
}

int main(int argc, char **argv)
{
	int frames = 300, want_hash = 0, want_text = 0, want_log = 0;
	const char *expect = NULL, *type = NULL;
	char *a8argv[64];
	int a8argc = 0, i, f;
	uint32_t screen_crc = 0;
	char text[24 * 42 + 1];
	a8host_callbacks cb = {0};
#ifdef A800_STOCK
	char cfg[FILENAME_MAX];
#endif

	a8argv[a8argc++] = "a800_headless";
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--hash"))
			want_hash = 1;
		else if (!strcmp(argv[i], "--screen-text"))
			want_text = 1;
		else if (!strcmp(argv[i], "--expect-text") && i + 1 < argc)
			expect = argv[++i];
		else if (!strcmp(argv[i], "--type") && i + 1 < argc) {
			/* "\n" in the argument means Return */
			char *t = strdup(argv[++i]), *r = t, *w = t;
			for (; *r; r++, w++) {
				if (r[0] == '\\' && r[1] == 'n') {
					*w = '\n';
					r++;
				}
				else
					*w = *r;
			}
			*w = 0;
			type = t;
		}
		else if (!strcmp(argv[i], "--log"))
			want_log = 1;
		else if (!strcmp(argv[i], "--")) {
			for (i++; i < argc && a8argc < 60; i++)
				a8argv[a8argc++] = argv[i];
		}
		else {
			fprintf(stderr, "a800_headless: unknown option %s\n", argv[i]);
			return 2;
		}
	}
#ifdef A800_STOCK
	/* stock atari800 reads and writes a config file: keep it out of $HOME */
	snprintf(cfg, sizeof cfg, "a800_headless_stock-%d.cfg", (int)getpid());
	{
		FILE *fp = fopen(cfg, "w");
		if (fp)
			fclose(fp);
	}
	a8argv[a8argc++] = "-config";
	a8argv[a8argc++] = cfg;
#else
	Atari800_host_paced = TRUE;
#endif
	a8argv[a8argc] = NULL;

	crc_init();
	cb.sound = on_sound;
	cb.brk = on_break;
	a8host_set_callbacks(&cb);
	Sound_desired.freq = A8HOST_AUDIO_RATE;
	Sound_latency = 0;

	if (!Atari800_Initialise(&a8argc, a8argv)) {
		fprintf(stderr, "a800_headless: Atari800_Initialise failed\n%s", Log_buffer);
		return 1;
	}
	for (f = 0; f < frames; f++) {
		INPUT_key_code = AKEY_NONE;
		if (type && f >= 120) {
			/* one key every 4 frames: down 2, up 2 */
			int idx = (f - 120) / 4;
			if (idx < (int)strlen(type) && (f - 120) % 4 < 2)
				INPUT_key_code = akey_for(type[idx]);
		}
		Atari800_Frame();
		if (want_hash)
			screen_crc = crc32_update(screen_crc, Screen_atari, Screen_WIDTH * Screen_HEIGHT);
		if (want_log && Log_buffer[0]) {
			fputs(Log_buffer, stderr);
			Log_buffer[0] = 0;
		}
	}
	screen_text(text, sizeof text);
	if (want_hash)
		printf("screen %08x audio %08x bytes %lu\n", screen_crc, audio_crc, audio_bytes);
	if (want_text)
		fputs(text, stdout);
#ifdef A800_STOCK
	remove(cfg);
#endif
	if (expect && !strstr(text, expect)) {
		fprintf(stderr, "a800_headless: \"%s\" not on screen:\n%s", expect, text);
		return 1;
	}
	return 0;
}
