/*
 * SessionImpl -- the C++ half of a800session, shared by session.cpp,
 * input.cpp, media.cpp and debug.cpp.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A800_SESSION_IMPL_H
#define A800_SESSION_IMPL_H

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "A800Host.h"
#include "session_internal.h"

struct SessionImpl
{
	explicit SessionImpl(a800session* s);

	a800session* s;
	A800Host host;
	bool coreUp = false;

	// ---- settings applied at the last start or apply
	std::vector<std::string> applied;
	std::string appliedRoms;
	std::string PrefValue(int i);
	void SnapshotApplied();
	std::string RomsSignature();
	std::string ArgvSignature();
	std::string appliedArgv;

	// ---- emulation-thread work
	bool CoreInit(std::string& err);
	bool CoreReinit(std::string& err);
	void ApplyLive();
	void ApplyVideo();
	void ApplyPorts();
	void ColdStart();
	void RemountMedia();
	void BeforeFrame();
	void AfterFrame();
	void DevicesBeforeFrame();
	void OnLog(const char* text);
	bool suppressReset = false;

	// ---- joysticks, console keys
	std::atomic<unsigned> joy[A800_PORTS];      // frontends, keyboard bindings
	std::atomic<unsigned> joyPad[A800_PORTS];   // gamepads
	std::atomic<unsigned> console { 0 };
	bool resetHeld = false;
	int PortDevice(int port);
	std::atomic<int> portDevice[A800_PORTS] = { {0}, {0}, {0}, {0} };

	// paddles: 0..228 per POT line, fire per port; from gamepads or the API
	std::atomic<int> pot[8] = { {228}, {228}, {228}, {228}, {228}, {228}, {228}, {228} };
	std::atomic<unsigned> paddleFire { 0 };     // bit per POT line
	// the pointer for mice, Koala Pad, light pen and gun
	std::atomic<int> ptrX { 0 }, ptrY { 0 }, ptrInside { 0 };
	std::atomic<unsigned> ptrButtons { 0 };
	std::atomic<int> ptrDx { 0 }, ptrDy { 0 };
	std::atomic<int> mouseSpeed { 3 };

	// ---- the keyboard
	struct Held { uint32_t keysym; int akey; };
	std::mutex keyMtx;
	std::vector<Held> held;
	int modShift = 0, modCtrl = 0;
	std::atomic<int> keyCode { -1 };
	std::atomic<int> keyShift { 0 };
	std::atomic<bool> positional { false };
	std::atomic<bool> fkeys1200 { false };
	std::atomic<int> oskKey { -1 };             // on-screen keyboard's
	void UpdateKey();

	// ---- paste
	std::mutex pasteMtx;
	std::vector<int> pasteQueue;                // AKEY codes, -2 = Return pause
	size_t pastePos = 0;
	int pasteKey = -1;                          // what the paste holds this frame
	unsigned pasteConsole = 0;
	int pastePhase = 0, pasteWait = 0;
	void PasteStep();

	// ---- media, kept to remount after a re-initialisation
	std::mutex mediaMtx;
	std::string drive[A800_DRIVES];
	bool driveRO[A800_DRIVES] = {};
	std::string cart, cart2, tape;
	int cartType = 0, cart2Type = 0;

	// ---- video
	std::atomic<int> area { 1 };
	std::atomic<bool> show80col { false };

	// ---- log and notices
	std::mutex logMtx;
	std::string logText;
	std::mutex noticeMtx;
	std::string notice;
	void Notice(const std::string& text);
};

SessionImpl* Impl(a800session* s);

extern "C" {
/* fujinet_runtime.c */
int  session_fujinet_available(struct a800session* s);
int  session_fujinet_start(struct a800session* s);
void session_fujinet_stop(struct a800session* s);
void session_fujinet_restart_for_power(struct a800session* s);
void session_fujinet_power_cycled(struct a800session* s);
/* input.cpp */
int  session_binding_target_for_key(struct a800session* s, uint32_t keysym);
/* debug.cpp */
void session_debug_stopped(struct a800session* s);
void session_debug_pause(struct a800session* s);
void session_debug_free(struct a800session* s);
}

#endif
