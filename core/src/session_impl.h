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
#include "input_merge.h"
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

	// ---- joysticks, console keys: one hold per SOURCE (input_merge.h), merged
	std::atomic<unsigned> joySrc[A800_PORTS][A800_SRC_COUNT];   // bit per a800_action
	std::atomic<unsigned> conSrc[A800_SRC_COUNT];               // bit per a800_console
	unsigned MergedJoy(int port) const;         // all sources, opposed pairs cancelled
	unsigned MergedConsole() const;             // all sources
	bool resetHeld = false;
	int PortDevice(int port);
	std::atomic<int> portDevice[A800_PORTS] = { {0}, {0}, {0}, {0} };

	// paddles: 0..A800_POT_MAX per POT line, fire per source; from gamepads or the API
	std::atomic<int> pot[8] = { {A800_POT_IDLE}, {A800_POT_IDLE}, {A800_POT_IDLE}, {A800_POT_IDLE},
	                            {A800_POT_IDLE}, {A800_POT_IDLE}, {A800_POT_IDLE}, {A800_POT_IDLE} };
	std::atomic<int> potSrc[8];                 // the source that wrote the line last, -1 none
	std::atomic<unsigned> fireSrc[A800_SRC_COUNT];   // paddle fire, bit per POT line
	std::atomic<int> turnDir[8];                // the window's turn arrows: -1 left, 0, +1 right
	unsigned MergedPaddleFire() const;
	// the pointer for mice, Koala Pad, light pen and gun
	std::atomic<int> ptrX { 0 }, ptrY { 0 }, ptrInside { 0 };
	std::atomic<unsigned> ptrButtons { 0 };
	std::atomic<int> ptrDx { 0 }, ptrDy { 0 };
	std::atomic<int> mouseSpeed { 3 };
	// an absolute device's pad drawn in the window: a point of its surface (0..1)
	std::atomic<int> ptrNorm { 0 };             // 1: ptrU/ptrV are the position (else ptrX/ptrY)
	std::atomic<int> ptrU { 0 }, ptrV { 0 };    // thousandths

	// ---- the keyboard
	struct Held { uint32_t keysym; int akey; };
	std::mutex keyMtx;
	std::vector<Held> held;
	int modShift = 0, modCtrl = 0;
	uint32_t heldKeysym[A800_TARGET_COUNT] = {};   // keyMtx: the key that holds each target,
	                                               // so its release lets go whatever it is bound to now
	std::atomic<int> keyCode { -1 };
	std::atomic<int> keyShift { 0 };
	std::atomic<bool> positional { false };
	std::atomic<bool> fkeys1200 { false };
	std::atomic<int> oskKey { -1 };             // on-screen keyboard's
	int oskFrames = 0;                          // pasteMtx: frames it has been seen
	bool oskUp = false;                         // pasteMtx: let go, held for its frames
	static constexpr int KeyMinFrames = 3;
	static constexpr int TurnStep = 3;          // POT counts per frame a turn arrow is held
	std::atomic<int> keyFrames { 0 };           // frames the key has been seen
	std::atomic<int> tapCode { -1 }, tapShift { 0 }, tapLeft { 0 };
	void UpdateKey();
	bool cx85Fitted();

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
/* input.cpp */
int  session_binding_target_for_key(struct a800session* s, uint32_t keysym);
/* debug.cpp */
void session_debug_stopped(struct a800session* s);
void session_debug_pause(struct a800session* s);
void session_debug_free(struct a800session* s);
void session_debug_after_frame(void);
void session_debug_trace_install(struct a800session* s);
}

#endif
