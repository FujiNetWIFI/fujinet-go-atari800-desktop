/*
 * media.cpp -- disks, cartridges, tapes, programs and save states.
 *
 * With FujiNet on, every SIO request goes to FujiNet (atari800's own drives
 * are bypassed): a disk is copied to the SD folder and mounted in FujiNet's
 * drive slot over its REST API. With FujiNet off, the drives are
 * atari800's. Programs and cartridges load the same either way.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "fujinet_api.h"
#include "session_impl.h"
#include "zipread.h"

extern "C" {
#include "config.h"
#include "afile.h"
#include "atari.h"
#include "binload.h"
#include "cartridge.h"
#include "cartridge_info.h"
#include "cassette.h"
#include "sio.h"
#include "statesav.h"
}

namespace {

std::string base_name(const std::string& p)
{
	size_t i = p.find_last_of("/\\");
	return i == std::string::npos ? p : p.substr(i + 1);
}

std::string lower_ext(const std::string& p)
{
	std::string b = base_name(p);
	size_t i = b.rfind('.');
	std::string e = i == std::string::npos ? "" : b.substr(i + 1);
	for (auto& c : e)
		c = (char)tolower((unsigned char)c);
	return e;
}

bool write_file(const std::string& path, const uint8_t* d, size_t n)
{
	FILE* fp = fopen(path.c_str(), "wb");
	if (!fp)
		return false;
	bool ok = fwrite(d, 1, n, fp) == n;
	return fclose(fp) == 0 && ok;
}

bool copy_file(const std::string& src, const std::string& dst)
{
	if (src == dst)
		return true;
	FILE* in = fopen(src.c_str(), "rb");
	if (!in)
		return false;
	FILE* out = fopen(dst.c_str(), "wb");
	if (!out) {
		fclose(in);
		return false;
	}
	char buf[65536];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		if (fwrite(buf, 1, n, out) != n) {
			ok = false;
			break;
		}
	fclose(in);
	return fclose(out) == 0 && ok;
}

bool is_media_ext(const std::string& e)
{
	static const char* const exts[] = {"atr", "xfd", "dcm", "pro", "atx", "xex", "com", "exe",
	                                   "bas", "lst", "car", "rom", "bin", "cas", "a8s", "gz"};
	for (auto x : exts)
		if (e == x)
			return true;
	return false;
}

int kind_of_file(const char* path)
{
	if (roms_identify_file(path) >= 0)
		return A800_MEDIA_ROM;
	switch (AFILE_DetectFileType(path)) {
	case AFILE_ATR: case AFILE_XFD: case AFILE_ATR_GZ: case AFILE_XFD_GZ:
	case AFILE_DCM: case AFILE_PRO: case AFILE_ATX:
		return A800_MEDIA_DISK;
	case AFILE_XEX:
		return A800_MEDIA_EXECUTABLE;
	case AFILE_BAS: case AFILE_LST:
		return A800_MEDIA_BASIC;
	case AFILE_CART:
		return A800_MEDIA_CARTRIDGE;
	case AFILE_CAS: case AFILE_BOOT_TAPE:
		return A800_MEDIA_TAPE;
	case AFILE_STATE: case AFILE_STATE_GZ:
		return A800_MEDIA_STATE;
	default:
		break;
	}
	if (AFILE_DetectFileType(path) == AFILE_ROM)
		return A800_MEDIA_CARTRIDGE;   // a raw ROM of a size the Atari takes
	return A800_MEDIA_UNKNOWN;
}

void set_msg(char* msg, int msgsz, const std::string& text)
{
	if (msg && msgsz > 0)
		snprintf(msg, (size_t)msgsz, "%s", text.c_str());
}

} // namespace

// The first media file in a zip, unpacked into dir (or "" if none).
static std::string unpack_first_media(const char* path, const std::string& dir)
{
	zipread* z = zipread_open(path);
	if (!z)
		return "";
	std::string out;
	for (int i = 0; i < zipread_count(z) && out.empty(); i++) {
		std::string name = base_name(zipread_name(z, i));
		if (!is_media_ext(lower_ext(name)))
			continue;
		uint8_t* d;
		uint32_t n;
		if (zipread_extract(z, i, &d, &n, 64u << 20) == 0) {
			std::string dst = dir + "/" + name;
			if (write_file(dst, d, n))
				out = dst;
			free(d);
		}
	}
	zipread_close(z);
	return out;
}

// A path to open: a zip's first media file, unpacked into the media folder.
static std::string resolve_media(a800session* s, const char* path)
{
	if (lower_ext(path) == "zip") {
		std::string got = unpack_first_media(path, s->media_dir);
		if (!got.empty())
			return got;
	}
	return path;
}

int a800session_media_kind(a800session* s, const char* path)
{
	if (!path || !*path)
		return A800_MEDIA_UNKNOWN;
	if (lower_ext(path) == "zip") {
		if (roms_identify_file(path) >= 0)
			return A800_MEDIA_ROM;
		zipread* z = zipread_open(path);
		int kind = A800_MEDIA_UNKNOWN;
		if (z) {
			for (int i = 0; i < zipread_count(z) && kind == A800_MEDIA_UNKNOWN; i++) {
				std::string e = lower_ext(zipread_name(z, i));
				if (e == "atr" || e == "xfd" || e == "dcm" || e == "pro" || e == "atx")
					kind = A800_MEDIA_DISK;
				else if (e == "xex" || e == "com" || e == "exe")
					kind = A800_MEDIA_EXECUTABLE;
				else if (e == "car" || e == "rom" || e == "bin")
					kind = A800_MEDIA_CARTRIDGE;
				else if (e == "cas")
					kind = A800_MEDIA_TAPE;
				else if (e == "bas" || e == "lst")
					kind = A800_MEDIA_BASIC;
			}
			zipread_close(z);
		}
		return kind;
	}
	(void)s;
	return kind_of_file(path);
}

// ---------------------------------------------------------------------------
// the SD folder
// ---------------------------------------------------------------------------

int a800session_import_to_sd(a800session* s, const char* src, char* dest_out, int dest_sz)
{
	std::string sd = s->fujinet_sd, first;
	paths_mkdir_p(s->fujinet_sd);
	zipread* z = lower_ext(src) == "zip" ? zipread_open(src) : nullptr;
	if (z) {
		for (int i = 0; i < zipread_count(z); i++) {
			std::string name = base_name(zipread_name(z, i));
			uint8_t* d;
			uint32_t n;
			if (name.empty() || zipread_extract(z, i, &d, &n, 64u << 20) != 0)
				continue;
			std::string dst = sd + "/" + name;
			if (write_file(dst, d, n) && first.empty())
				first = dst;
			free(d);
		}
		zipread_close(z);
		if (first.empty())
			return -1;
	}
	else {
		first = sd + "/" + base_name(src);
		if (!copy_file(src, first))
			return -1;
	}
	if (dest_out && dest_sz > 0)
		snprintf(dest_out, (size_t)dest_sz, "%s", first.c_str());
	return 0;
}

// Copies into the SD folder and mounts in FujiNet's drive slot.
static int fujinet_mount(a800session* s, int drive, const char* path, int ro, char* msg, int msgsz)
{
	char dest[A800_PATH_MAX];
	std::string src = resolve_media(s, path);
	if (a800session_import_to_sd(s, src.c_str(), dest, sizeof dest) != 0) {
		set_msg(msg, msgsz, "Could not copy it to the SD folder");
		return -1;
	}
	std::string on_sd = "/" + base_name(dest);
	if (fujinet_api_mount(drive + 1, 0, on_sd.c_str(), !ro, msg, msgsz) != 0)
		return -1;
	set_msg(msg, msgsz, base_name(dest) + " is in FujiNet's D" + std::to_string(drive + 1) + ":");
	return 0;
}

// ---------------------------------------------------------------------------
// drives
// ---------------------------------------------------------------------------

int a800session_drive_info(a800session* s, int drive, a800_drive_info* out)
{
	SessionImpl* im = Impl(s);
	if (drive < 0 || drive >= A800_DRIVES || !out)
		return -1;
	memset(out, 0, sizeof *out);
	if (s->fujinet_wanted) {
		fujinet_drive d[16];
		int n = fujinet_api_drives(d, 16);
		out->fujinet = 1;
		for (int i = 0; i < n; i++)
			if (d[i].slot == drive + 1 && d[i].mounted) {
				snprintf(out->path, sizeof out->path, "%s%s", d[i].host_slot == 0 ? "SD:" : "",
				         d[i].path);
				out->read_only = !d[i].writable;
			}
		return n < 0 ? -1 : 0;
	}
	std::lock_guard<std::mutex> lock(im->mediaMtx);
	snprintf(out->path, sizeof out->path, "%s", im->drive[drive].c_str());
	out->read_only = im->driveRO[drive];
	return 0;
}

int a800session_drive_insert(a800session* s, int drive, const char* path, int read_only,
                             char* msg, int msgsz)
{
	SessionImpl* im = Impl(s);
	if (drive < 0 || drive >= A800_DRIVES || !path)
		return -1;
	if (s->fujinet_wanted)
		return fujinet_mount(s, drive, path, read_only, msg, msgsz);
	std::string p = resolve_media(s, path);
	int ok = 0;
	im->host.Call([&] { ok = SIO_Mount(drive + 1, p.c_str(), read_only); });
	if (!ok) {
		set_msg(msg, msgsz, "Not a disk image atari800 can open");
		return -1;
	}
	std::lock_guard<std::mutex> lock(im->mediaMtx);
	im->drive[drive] = p;
	im->driveRO[drive] = read_only != 0;
	set_msg(msg, msgsz, base_name(p) + " is in D" + std::to_string(drive + 1) + ":");
	return 0;
}

int a800session_drive_eject(a800session* s, int drive)
{
	SessionImpl* im = Impl(s);
	if (drive < 0 || drive >= A800_DRIVES)
		return -1;
	if (s->fujinet_wanted)
		return fujinet_api_eject(drive + 1);
	im->host.Call([drive] { SIO_Dismount(drive + 1); });
	std::lock_guard<std::mutex> lock(im->mediaMtx);
	im->drive[drive].clear();
	return 0;
}

int a800session_drive_set_read_only(a800session* s, int drive, int ro)
{
	SessionImpl* im = Impl(s);
	a800_drive_info info;
	if (a800session_drive_info(s, drive, &info) != 0 || !info.path[0])
		return -1;
	if (s->fujinet_wanted) {
		const char* p = info.path;
		if (!strncmp(p, "SD:", 3))
			p += 3;
		return fujinet_api_mount(drive + 1, 0, p, !ro, nullptr, 0);
	}
	std::string path = info.path;
	int ok = 0;
	im->host.Call([&] { ok = SIO_Mount(drive + 1, path.c_str(), ro); });
	std::lock_guard<std::mutex> lock(im->mediaMtx);
	im->driveRO[drive] = ro != 0;
	return ok ? 0 : -1;
}

int a800session_new_blank_disk(const char* path, int density)
{
	// single (720 x 128), enhanced (1040 x 128), double (3 x 128 + 717 x 256)
	unsigned sectors = density == 1 ? 1040 : 720;
	unsigned secsize = density == 2 ? 256 : 128;
	unsigned long bytes = density == 2 ? 3 * 128 + 717ul * 256 : (unsigned long)sectors * 128;
	unsigned long paras = bytes / 16;
	uint8_t hdr[16] = {0x96, 0x02, (uint8_t)paras, (uint8_t)(paras >> 8), (uint8_t)secsize,
	                   (uint8_t)(secsize >> 8), (uint8_t)(paras >> 16)};
	FILE* fp = fopen(path, "wb");
	if (!fp)
		return -1;
	bool ok = fwrite(hdr, 1, 16, fp) == 16;
	static const uint8_t zero[256] = {0};
	for (unsigned long left = bytes; ok && left > 0;) {
		size_t n = left > sizeof zero ? sizeof zero : (size_t)left;
		ok = fwrite(zero, 1, n, fp) == n;
		left -= n;
	}
	return fclose(fp) == 0 && ok ? 0 : -1;
}

// ---------------------------------------------------------------------------
// cartridges
// ---------------------------------------------------------------------------

int a800session_cart_types_for(int kb, int* types, int max)
{
	int n = 0;
	for (int t = 1; t < CARTRIDGE_TYPE_COUNT && n < max; t++)
		if (CARTRIDGES[t].kb == kb && !CARTRIDGE_IsFor5200(t))
			types[n++] = t;
	return n;
}

const char* a800session_cart_type_name(int type)
{
	return type > 0 && type < CARTRIDGE_TYPE_COUNT ? CARTRIDGES[type].description : "";
}

int a800session_cart_needs_type(const char* path)
{
	struct stat st;
	if (AFILE_DetectFileType(path) != AFILE_ROM || stat(path, &st) != 0)
		return 0;
	return (int)(st.st_size / 1024);
}

static int insert_cart(a800session* s, const char* path, int type, bool piggyback, char* msg,
                       int msgsz)
{
	SessionImpl* im = Impl(s);
	std::string p = resolve_media(s, path);
	int rc = 0;
	bool refused = false;
	im->host.Call([&] {
		CARTRIDGE_image_t* cart = piggyback ? &CARTRIDGE_piggyback : &CARTRIDGE_main;
		rc = piggyback ? CARTRIDGE_Insert_Second(p.c_str()) : CARTRIDGE_Insert(p.c_str());
		if (rc > 0 && type > 0) {
			CARTRIDGE_SetType(cart, type);
			rc = 0;
		}
		if (rc == 0 || rc == CARTRIDGE_BAD_CHECKSUM) {
			if (CARTRIDGE_IsFor5200(cart->type)) {
				refused = true;
				if (piggyback)
					CARTRIDGE_Remove_Second();
				else
					CARTRIDGE_Remove();
			}
			else if (!piggyback) {
				im->ColdStart();
			}
		}
	});
	if (refused) {
		set_msg(msg, msgsz, "That is an Atari 5200 cartridge");
		return -1;
	}
	if (rc > 0) {
		set_msg(msg, msgsz, "Choose the cartridge type");
		return rc;   // the size in KB: the caller asks for a type
	}
	if (rc < 0 && rc != CARTRIDGE_BAD_CHECKSUM) {
		set_msg(msg, msgsz, rc == CARTRIDGE_CANT_OPEN ? "Could not open it" : "Not a cartridge image");
		return -1;
	}
	std::lock_guard<std::mutex> lock(im->mediaMtx);
	if (piggyback) {
		im->cart2 = p;
		im->cart2Type = type;
	}
	else {
		im->cart = p;
		im->cartType = type;
	}
	set_msg(msg, msgsz, base_name(p) + (piggyback ? " is in the piggyback slot" : " is in"));
	return 0;
}

int a800session_cart_insert(a800session* s, const char* path, int type, char* msg, int msgsz)
{
	return insert_cart(s, path, type, false, msg, msgsz);
}

int a800session_cart_insert_piggyback(a800session* s, const char* path, int type, char* msg,
                                      int msgsz)
{
	return insert_cart(s, path, type, true, msg, msgsz);
}

int a800session_cart_remove(a800session* s)
{
	SessionImpl* im = Impl(s);
	im->host.Call([im] {
		CARTRIDGE_Remove();
		im->ColdStart();
	});
	std::lock_guard<std::mutex> lock(im->mediaMtx);
	im->cart.clear();
	im->cartType = 0;
	return 0;
}

int a800session_cart_remove_piggyback(a800session* s)
{
	SessionImpl* im = Impl(s);
	im->host.Call([] { CARTRIDGE_Remove_Second(); });
	std::lock_guard<std::mutex> lock(im->mediaMtx);
	im->cart2.clear();
	im->cart2Type = 0;
	return 0;
}

int a800session_cart_info(a800session* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	std::string out;
	im->host.Call([&] {
		if (CARTRIDGE_main.type > 0)
			out = base_name(CARTRIDGE_main.filename) + " (" + CARTRIDGES[CARTRIDGE_main.type].description + ")";
		if (CARTRIDGE_piggyback.type > 0)
			out += " + " + base_name(CARTRIDGE_piggyback.filename);
	}, A800Host::Kind::Any);
	if (dst && dstsz > 0)
		snprintf(dst, (size_t)dstsz, "%s", out.c_str());
	return (int)out.size();
}

// ---------------------------------------------------------------------------
// the program recorder
// ---------------------------------------------------------------------------

int a800session_tape_insert(a800session* s, const char* path, char* msg, int msgsz)
{
	SessionImpl* im = Impl(s);
	std::string p = resolve_media(s, path);
	if (s->fujinet_wanted) {
		// FujiNet's own tape (its web UI mounts a .cas in slot 8)
		return fujinet_mount(s, 7, p.c_str(), 1, msg, msgsz);
	}
	int ok = 0;
	im->host.Call([&] { ok = CASSETTE_Insert(p.c_str()); });
	if (!ok) {
		set_msg(msg, msgsz, "Not a tape image");
		return -1;
	}
	std::lock_guard<std::mutex> lock(im->mediaMtx);
	im->tape = p;
	set_msg(msg, msgsz, base_name(p) + " is in the program recorder");
	return 0;
}

int a800session_tape_record_new(a800session* s, const char* path)
{
	SessionImpl* im = Impl(s);
	int ok = 0;
	if (s->fujinet_wanted)
		return -1;
	im->host.Call([&] {
		ok = CASSETTE_CreateCAS(path, "") && CASSETTE_Insert(path);
		if (ok && !CASSETTE_record)
			CASSETTE_ToggleRecord();
	});
	if (ok) {
		std::lock_guard<std::mutex> lock(im->mediaMtx);
		im->tape = path;
	}
	return ok ? 0 : -1;
}

int a800session_tape_rewind(a800session* s)
{
	Impl(s)->host.Call([] { CASSETTE_Seek(1); });
	return 0;
}

int a800session_tape_eject(a800session* s)
{
	SessionImpl* im = Impl(s);
	im->host.Call([] { CASSETTE_Remove(); });
	std::lock_guard<std::mutex> lock(im->mediaMtx);
	im->tape.clear();
	return 0;
}

int a800session_tape_boot(a800session* s)
{
	SessionImpl* im = Impl(s);
	im->host.Call([im] {
		CASSETTE_hold_start = 1;
		im->ColdStart();
	});
	return 0;
}

int a800session_tape_position(a800session* s, int* block, int* blocks)
{
	SessionImpl* im = Impl(s);
	unsigned pos = 0, size = 0;
	im->host.Call([&] {
		pos = CASSETTE_GetPosition();
		size = CASSETTE_GetSize();
	}, A800Host::Kind::Any);
	if (block)
		*block = (int)pos;
	if (blocks)
		*blocks = (int)size;
	return 0;
}

// ---------------------------------------------------------------------------
// programs, states, Open
// ---------------------------------------------------------------------------

int a800session_states_available(a800session* s)
{
	return !s->fujinet_wanted;
}

int a800session_state_save(a800session* s, const char* path)
{
	int ok = 0;
	if (!a800session_states_available(s))
		return -1;
	Impl(s)->host.Call([&] { ok = StateSav_SaveAtariState(path, "wb", TRUE); });
	return ok ? 0 : -1;
}

int a800session_state_load(a800session* s, const char* path)
{
	int ok = 0;
	if (!a800session_states_available(s))
		return -1;
	Impl(s)->host.Call([&] { ok = StateSav_ReadAtariState(path, "rb"); });
	return ok ? 0 : -1;
}

int a800session_open(a800session* s, const char* path, char* msg, int msgsz)
{
	SessionImpl* im = Impl(s);
	int kind = a800session_media_kind(s, path);
	std::string p;
	int ok = 0;
	switch (kind) {
	case A800_MEDIA_ROM:
		return a800session_rom_import(s, path, -1, msg, msgsz) > 0 ? 0 : -1;
	case A800_MEDIA_DISK:
		if (a800session_drive_insert(s, 0, path, 0, msg, msgsz) != 0)
			return -1;
		im->host.Call([im] { im->ColdStart(); });
		return 0;
	case A800_MEDIA_EXECUTABLE:
	case A800_MEDIA_BASIC:
		p = resolve_media(s, path);
		im->host.Call([&] { ok = BINLOAD_Loader(p.c_str()); });
		set_msg(msg, msgsz, ok ? "Running " + base_name(p) : "Not a program atari800 can load");
		return ok ? 0 : -1;
	case A800_MEDIA_CARTRIDGE: {
		int rc = a800session_cart_insert(s, path, 0, msg, msgsz);
		return rc > 0 ? 1 : rc;
	}
	case A800_MEDIA_TAPE:
		return a800session_tape_insert(s, path, msg, msgsz);
	case A800_MEDIA_STATE:
		if (!a800session_states_available(s)) {
			set_msg(msg, msgsz, "Save states need FujiNet off (Preferences > FujiNet)");
			return -1;
		}
		ok = a800session_state_load(s, path) == 0;
		set_msg(msg, msgsz, ok ? "State loaded" : "Could not load the state");
		return ok ? 0 : -1;
	default:
		set_msg(msg, msgsz, "Not something the Atari can use");
		return -1;
	}
}

// With FujiNet on: eject what Open put in D1 and come back to CONFIG.
int a800session_reboot_to_config(a800session* s)
{
	if (s->fujinet_wanted)
		fujinet_api_eject(1);
	return a800session_power_cycle(s);
}
