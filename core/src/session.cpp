/*
 * session.cpp -- a800session: the emulator (core/atari800/A800Host) under
 * the settings, input, media and FujiNet the frontends drive.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "A800Host.h"
#include "session_impl.h"
#include "session_internal.h"
#include "keymap.h"

extern "C" {
#include "config.h"
#include "akey.h"
#include "artifact.h"
#include "atari.h"
#include "binload.h"
#include "cartridge.h"
#include "cassette.h"
#include "cfg.h"
#include "colours.h"
#include "colours_external.h"
#include "colours_ntsc.h"
#include "colours_pal.h"
#include "cpu.h"
#include "devices.h"
#include "esc.h"
#include "filter_ntsc.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "netsio.h"
#include "pokeysnd.h"
#include "sio.h"
#include "sound.h"
#include "af80.h"
#include "bit3.h"
#include "pbi.h"
#include "pbi_bb.h"
#include "pbi_mio.h"
#include "voicebox.h"
#include "xep80.h"
#include "a8host.h"
#include "a8video.h"
}

SessionImpl* Impl(a800session* s)
{
	return static_cast<SessionImpl*>(s->impl);
}

void session_set_error(struct a800session* s, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
	va_end(ap);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

a800session* a800session_new(const a800session_paths* paths)
{
	a800session* s = (a800session*)calloc(1, sizeof *s);
	if (!s)
		return nullptr;
	pthread_mutex_init(&s->sysact_mtx, nullptr);
	pthread_mutex_init(&s->pad_event_mtx, nullptr);
	if (paths_init(s, paths ? paths->config_dir : nullptr, paths ? paths->data_dir : nullptr) != 0) {
		pthread_mutex_destroy(&s->sysact_mtx);
		pthread_mutex_destroy(&s->pad_event_mtx);
		free(s);
		return nullptr;
	}
	if (paths && paths->fujinet_lib) {
		snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", paths->fujinet_lib);
		s->fujinet_disabled = !*paths->fujinet_lib;
	}
	if (paths && paths->fujinet_runtime_src)
		snprintf(s->fujinet_runtime_src, sizeof s->fujinet_runtime_src, "%s",
		         paths->fujinet_runtime_src);
	settings_init(s);
	s->impl = new SessionImpl(s);
	roms_provision_embedded(s);
	return s;
}

void a800session_free(a800session* s)
{
	if (!s)
		return;
	a800session_stop(s);
	session_debug_free(s);
	if (Impl(s)->coreUp) {
		// no emulation thread now: the core is ours to tear down
		Atari800_Exit(FALSE);
		Impl(s)->coreUp = false;
	}
	delete Impl(s);
	a800session_settings_flush(s);
	settings_free_all(s);
	pthread_mutex_destroy(&s->sysact_mtx);
	pthread_mutex_destroy(&s->pad_event_mtx);
	free(s);
}

void a800session_default_opts(a800session* s, a800session_start_opts* o)
{
	o->enable_fujinet = a800session_get_int(s, "enable_fujinet", 1);
	o->enable_audio = a800session_get_int(s, "enable_audio", 1);
	o->enable_gamepad = a800session_get_int(s, "enable_gamepad", 1);
}

int a800session_is_running(const a800session* s)
{
	return s->running;
}

const char* a800session_last_error(const a800session* s)
{
	return s->last_error;
}

// ---- on the emulation thread -------------------------------------------------

static void apply_option(void*, const char* key, const char* val)
{
	char k[128], v[1100];
	snprintf(k, sizeof k, "%s", key);
	snprintf(v, sizeof v, "%s", val);
	CFG_ApplyOption(k, v);
}

// The palette and the NTSC filter: presets plus the user's adjustments.
void SessionImpl::ApplyVideo()
{
	static const Colours_setup_t presets[] = {
		{0.0, 0.0, 0.0, 0.0, 2.35, 0.0, 16, 235},
		{0.0, 0.0, 0.08, -0.08, 2.35, 0.0, 16, 235},
		{0.0, 0.26, 0.72, -0.16, 2.00, 0.0, 16, 235},
	};
	auto adj = [&](const char* key) { return a800session_get_int(s, key, 0) / 100.0; };
	auto clamp = [](double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; };
	int p = a800session_get_int(s, "palette_preset", 0);
	if (p < 0 || p > 2)
		p = 0;
	int np = a800session_get_int(s, "ntsc_preset", 0);
	if (np < 0 || np >= FILTER_NTSC_PRESET_SIZE)
		np = 0;

	// the filter preset first: it also writes COLOURS_NTSC_setup
	FILTER_NTSC_SetPreset(np);
	FILTER_NTSC_setup.sharpness = clamp(FILTER_NTSC_setup.sharpness + adj("ntsc_sharpness"), -1, 1);
	FILTER_NTSC_setup.resolution = clamp(FILTER_NTSC_setup.resolution + adj("ntsc_resolution"), -1, 1);
	FILTER_NTSC_setup.artifacts = clamp(FILTER_NTSC_setup.artifacts + adj("ntsc_artifacts"), -1, 1);
	FILTER_NTSC_setup.fringing = clamp(FILTER_NTSC_setup.fringing + adj("ntsc_fringing"), -1, 1);
	FILTER_NTSC_setup.bleed = clamp(FILTER_NTSC_setup.bleed + adj("ntsc_bleed"), -1, 1);

	Colours_setup_t c = presets[p];
	c.hue = clamp(c.hue + adj("hue"), COLOURS_HUE_MIN, COLOURS_HUE_MAX);
	c.saturation = clamp(c.saturation + adj("saturation"), COLOURS_SATURATION_MIN, COLOURS_SATURATION_MAX);
	c.contrast = clamp(c.contrast + adj("contrast"), COLOURS_CONTRAST_MIN, COLOURS_CONTRAST_MAX);
	c.brightness = clamp(c.brightness + adj("brightness"), COLOURS_BRIGHTNESS_MIN, COLOURS_BRIGHTNESS_MAX);
	c.gamma = clamp(c.gamma + adj("gamma"), COLOURS_GAMMA_MIN, COLOURS_GAMMA_MAX);
	c.color_delay = COLOURS_NTSC_setup.color_delay;
	COLOURS_NTSC_setup = c;
	c.color_delay = COLOURS_PAL_setup.color_delay;
	COLOURS_PAL_setup = c;

	const char* pal = a800session_get_str(s, "palette_file", "");
	if (*pal) {
		COLOURS_EXTERNAL_ReadFilename(&COLOURS_NTSC_external, (char*)pal);
		COLOURS_EXTERNAL_ReadFilename(&COLOURS_PAL_external, (char*)pal);
	}
	else {
		COLOURS_EXTERNAL_Remove(&COLOURS_NTSC_external);
		COLOURS_EXTERNAL_Remove(&COLOURS_PAL_external);
	}

	static const ARTIFACT_t ntsc_modes[] = {ARTIFACT_NONE, ARTIFACT_NTSC_OLD, ARTIFACT_NTSC_NEW,
	                                        ARTIFACT_NTSC_FULL};
	static const ARTIFACT_t pal_modes[] = {ARTIFACT_NONE, ARTIFACT_PAL_SIMPLE, ARTIFACT_PAL_BLEND};
	int an = a800session_get_int(s, "artifact_ntsc", 0);
	int ap = a800session_get_int(s, "artifact_pal", 0);
	ARTIFACT_t mode = Atari800_tv_mode == Atari800_TV_PAL ? pal_modes[ap >= 0 && ap < 3 ? ap : 0]
	                                                      : ntsc_modes[an >= 0 && an < 4 ? an : 0];
	if (mode != ARTIFACT_mode)
		ARTIFACT_Set(mode);
	Colours_Update();
	if (ARTIFACT_mode == ARTIFACT_NTSC_FULL) {
		if (!FILTER_NTSC_emu)
			FILTER_NTSC_emu = FILTER_NTSC_New();
		FILTER_NTSC_Update(FILTER_NTSC_emu);
	}
	a8video_invalidate();
	area.store(a800session_get_int(s, "area", 1));
}

// Settings that apply without a restart.
void SessionImpl::ApplyLive()
{
	prefs_emit_live(s, apply_option, nullptr);
	ApplyVideo();
	ESC_enable_sio_patch = !s->fujinet_wanted && a800session_get_int(s, "sio_patch", 1);
	Devices_enable_h_patch = TRUE;
	Devices_enable_p_patch = !s->fujinet_wanted;
	Devices_enable_r_patch = FALSE;
	BINLOAD_slow_xex_loading = a800session_get_int(s, "slow_xex", 0);
	ESC_UpdatePatches();
	POKEYSND_console_sound_enabled = a800session_get_int(s, "speaker", 1);
	Atari800_keyboard_detached = prefs_machine(s) == A800_M_XEGS
	                             && !a800session_get_int(s, "xegs_keyboard", 1);
	int af = a800session_get_int(s, "autofire", 0);
	for (int p = 0; p < 4; p++)
		INPUT_joy_autofire[p] = af == 1 ? INPUT_AUTOFIRE_FIRE : af == 2 ? INPUT_AUTOFIRE_CONT
		                                                               : INPUT_AUTOFIRE_OFF;
	INPUT_joy_multijoy = a800session_get_int(s, "multijoy", 0);
	positional.store(a800session_get_int(s, "kbd_mode", 0) == 1);
	host.SetVolume(a800session_get_int(s, "volume", 80));
	ApplyPorts();
}

bool SessionImpl::CoreInit(std::string& err)
{
	char* argv[32];
	int argc = prefs_build_argv(s, argv, 31);
	Atari800_host_paced = TRUE;
	CFG_save_on_exit = FALSE;
	CARTRIDGE_allow_5200_switch = FALSE;
	Sound_desired.freq = A8HOST_AUDIO_RATE;
	Sound_desired.sample_size = 2;
	Sound_desired.channels = a800session_get_int(s, "stereo", 0) ? 2 : 1;
	Sound_latency = 0;
	// atari800's Exit frees the peripherals but leaves them "enabled";
	// only the flags in argv may turn them on again
	if (BINLOAD_bin_file) {
		fclose(BINLOAD_bin_file);
		BINLOAD_bin_file = NULL;
	}
	BINLOAD_start_binloading = FALSE;
	BINLOAD_loading_basic = 0;
	XEP80_enabled = AF80_enabled = BIT3_enabled = FALSE;
	PBI_MIO_enabled = PBI_BB_enabled = FALSE;
	VOICEBOX_enabled = FALSE;
	PBI_D6D7ram = FALSE;
	// ApplyPorts sets these after; a direct mouse left from paddles or a
	// Koala Pad fails INPUT_Initialise, whose ErrExit then frees twice
	INPUT_direct_mouse = 0;
	INPUT_mouse_mode = INPUT_MOUSE_OFF;
	logText.clear();
	if (!Atari800_Initialise(&argc, argv)) {
		err = "atari800 did not start";
		if (Log_buffer[0]) {
			err += ": ";
			err += Log_buffer;
		}
		Log_buffer[0] = 0;
		return false;
	}
	coreUp = true;
	if (s->fujinet_wanted) {
		// the hub is bound before FujiNet starts: FujiNet pings it first
		netsio_loopback = 1;
		if (netsio_init(A800SESSION_NETSIO_PORT) != 0) {
			Notice("FujiNet is off: NetSIO port " + std::to_string(A800SESSION_NETSIO_PORT)
			       + " is in use (another copy of this app?)");
			s->fujinet_wanted = 0;
		}
	}
	else {
		netsio_enabled = 0;
	}
	ApplyLive();
	RemountMedia();
	ColdStart();
	return true;
}

// atari800 again from the top, keeping the media: machine, ROM and
// peripheral changes.
bool SessionImpl::CoreReinit(std::string& err)
{
	if (coreUp) {
		Atari800_Exit(FALSE);
		coreUp = false;
	}
	return CoreInit(err);
}

void SessionImpl::ColdStart()
{
	// a program Open loaded earlier must not load again at power-on
	if (BINLOAD_bin_file) {
		fclose(BINLOAD_bin_file);
		BINLOAD_bin_file = NULL;
	}
	BINLOAD_start_binloading = FALSE;
	BINLOAD_loading_basic = 0;
	CPU_cim_encountered = FALSE;   // a crash left behind with no debugger
	suppressReset = true;
	Atari800_Coldstart();
	suppressReset = false;
}

void SessionImpl::RemountMedia()
{
	std::lock_guard<std::mutex> lock(mediaMtx);
	for (int d = 0; d < A800_DRIVES; d++)
		if (!drive[d].empty() && !s->fujinet_wanted)
			SIO_Mount(d + 1, drive[d].c_str(), driveRO[d]);
	if (!cart.empty()) {
		int rc = CARTRIDGE_InsertAutoReboot(cart.c_str());
		if (rc > 0 && cartType > 0)
			CARTRIDGE_SetType(&CARTRIDGE_main, cartType);
	}
	if (!cart2.empty()) {
		int rc = CARTRIDGE_Insert_Second(cart2.c_str());
		if (rc > 0 && cart2Type > 0)
			CARTRIDGE_SetType(&CARTRIDGE_piggyback, cart2Type);
	}
	if (!tape.empty())
		CASSETTE_Insert(tape.c_str());
}

// Input staged from any thread, into the core before each frame.
void SessionImpl::BeforeFrame()
{
	for (int p = 0; p < A800_PORTS; p++) {
		unsigned b = joy[p].load() | joyPad[p].load();
		unsigned char stick = INPUT_STICK_CENTRE;
		if (b & (1u << A800_ACT_UP))
			stick &= ~1;
		if (b & (1u << A800_ACT_DOWN))
			stick &= ~2;
		if (b & (1u << A800_ACT_LEFT))
			stick &= ~4;
		if (b & (1u << A800_ACT_RIGHT))
			stick &= ~8;
		if (PortDevice(p) == A800_DEV_JOYSTICK) {
			a8host_stick[p] = stick;
			a8host_trig[p] = (b & (1u << A800_ACT_FIRE)) ? 0 : 1;
		}
	}
	DevicesBeforeFrame();

	unsigned con = console.load() | pasteConsole;
	int consol = INPUT_CONSOL_NONE;
	if (con & (1u << A800_CON_START))
		consol &= ~INPUT_CONSOL_START;
	if (con & (1u << A800_CON_SELECT))
		consol &= ~INPUT_CONSOL_SELECT;
	if (con & (1u << A800_CON_OPTION))
		consol &= ~INPUT_CONSOL_OPTION;
	INPUT_key_consol = consol;

	int code = keyCode.load();
	int shift = keyShift.load();
	if (code != AKEY_NONE)
		keyFrames.fetch_add(1);
	else if (tapLeft.load() > 0) {
		// a key let go before the Atari saw it: it gets its frames
		code = tapCode.load();
		shift = tapShift.load();
		tapLeft.fetch_sub(1);
	}
	if (pasteKey != AKEY_NONE) {
		code = pasteKey;
		shift = (pasteKey & AKEY_SHFT) != 0;
	}
	if (con & (1u << A800_CON_HELP))
		code = AKEY_HELP;
	if (con & (1u << A800_CON_BREAK))
		code = AKEY_BREAK;
	INPUT_key_code = code;
	INPUT_key_shift = shift;

	bool reset = (con & (1u << A800_CON_RESET)) != 0;
	if (reset && !resetHeld)
		Atari800_Warmstart();
	resetHeld = reset;
}

void SessionImpl::AfterFrame()
{
	PasteStep();
	if (SIO_last_op_time > 0)
		SIO_last_op_time--;
	session_debug_after_frame();
}

void SessionImpl::OnLog(const char* text)
{
	std::lock_guard<std::mutex> lock(logMtx);
	logText += text;
	if (logText.size() > 64 * 1024)
		logText.erase(0, logText.size() - 48 * 1024);
	if (getenv("A800_LOG"))
		fputs(text, stderr);
}

SessionImpl::SessionImpl(a800session* sess) : s(sess)
{
	for (auto& j : joy)
		j.store(0);
	for (auto& j : joyPad)
		j.store(0);
	A800Host::Callbacks cb;
	cb.init = [this](std::string& err) { return CoreReinit(err); };
	cb.applyConfig = [this] { prefs_emit_options(s, apply_option, nullptr); };
	cb.beforeFrame = [this] { BeforeFrame(); };
	cb.afterFrame = [this] { AfterFrame(); };
	cb.videoOptions = [this](int& a, bool& show80) {
		a = area.load();
		show80 = show80col.load();
	};
	cb.onStopped = [this] { session_debug_stopped(s); };
	cb.onLog = [this](const char* t) { OnLog(t); };
	host.SetCallbacks(cb);
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

int a800session_start(a800session* s, const a800session_start_opts* opts)
{
	SessionImpl* im = Impl(s);
	if (s->running)
		return 0;
	s->last_error[0] = 0;
	if (opts)
		s->opts = *opts;
	else
		a800session_default_opts(s, &s->opts);
	s->fujinet_wanted = s->opts.enable_fujinet && session_fujinet_available(s);

	std::string err;
	// with FujiNet, the Atari stays off until FujiNet is there to boot it
	im->host.SetPowered(!s->fujinet_wanted);
	if (!im->host.Start(err)) {
		session_set_error(s, "%s", err.c_str());
		return -1;
	}
	s->running = 1;
	if (s->fujinet_wanted) {
		if (session_fujinet_start(s) == 0 && !session_fujinet_wait_link(s, 4000))
			im->Notice("FujiNet has not connected yet");
		im->host.Call([im] { im->ColdStart(); });
		im->host.SetPowered(true);
	}
	if (s->opts.enable_gamepad)
		gamepad_start(s);
	if (s->opts.enable_audio && audio_start(s) != 0)
		fprintf(stderr, "a800: %s (continuing without sound)\n", s->last_error);
	im->SnapshotApplied();
	return 0;
}

void a800session_stop(a800session* s)
{
	SessionImpl* im = Impl(s);
	if (!s->running)
		return;
	audio_stop(s);
	gamepad_stop(s);
	session_fujinet_stop(s);
	im->host.Stop();
	s->running = 0;
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------

std::string SessionImpl::PrefValue(int i)
{
	const a800_pref* p = a800session_pref(i);
	if (!p)
		return "";
	if (p->type == A800_PREF_CHOICE) {
		const char* label = a800session_pref_choice(s, i, a800session_pref_get_choice(s, i));
		return std::to_string(a800session_pref_get_choice(s, i)) + ":" + (label ? label : "");
	}
	if (p->type == A800_PREF_FILE || p->type == A800_PREF_DIR)
		return a800session_get_str(s, p->key, "");
	return std::to_string(a800session_get_int(s, p->key, p->def));
}

void SessionImpl::SnapshotApplied()
{
	applied.clear();
	for (int i = 0; i < a800session_pref_count(); i++)
		applied.push_back(PrefValue(i));
	appliedRoms = RomsSignature();
	appliedArgv = ArgvSignature();
}

std::string SessionImpl::RomsSignature()
{
	std::string sig;
	for (int i = 0; i < a800session_rom_slot_count(); i++)
		sig += a800session_rom_available(s, i) ? '1' : '0';
	return sig;
}

std::string SessionImpl::ArgvSignature()
{
	char* argv[32];
	int argc = prefs_build_argv(s, argv, 31);
	std::string sig;
	for (int i = 0; i < argc; i++)
		sig += std::string(argv[i]) + '\n';
	return sig;
}

int a800session_apply_settings(a800session* s)
{
	SessionImpl* im = Impl(s);
	int need = A800_APPLY_LIVE;
	bool restart = false;
	for (int i = 0; i < a800session_pref_count(); i++) {
		std::string v = im->PrefValue(i);
		if (i < (int)im->applied.size() && v == im->applied[i])
			continue;
		const a800_pref* p = a800session_pref(i);
		need = std::max(need, p->applies);
		if (p->applies == A800_APPLY_RESTART)
			restart = true;
	}
	// a peripheral flag atari800 only reads at start
	if (need < A800_APPLY_POWER && im->ArgvSignature() != im->appliedArgv)
		need = A800_APPLY_POWER;
	a800session_settings_flush(s);
	if (!s->running) {
		im->SnapshotApplied();
		return need;
	}
	if (restart) {
		a800session_start_opts o;
		a800session_default_opts(s, &o);
		a800session_stop(s);
		a800session_start(s, &o);
	}
	else if (need == A800_APPLY_POWER) {
		std::string err;
		im->host.Call([&] {
			if (!im->CoreReinit(err))
				session_set_error(s, "%s", err.c_str());
		});
		session_fujinet_power_cycled(s);
	}
	else {
		im->host.Call([im] { im->ApplyLive(); });
	}
	im->SnapshotApplied();
	return need;
}

void session_roms_changed(struct a800session* s)
{
	(void)s;  // picked up by the next power cycle (options are rebuilt then)
}

// ---------------------------------------------------------------------------
// power
// ---------------------------------------------------------------------------

int a800session_power_cycle(a800session* s)
{
	SessionImpl* im = Impl(s);
	if (!s->running)
		return -1;
	if (s->fujinet_wanted && a800session_get_int(s, "fujinet_power", 1))
		session_fujinet_restart_for_power(s);
	im->host.Call([im] { im->ColdStart(); });
	return 0;
}

int a800session_reset(a800session* s)
{
	SessionImpl* im = Impl(s);
	if (!s->running)
		return -1;
	im->host.Call([] { Atari800_Warmstart(); });
	return 0;
}

void a800session_set_warp(a800session* s, int on)
{
	SessionImpl* im = Impl(s);
	im->host.SetWarp(on != 0);
	im->host.Post([on] {
		Atari800_refresh_rate = on ? 4 : 1;
		Atari800_collisions_in_skipped_frames = on ? TRUE : FALSE;
	});
	if (!on)
		im->host.FlushAudio();
}

int a800session_warp(a800session* s)
{
	return Impl(s)->host.Warp() ? 1 : 0;
}

// ---------------------------------------------------------------------------
// video and audio
// ---------------------------------------------------------------------------

int a800session_copy_frame(a800session* s, uint32_t* dst, int* width, int* height,
                           int* par_milli, uint64_t* serial_inout)
{
	int w = 0, h = 0, par = 1000;
	bool fresh = Impl(s)->host.CopyFrame(dst, (size_t)A800SESSION_FB_MAX_WIDTH * A800SESSION_FB_MAX_HEIGHT,
	                                     w, h, par, serial_inout);
	if (width)
		*width = w;
	if (height)
		*height = h;
	if (par_milli)
		*par_milli = par;
	return fresh ? 1 : 0;
}

int a800session_refresh_rate(a800session* s)
{
	return (int)std::lround(Impl(s)->host.Fps() * 1000.0);
}

void a800session_notify_vsync(a800session* s, int64_t frame_time_ns)
{
	Impl(s)->host.NotifyVsync(frame_time_ns);
}

void a800session_set_80col_display(a800session* s, int on)
{
	Impl(s)->show80col.store(on != 0);
	a800session_set_int(s, "show_80col", on != 0);
}

int a800session_80col_display(a800session* s)
{
	return Impl(s)->show80col.load() ? 1 : 0;
}

int a800session_80col_fitted(a800session* s)
{
	SessionImpl* im = Impl(s);
	int card = 0;
	if (s->running)
		im->host.Call([&] { card = a8video_80col_card(); }, A800Host::Kind::Any);
	return card != 0;
}

int a800session_render_audio(a800session* s, float* stereo, int nframes)
{
	Impl(s)->host.FillAudio(stereo, (uint32_t)nframes);
	return nframes;
}

void a800session_set_volume(a800session* s, int percent)
{
	Impl(s)->host.SetVolume(percent);
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

void a800session_press(a800session* s, int target, int down)
{
	SessionImpl* im = Impl(s);
	if (target < 0 || target >= A800_TARGET_COUNT)
		return;
	if (target < A800_TARGET_CONSOLE(0)) {
		int port = target / A800_ACT_PER_PORT, act = target % A800_ACT_PER_PORT;
		if (down)
			im->joy[port].fetch_or(1u << act);
		else
			im->joy[port].fetch_and(~(1u << act));
	}
	else if (target < A800_TARGET_SYSACT(0)) {
		a800session_console(s, target - A800_TARGET_CONSOLE(0), down);
	}
	else if (down) {
		a800session_sysaction(s, target - A800_TARGET_SYSACT(0));
	}
}

void a800session_console(a800session* s, int c, int down)
{
	SessionImpl* im = Impl(s);
	if (c < 0 || c >= A800_CON_COUNT)
		return;
	if (down)
		im->console.fetch_or(1u << c);
	else
		im->console.fetch_and(~(1u << c));
}

void a800session_sysaction(a800session* s, int sysact)
{
	switch (sysact) {
	case A800_SYSACT_POWER:
		a800session_power_cycle(s);
		break;
	case A800_SYSACT_REBOOT_CONFIG:
		a800session_reboot_to_config(s);
		break;
	case A800_SYSACT_WARP:
		a800session_set_warp(s, !a800session_warp(s));
		break;
	case A800_SYSACT_PAUSE:
		session_debug_pause(s);
		break;
	default:
		break;
	}
}

void a800session_release_all(a800session* s)
{
	SessionImpl* im = Impl(s);
	for (auto& j : im->joy)
		j.store(0);
	im->console.store(0);
	std::lock_guard<std::mutex> lock(im->keyMtx);
	im->held.clear();
	im->modShift = im->modCtrl = 0;
	im->keyCode.store(AKEY_NONE);   // a pending tap (already let go) still lands
	im->keyShift.store(0);
}

unsigned a800session_buttons_held(a800session* s, int port)
{
	SessionImpl* im = Impl(s);
	if (port < 0 || port >= A800_PORTS)
		return 0;
	return im->joy[port].load() | im->joyPad[port].load();
}

unsigned a800session_console_held(a800session* s)
{
	return Impl(s)->console.load();
}

void session_gamepad_apply(struct a800session* s, int port, int act, int down)
{
	SessionImpl* im = Impl(s);
	if (port < 0 || port >= A800_PORTS || act < 0 || act >= A800_ACT_PER_PORT)
		return;
	if (down)
		im->joyPad[port].fetch_or(1u << act);
	else
		im->joyPad[port].fetch_and(~(1u << act));
}

// The Atari takes one key at a time: the most recent one still held.
// Keyboard lock held. A key pressed and let go within a frame or two (a
// fast tap, a synthetic one) would never be seen: it stays as a tap until
// the Atari has had it for KeyMinFrames.
void SessionImpl::UpdateKey()
{
	const int was = keyCode.load();
	const int now = held.empty() ? AKEY_NONE : held.back().akey;
	if (now != was) {
		if (now == AKEY_NONE && keyFrames.load() < KeyMinFrames) {
			tapCode.store(was);
			tapShift.store(keyShift.load());
			tapLeft.store(KeyMinFrames - keyFrames.load());
		}
		else if (now != AKEY_NONE)
			tapLeft.store(0);
		keyFrames.store(0);
	}
	keyCode.store(now);
	keyShift.store(modShift ? 1 : 0);
}

static int cx85_code(uint32_t keysym)
{
	switch (keysym) {
	case A800_KEYSYM_KP_0: return AKEY_CX85_0;
	case A800_KEYSYM_KP_1: return AKEY_CX85_1;
	case A800_KEYSYM_KP_2: return AKEY_CX85_2;
	case A800_KEYSYM_KP_3: return AKEY_CX85_3;
	case A800_KEYSYM_KP_4: return AKEY_CX85_4;
	case A800_KEYSYM_KP_5: return AKEY_CX85_5;
	case A800_KEYSYM_KP_6: return AKEY_CX85_6;
	case A800_KEYSYM_KP_7: return AKEY_CX85_7;
	case A800_KEYSYM_KP_8: return AKEY_CX85_8;
	case A800_KEYSYM_KP_9: return AKEY_CX85_9;
	case A800_KEYSYM_KP_PERIOD: return AKEY_CX85_PERIOD;
	case A800_KEYSYM_KP_SUBTRACT: return AKEY_CX85_MINUS;
	case A800_KEYSYM_KP_ENTER:
	case A800_KEYSYM_KP_ADD: return AKEY_CX85_PLUS_ENTER;
	case A800_KEYSYM_KP_DIVIDE: return AKEY_CX85_ESCAPE;
	case A800_KEYSYM_KP_MULTIPLY: return AKEY_CX85_DELETE;
	default: return AKEY_NONE;
	}
}

bool SessionImpl::cx85Fitted()
{
	for (int p = 0; p < A800_PORTS; p++)
		if (PortDevice(p) == A800_DEV_CX85)
			return true;
	return false;
}

int a800session_key(a800session* s, uint32_t keysym, uint32_t unicode, int down)
{
	SessionImpl* im = Impl(s);
	std::lock_guard<std::mutex> lock(im->keyMtx);
	int bit = 0;
	switch (keysym) {
	case A800_KEYSYM_LSHIFT: bit = 1; break;
	case A800_KEYSYM_RSHIFT: bit = 2; break;
	case A800_KEYSYM_LCTRL: bit = 4; break;
	case A800_KEYSYM_RCTRL: bit = 8; break;
	default: break;
	}
	if (bit) {
		if (bit < 4)
			im->modShift = down ? (im->modShift | bit) : (im->modShift & ~bit);
		else
			im->modCtrl = down ? (im->modCtrl | bit) : (im->modCtrl & ~bit);
		im->UpdateKey();
		return 1;
	}
	// a CX85 keypad on a port takes the host's numeric keypad
	int cx85 = cx85_code(keysym);
	if (cx85 != AKEY_NONE && im->cx85Fitted()) {
		if (down) {
			im->held.push_back({keysym, cx85});
		}
		else {
			im->held.erase(std::remove_if(im->held.begin(), im->held.end(),
			                              [&](const SessionImpl::Held& h) { return h.keysym == keysym; }),
			               im->held.end());
		}
		im->UpdateKey();
		return 1;
	}
	int target = session_binding_target_for_key(s, keysym);
	if (target >= 0) {
		a800session_press(s, target, down);
		return 1;
	}
	if (!down) {
		auto it = std::remove_if(im->held.begin(), im->held.end(),
		                         [&](const SessionImpl::Held& h) { return h.keysym == keysym; });
		bool was = it != im->held.end();
		im->held.erase(it, im->held.end());
		im->UpdateKey();
		return was ? 1 : 0;
	}
	int akey;
	if (!keymap_translate(keysym, unicode, im->modShift != 0, im->modCtrl != 0,
	                      im->positional.load(), im->fkeys1200.load(), &akey))
		return 0;
	for (auto& h : im->held)
		if (h.keysym == keysym)
			return 1;   // auto-repeat: the Atari repeats on its own
	im->held.push_back({keysym, akey});
	im->UpdateKey();
	return 1;
}

int a800session_keyboard_captures(a800session* s)
{
	(void)s;
	return 1;
}

// ---------------------------------------------------------------------------
// paths and status
// ---------------------------------------------------------------------------

const char* a800session_config_path(const a800session* s) { return s->config_dir; }
const char* a800session_data_path(const a800session* s) { return s->data_dir; }
const char* a800session_roms_path(const a800session* s) { return s->roms_dir; }
const char* a800session_media_path(const a800session* s) { return s->media_dir; }
const char* a800session_sd_path(const a800session* s) { return s->fujinet_sd; }

int a800session_status(a800session* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	int m = prefs_machine(s);
	const char* tv = a800session_get_int(s, "tv", 0) ? "PAL" : "NTSC";
	std::string fn;
	if (!s->fujinet_wanted)
		fn = "FujiNet off";
	else if (a800session_fujinet_link(s))
		fn = "FujiNet connected";
	else if (a800session_fujinet_running(s))
		fn = "FujiNet starting";
	else
		fn = "FujiNet stopped";
	std::string extra;
	if (im->host.Warp())
		extra += " \xc2\xb7 Warp";
	if (im->host.Stopped())
		extra += " \xc2\xb7 Stopped";
	return snprintf(dst, (size_t)dstsz, "%s \xc2\xb7 %s \xc2\xb7 %s%s", a800_machine_name(m), tv,
	                fn.c_str(), extra.c_str());
}

int a800session_take_notice(a800session* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	std::lock_guard<std::mutex> lock(im->noticeMtx);
	if (im->notice.empty())
		return 0;
	snprintf(dst, (size_t)dstsz, "%s", im->notice.c_str());
	im->notice.clear();
	return 1;
}

void SessionImpl::Notice(const std::string& text)
{
	std::lock_guard<std::mutex> lock(noticeMtx);
	notice = text;
}

// ---- cross-thread system actions ----------------------------------------------

void a800session_sysaction_post(a800session* s, int sysact)
{
	if (sysact < 0 || sysact >= A800_SYSACT_COUNT)
		return;
	pthread_mutex_lock(&s->sysact_mtx);
	s->sysact_pending |= 1u << sysact;
	pthread_mutex_unlock(&s->sysact_mtx);
}

int a800session_sysaction_take(a800session* s, int* out)
{
	int rc = 0;
	pthread_mutex_lock(&s->sysact_mtx);
	for (int i = 0; i < A800_SYSACT_COUNT; i++)
		if (s->sysact_pending & (1u << i)) {
			s->sysact_pending &= ~(1u << i);
			*out = i;
			rc = 1;
			break;
		}
	pthread_mutex_unlock(&s->sysact_mtx);
	return rc;
}

// ---- the NetSIO link ------------------------------------------------------------

int a800session_fujinet_link(a800session* s)
{
	return s->fujinet_wanted && netsio_enabled ? 1 : 0;
}

void session_fujinet_link_lost(struct a800session* s)
{
	(void)s;
	// a FujiNet that stopped does not always say so
	netsio_enabled = 0;
}

// ---- gamepads ------------------------------------------------------------------

void session_gamepad_event(struct a800session* s, const char* text)
{
	pthread_mutex_lock(&s->pad_event_mtx);
	snprintf(s->pad_event, sizeof s->pad_event, "%s", text ? text : "");
	pthread_mutex_unlock(&s->pad_event_mtx);
}

int a800session_gamepad_last_event(a800session* s, char* dst, int dstsz)
{
	if (!dst || dstsz <= 0)
		return 0;
	pthread_mutex_lock(&s->pad_event_mtx);
	const int n = snprintf(dst, (size_t)dstsz, "%s", s->pad_event);
	pthread_mutex_unlock(&s->pad_event_mtx);
	return n;
}

int session_active_ports(struct a800session* s)
{
	return !a800_machine_is_xl(prefs_machine(s)) || a800session_get_int(s, "multijoy", 0) ? 4 : 2;
}

#ifdef A800_GAMEPAD_STUB
int gamepad_start(struct a800session* s)
{
	session_set_error(s, "gamepad support not built");
	return -1;
}
void gamepad_stop(struct a800session*) {}
int a800session_gamepad_count(a800session*) { return 0; }
int a800session_gamepad_name(a800session*, int, char* dst, int dstsz)
{
	if (dst && dstsz > 0)
		dst[0] = '\0';
	return 0;
}
void a800session_gamepad_assign(a800session*, int, int) {}
int a800session_gamepad_assignment(a800session*, int) { return -1; }
int a800session_gamepad_effective_port(a800session*, int) { return -1; }
unsigned a800session_gamepad_generation(a800session*) { return 0; }
void a800session_gamepad_capture_begin(a800session*) {}
void a800session_gamepad_capture_cancel(a800session*) {}
int a800session_gamepad_capture_poll(a800session*, int*) { return 0; }
#endif
