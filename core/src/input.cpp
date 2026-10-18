/*
 * input.cpp -- what is plugged into the controller ports (joysticks,
 * paddles, mice, the Koala Pad, light pen and gun, the CX85 keypad), the
 * on-screen keyboard, and pasting text as keystrokes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "keymap.h"
#include "session_impl.h"

extern "C" {
#include "config.h"
#include "akey.h"
#include "antic.h"
#include "atari.h"
#include "input.h"
#include "memory.h"
#include "pokey.h"
#include "a8host.h"
#include "a8video.h"
}

// ---------------------------------------------------------------------------
// controller ports
// ---------------------------------------------------------------------------

static const char* const device_names[A800_DEV_COUNT] = {
	"Joystick", "Paddles", "Koala Pad", "Atari ST mouse", "Amiga mouse",
	"Trak-Ball (CX22)", "Light pen", "Light gun (XG-1)", "Touch tablet (CX77)",
	"Numeric keypad (CX85)", "Nothing",
};

const char* a800_device_name(int d)
{
	return d >= 0 && d < A800_DEV_COUNT ? device_names[d] : "";
}

static bool is_mouse_device(int d)
{
	return d == A800_DEV_KOALA || d == A800_DEV_ST_MOUSE || d == A800_DEV_AMIGA_MOUSE
	       || d == A800_DEV_TRAKBALL || d == A800_DEV_LIGHTPEN || d == A800_DEV_LIGHTGUN
	       || d == A800_DEV_TOUCH_TABLET;
}

int SessionImpl::PortDevice(int port)
{
	return port >= 0 && port < A800_PORTS ? portDevice[port].load() : A800_DEV_NONE;
}

int a800session_port_device(a800session* s, int port)
{
	return Impl(s)->PortDevice(port);
}

// Emulation thread: the devices from the settings.
void SessionImpl::ApplyPorts()
{
	bool xl = a800_machine_is_xl(prefs_machine(s)) && !a800session_get_int(s, "multijoy", 0);
	for (int p = 0; p < A800_PORTS; p++) {
		char key[16];
		snprintf(key, sizeof key, "port%d_device", p + 1);
		int d = a800session_get_int(s, key, A800_DEV_JOYSTICK);
		if (d < 0 || d >= A800_DEV_COUNT || (xl && p >= 2))
			d = A800_DEV_NONE;
		portDevice[p].store(d);
	}
	mouseSpeed.store(a800session_get_int(s, "mouse_speed", 3));
	fkeys1200.store(Atari800_f_keys != 0);

	INPUT_mouse_mode = INPUT_MOUSE_OFF;
	INPUT_cx85 = 0;
	for (int p = 0; p < A800_PORTS; p++) {
		int d = portDevice[p].load();
		if (d == A800_DEV_CX85)
			INPUT_cx85 = 1;
		if (!is_mouse_device(d) || INPUT_mouse_mode != INPUT_MOUSE_OFF)
			continue;
		static const int modes[A800_DEV_COUNT] = {
			INPUT_MOUSE_OFF, INPUT_MOUSE_OFF, INPUT_MOUSE_KOALA, INPUT_MOUSE_ST,
			INPUT_MOUSE_AMIGA, INPUT_MOUSE_TRAK, INPUT_MOUSE_PEN, INPUT_MOUSE_GUN,
			INPUT_MOUSE_TOUCH, INPUT_MOUSE_OFF, INPUT_MOUSE_OFF,
		};
		INPUT_mouse_mode = modes[d];
		INPUT_mouse_port = p;
	}
	INPUT_mouse_speed = mouseSpeed.load();
}

// Emulation thread, before each frame (after the joystick nibbles are set).
void SessionImpl::DevicesBeforeFrame()
{
	unsigned buttons = ptrButtons.load();
	bool direct = false;
	for (int p = 0; p < A800_PORTS; p++) {
		int d = portDevice[p].load();
		if (d == A800_DEV_PADDLES || d == A800_DEV_KOALA || d == A800_DEV_TOUCH_TABLET)
			direct = true;
		if (d == A800_DEV_PADDLES) {
			unsigned f = paddleFire.load() >> (p * 2);
			// the paddles' triggers are the stick's left and right lines
			unsigned char stick = INPUT_STICK_CENTRE;
			if (f & 1)
				stick &= ~4;
			if (f & 2)
				stick &= ~8;
			a8host_stick[p] = stick;
			a8host_trig[p] = 1;
		}
		else if (d == A800_DEV_NONE || d == A800_DEV_CX85 || is_mouse_device(d)) {
			a8host_stick[p] = INPUT_STICK_CENTRE;
			a8host_trig[p] = 1;
		}
	}
	for (int i = 0; i < 8; i++)
		a8host_pot[i] = (unsigned char)std::max(0, std::min(228, pot[i].load()));

	INPUT_direct_mouse = direct ? 1 : 0;
	if (direct)
		for (int i = 0; i < 8; i++)
			POKEY_POT_input[i] = a8host_pot[i];

	INPUT_mouse_buttons = (int)(buttons & 7);
	INPUT_mouse_delta_x = ptrDx.exchange(0);
	INPUT_mouse_delta_y = ptrDy.exchange(0);
	if (INPUT_mouse_mode == INPUT_MOUSE_OFF)
		return;

	// absolute devices follow the pointer over the picture
	int sx, sy;
	bool inside = ptrInside.load() && a8video_to_screen(ptrX.load(), ptrY.load(), &sx, &sy);
	int port = INPUT_mouse_port;
	switch (INPUT_mouse_mode) {
	case INPUT_MOUSE_KOALA:
	case INPUT_MOUSE_TOUCH:
		if (inside) {
			// the pad's 228 x 228 surface over the 320 x 192 playfield
			int x = (sx - 32) * 228 / 320, y = (sy - 24) * 228 / 192;
			POKEY_POT_input[port * 2] = (UBYTE)std::max(1, std::min(228, x));
			POKEY_POT_input[port * 2 + 1] = (UBYTE)std::max(1, std::min(228, y));
		}
		else {
			POKEY_POT_input[port * 2] = POKEY_POT_input[port * 2 + 1] = 228;
		}
		INPUT_mouse_delta_x = INPUT_mouse_delta_y = 0;
		break;
	case INPUT_MOUSE_PEN:
	case INPUT_MOUSE_GUN:
		if (inside) {
			ANTIC_PENH_input = (UBYTE)(44 + INPUT_mouse_pen_ofs_h + std::max(0, (sx - 24) / 2));
			ANTIC_PENV_input = (UBYTE)(4 + INPUT_mouse_pen_ofs_v + std::max(0, sy / 2));
		}
		INPUT_mouse_delta_x = INPUT_mouse_delta_y = 0;
		// atari800 then reads the trigger from the buttons
		break;
	default:
		break;
	}
}

void a800session_paddle(a800session* s, int port, int paddle, int value, int fire)
{
	SessionImpl* im = Impl(s);
	if (port < 0 || port >= A800_PORTS || paddle < 0 || paddle > 1)
		return;
	int line = port * 2 + paddle;
	im->pot[line].store(std::max(0, std::min(228, value)));
	if (fire)
		im->paddleFire.fetch_or(1u << line);
	else
		im->paddleFire.fetch_and(~(1u << line));
}

void a800session_pointer(a800session* s, int x, int y, int inside, unsigned buttons)
{
	SessionImpl* im = Impl(s);
	im->ptrX.store(x);
	im->ptrY.store(y);
	im->ptrInside.store(inside);
	im->ptrButtons.store(buttons);
}

void a800session_pointer_motion(a800session* s, int dx, int dy, unsigned buttons)
{
	SessionImpl* im = Impl(s);
	im->ptrDx.fetch_add(dx);
	im->ptrDy.fetch_add(dy);
	im->ptrButtons.store(buttons);
	// paddles follow the mouse too: left-right turns the first paddle
	for (int p = 0; p < A800_PORTS; p++)
		if (im->PortDevice(p) == A800_DEV_PADDLES) {
			int v = im->pot[p * 2].load() - dx * im->mouseSpeed.load() / 2;
			im->pot[p * 2].store(std::max(0, std::min(228, v)));
			if (buttons & 1)
				im->paddleFire.fetch_or(1u << (p * 2));
			else
				im->paddleFire.fetch_and(~(1u << (p * 2)));
			break;
		}
}

int a800session_pointer_device_active(a800session* s)
{
	SessionImpl* im = Impl(s);
	for (int p = 0; p < A800_PORTS; p++) {
		int d = im->PortDevice(p);
		if (is_mouse_device(d) || d == A800_DEV_PADDLES)
			return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// the on-screen keyboard
// ---------------------------------------------------------------------------

namespace {

enum KeyKind { K_KEY, K_SHIFT, K_CTRL, K_CONSOLE, K_BREAK };

struct OskKey
{
	const char* label;
	const char* shifted;
	int kind;
	int code;           // AKEY (K_KEY) or a800_console (K_CONSOLE)
	float x, y, w;
};

// One table: the 800's keyboard, with the XL's HELP and the 1200XL's F1-F4
// in the console row (filtered per machine in a800session_keyboard_layout).
const OskKey osk_keys[] = {
	{"RESET", nullptr, K_CONSOLE, A800_CON_RESET, 0.0f, 0, 2.0f},
	{"OPTION", nullptr, K_CONSOLE, A800_CON_OPTION, 2.5f, 0, 2.0f},
	{"SELECT", nullptr, K_CONSOLE, A800_CON_SELECT, 4.5f, 0, 2.0f},
	{"START", nullptr, K_CONSOLE, A800_CON_START, 6.5f, 0, 2.0f},
	{"HELP", nullptr, K_CONSOLE, A800_CON_HELP, 9.0f, 0, 1.75f},
	{"F1", nullptr, K_KEY, AKEY_F1, 11.25f, 0, 1.0f},
	{"F2", nullptr, K_KEY, AKEY_F2, 12.25f, 0, 1.0f},
	{"F3", nullptr, K_KEY, AKEY_F3, 13.25f, 0, 1.0f},
	{"F4", nullptr, K_KEY, AKEY_F4, 14.25f, 0, 1.0f},

	{"ESC", nullptr, K_KEY, AKEY_ESCAPE, 0, 1, 1.25f},
	{"1", "!", K_KEY, AKEY_1, 1.25f, 1, 1}, {"2", "\"", K_KEY, AKEY_2, 2.25f, 1, 1},
	{"3", "#", K_KEY, AKEY_3, 3.25f, 1, 1}, {"4", "$", K_KEY, AKEY_4, 4.25f, 1, 1},
	{"5", "%", K_KEY, AKEY_5, 5.25f, 1, 1}, {"6", "&", K_KEY, AKEY_6, 6.25f, 1, 1},
	{"7", "'", K_KEY, AKEY_7, 7.25f, 1, 1}, {"8", "@", K_KEY, AKEY_8, 8.25f, 1, 1},
	{"9", "(", K_KEY, AKEY_9, 9.25f, 1, 1}, {"0", ")", K_KEY, AKEY_0, 10.25f, 1, 1},
	{"<", "CLEAR", K_KEY, AKEY_LESS, 11.25f, 1, 1}, {">", "INSERT", K_KEY, AKEY_GREATER, 12.25f, 1, 1},
	{"BACK S", "DELETE", K_KEY, AKEY_BACKSPACE, 13.25f, 1, 1.5f},
	{"BREAK", nullptr, K_BREAK, 0, 14.75f, 1, 1.25f},

	{"TAB", "SET", K_KEY, AKEY_TAB, 0, 2, 1.75f},
	{"Q", nullptr, K_KEY, AKEY_q, 1.75f, 2, 1}, {"W", nullptr, K_KEY, AKEY_w, 2.75f, 2, 1},
	{"E", nullptr, K_KEY, AKEY_e, 3.75f, 2, 1}, {"R", nullptr, K_KEY, AKEY_r, 4.75f, 2, 1},
	{"T", nullptr, K_KEY, AKEY_t, 5.75f, 2, 1}, {"Y", nullptr, K_KEY, AKEY_y, 6.75f, 2, 1},
	{"U", nullptr, K_KEY, AKEY_u, 7.75f, 2, 1}, {"I", nullptr, K_KEY, AKEY_i, 8.75f, 2, 1},
	{"O", nullptr, K_KEY, AKEY_o, 9.75f, 2, 1}, {"P", nullptr, K_KEY, AKEY_p, 10.75f, 2, 1},
	{"-", "_", K_KEY, AKEY_MINUS, 11.75f, 2, 1}, {"=", "|", K_KEY, AKEY_EQUAL, 12.75f, 2, 1},
	{"RETURN", nullptr, K_KEY, AKEY_RETURN, 13.75f, 2, 2.25f},

	{"CTRL", nullptr, K_CTRL, 0, 0, 3, 2.0f},
	{"A", nullptr, K_KEY, AKEY_a, 2.0f, 3, 1}, {"S", nullptr, K_KEY, AKEY_s, 3.0f, 3, 1},
	{"D", nullptr, K_KEY, AKEY_d, 4.0f, 3, 1}, {"F", nullptr, K_KEY, AKEY_f, 5.0f, 3, 1},
	{"G", nullptr, K_KEY, AKEY_g, 6.0f, 3, 1}, {"H", nullptr, K_KEY, AKEY_h, 7.0f, 3, 1},
	{"J", nullptr, K_KEY, AKEY_j, 8.0f, 3, 1}, {"K", nullptr, K_KEY, AKEY_k, 9.0f, 3, 1},
	{"L", nullptr, K_KEY, AKEY_l, 10.0f, 3, 1}, {";", ":", K_KEY, AKEY_SEMICOLON, 11.0f, 3, 1},
	{"+", "\\", K_KEY, AKEY_PLUS, 12.0f, 3, 1}, {"*", "^", K_KEY, AKEY_ASTERISK, 13.0f, 3, 1},
	{"CAPS", nullptr, K_KEY, AKEY_CAPSTOGGLE, 14.0f, 3, 2.0f},

	{"SHIFT", nullptr, K_SHIFT, 0, 0, 4, 2.5f},
	{"Z", nullptr, K_KEY, AKEY_z, 2.5f, 4, 1}, {"X", nullptr, K_KEY, AKEY_x, 3.5f, 4, 1},
	{"C", nullptr, K_KEY, AKEY_c, 4.5f, 4, 1}, {"V", nullptr, K_KEY, AKEY_v, 5.5f, 4, 1},
	{"B", nullptr, K_KEY, AKEY_b, 6.5f, 4, 1}, {"N", nullptr, K_KEY, AKEY_n, 7.5f, 4, 1},
	{"M", nullptr, K_KEY, AKEY_m, 8.5f, 4, 1}, {",", "[", K_KEY, AKEY_COMMA, 9.5f, 4, 1},
	{".", "]", K_KEY, AKEY_FULLSTOP, 10.5f, 4, 1}, {"/", "?", K_KEY, AKEY_SLASH, 11.5f, 4, 1},
	{"SHIFT", nullptr, K_SHIFT, 0, 12.5f, 4, 2.5f},
	{"\xe2\x97\x90", nullptr, K_KEY, AKEY_ATARI, 15.0f, 4, 1.0f},   // the Atari / inverse key

	{"", nullptr, K_KEY, AKEY_SPACE, 3.5f, 5, 9.0f},
};

constexpr int N_OSK = (int)(sizeof osk_keys / sizeof osk_keys[0]);

} // namespace

int a800session_keyboard_layout(a800session* s, const a800_kbd_key** keys)
{
	static a800_kbd_key out[N_OSK];
	int m = prefs_machine(s), n = 0;
	bool xl = a800_machine_is_xl(m), f1200 = m == A800_M_1200XL;
	for (int i = 0; i < N_OSK; i++) {
		const OskKey& k = osk_keys[i];
		if (k.kind == K_CONSOLE && k.code == A800_CON_HELP && !xl)
			continue;
		if (k.kind == K_KEY && (k.code == AKEY_F1 || k.code == AKEY_F2 || k.code == AKEY_F3
		                        || k.code == AKEY_F4) && !f1200)
			continue;
		out[n].label = k.label;
		out[n].shifted = k.shifted;
		out[n].index = i;
		out[n].x = k.x;
		out[n].y = k.y;
		out[n].w = k.w;
		out[n].h = 1.0f;
		n++;
	}
	*keys = out;
	return n;
}

static std::atomic<unsigned> osk_latch { 0 };     // bit0 shift, bit1 ctrl
static std::atomic<int> osk_down { -1 };

void a800session_keyboard_press(a800session* s, int index, int down)
{
	SessionImpl* im = Impl(s);
	if (index < 0 || index >= N_OSK)
		return;
	const OskKey& k = osk_keys[index];
	switch (k.kind) {
	case K_SHIFT:
		if (down)
			osk_latch.fetch_xor(1);
		return;
	case K_CTRL:
		if (down)
			osk_latch.fetch_xor(2);
		return;
	case K_CONSOLE:
		a800session_console(s, k.code, down);
		return;
	case K_BREAK:
		a800session_console(s, A800_CON_BREAK, down);
		return;
	default:
		break;
	}
	if (down) {
		unsigned l = osk_latch.load();
		int code = k.code | ((l & 1) ? AKEY_SHFT : 0) | ((l & 2) ? AKEY_CTRL : 0);
		im->oskKey.store(code);
		osk_down.store(index);
	}
	else {
		im->oskKey.store(AKEY_NONE);
		osk_down.store(-1);
		osk_latch.store(0);
	}
}

int a800session_keyboard_held(a800session* s, int index)
{
	SessionImpl* im = Impl(s);
	if (index < 0 || index >= N_OSK)
		return 0;
	const OskKey& k = osk_keys[index];
	switch (k.kind) {
	case K_SHIFT: return (osk_latch.load() & 1) || im->modShift;
	case K_CTRL: return (osk_latch.load() & 2) || im->modCtrl;
	case K_CONSOLE: return (im->console.load() >> k.code) & 1;
	case K_BREAK: return (im->console.load() >> A800_CON_BREAK) & 1;
	default: break;
	}
	if (osk_down.load() == index)
		return 1;
	int held = im->keyCode.load();
	return held != AKEY_NONE && (held & ~AKEY_SHFTCTRL) == (k.code & ~AKEY_SHFTCTRL);
}

// ---------------------------------------------------------------------------
// paste and copy
// ---------------------------------------------------------------------------

namespace {
constexpr int PasteHold = 2;            // frames a key is down
constexpr int PasteGap = 1;             // frames between keys
constexpr int PasteWaitKey = 10;        // frames to wait for the OS to take a key
constexpr int PasteWaitLine = 600;      // ... after RETURN (a line may run)
constexpr int CH = 0x2fc;
}

int a800session_paste(a800session* s, const char* utf8)
{
	SessionImpl* im = Impl(s);
	bool upper = a800session_get_int(s, "paste_upper", 1) != 0;
	std::vector<int> q;
	for (const unsigned char* p = (const unsigned char*)utf8; p && *p; p++) {
		uint32_t c = *p;
		if (c >= 0x80) {
			// skip the rest of a multi-byte character: no ATASCII for it
			while ((p[1] & 0xc0) == 0x80)
				p++;
			continue;
		}
		if (c == '\r')
			continue;
		if (c == '\n') {
			q.push_back(AKEY_RETURN);
			continue;
		}
		if (c == '\t') {
			q.push_back(AKEY_TAB);
			continue;
		}
		if (upper && c >= 'a' && c <= 'z')
			c -= 32;
		int k = keymap_char(c);
		if (k != AKEY_NONE)
			q.push_back(k);
	}
	std::lock_guard<std::mutex> lock(im->pasteMtx);
	im->pasteQueue.insert(im->pasteQueue.end(), q.begin(), q.end());
	return (int)q.size();
}

void a800session_paste_cancel(a800session* s)
{
	SessionImpl* im = Impl(s);
	std::lock_guard<std::mutex> lock(im->pasteMtx);
	im->pasteQueue.clear();
	im->pastePos = 0;
}

int a800session_paste_pending(a800session* s)
{
	SessionImpl* im = Impl(s);
	std::lock_guard<std::mutex> lock(im->pasteMtx);
	return (int)(im->pasteQueue.size() - std::min(im->pastePos, im->pasteQueue.size()));
}

// Emulation thread, after each frame: one key at a time, each once the OS
// has taken the last (CH back to $FF).
void SessionImpl::PasteStep()
{
	std::lock_guard<std::mutex> lock(pasteMtx);
	int k = oskKey.load();
	if (k != AKEY_NONE && pasteQueue.empty()) {
		pasteKey = k;
		return;
	}
	if (pasteQueue.empty()) {
		pasteKey = AKEY_NONE;
		return;
	}
	if (pastePos >= pasteQueue.size()) {
		pasteQueue.clear();
		pastePos = 0;
		pasteKey = AKEY_NONE;
		pastePhase = 0;
		return;
	}
	switch (pastePhase) {
	case 0: {
		int limit = pastePos > 0 && pasteQueue[pastePos - 1] == AKEY_RETURN ? PasteWaitLine
		                                                                     : PasteWaitKey;
		if (MEMORY_dGetByte(CH) != 0xff && pasteWait < limit) {
			pasteWait++;
			return;
		}
		pasteKey = pasteQueue[pastePos];
		pasteWait = PasteHold;
		pastePhase = 1;
		break;
	}
	case 1:
		if (--pasteWait > 0)
			return;
		pasteKey = AKEY_NONE;
		pasteWait = PasteGap;
		pastePhase = 2;
		break;
	default:
		if (--pasteWait > 0)
			return;
		pastePos++;
		pasteWait = 0;
		pastePhase = 0;
		break;
	}
}

// Internal screen code -> ASCII (the ATASCII graphics as '.').
static char screen_ascii(unsigned char c)
{
	c &= 0x7f;
	if (c < 0x40)
		c += 0x20;
	else if (c < 0x60)
		c -= 0x40;
	return c >= 0x20 && c < 0x7f ? (char)c : ' ';
}

int a800session_copy_screen_text(a800session* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	std::string out;
	im->host.Call([&] {
		UWORD savmsc = MEMORY_dGetByte(0x58) | (MEMORY_dGetByte(0x59) << 8);
		for (int row = 0; row < 24; row++) {
			std::string line;
			for (int col = 0; col < 40; col++)
				line += screen_ascii(MEMORY_dGetByte((UWORD)(savmsc + row * 40 + col)));
			while (!line.empty() && line.back() == ' ')
				line.pop_back();
			out += line + "\n";
		}
	}, A800Host::Kind::Any);
	while (out.size() > 1 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
		out.pop_back();
	if (dst && dstsz > 0)
		snprintf(dst, (size_t)dstsz, "%s", out.c_str());
	return (int)out.size();
}
