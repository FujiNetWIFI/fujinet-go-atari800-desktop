/*
 * A800Host -- atari800 on a thread of its own, with the family's contracts
 * around it: a serial-stamped XRGB frame slot, pacing to the frontend's
 * vsync (or the wall clock), a resampled stereo audio ring the SDL device
 * pulls, and a job queue for everything that must touch the emulator.
 *
 * atari800 is a single global instance: every core call runs on the
 * emulation thread, in a job or a callback. Jobs come in two kinds:
 *   Frame   between frames only (power, media, machine changes)
 *   Any     also while the debugger holds the machine (inspection)
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A800_HOST_H
#define A800_HOST_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>

class A800Host
{
public:
	enum class Kind { Frame, Any };

	struct Callbacks
	{
		// emulation thread: bring the core up (Atari800_Initialise ...);
		// false fails Start() with error set
		std::function<bool(std::string& error)> init;
		// emulation thread, inside Atari800_Initialise where atari800 would
		// read its config file: apply the session's options
		std::function<void()> applyConfig;
		// emulation thread, before each frame: stage input into the core
		std::function<void()> beforeFrame;
		// emulation thread, after each frame
		std::function<void()> afterFrame;
		// emulation thread: the frame-render options
		std::function<void(int& area, bool& show80)> videoOptions;
		// emulation thread: the machine broke into the debugger; Resume() here
		// lets it go on without ever showing as stopped
		std::function<void()> onStopped;
		// emulation thread: atari800's log text
		std::function<void(const char* text)> onLog;
	};

	A800Host();
	~A800Host();
	A800Host(const A800Host&) = delete;
	A800Host& operator=(const A800Host&) = delete;

	void SetCallbacks(Callbacks cb) { _cb = std::move(cb); }

	bool Start(std::string& error);
	void Stop();
	bool IsRunning() const { return _running.load(); }
	bool Quitting() const { return _quit.load(); }
	static bool OnEmulationThread();

	// ---- jobs
	// Runs fn on the emulation thread and waits for it. false if the thread
	// is not running (fn did not run).
	bool Call(const std::function<void()>& fn, Kind kind = Kind::Frame);
	void Post(std::function<void()> fn, Kind kind = Kind::Frame);

	// ---- power: a powered-off machine runs no frames (black screen)
	void SetPowered(bool on);
	bool Powered() const { return _powered.load(); }

	// ---- video
	bool CopyFrame(uint32_t* dst, size_t maxPixels, int& width, int& height,
	               int& parMilli, uint64_t* serialInOut);
	void NotifyVsync(int64_t frameTimeNs);
	double Fps() const;                // the running TV system's
	void ClearFrame();                 // black, e.g. while powered off

	// ---- audio
	void FillAudio(float* stereo, uint32_t frames);
	void SetVolume(int percent);
	void SetMuted(bool m) { _muted.store(m); }
	void FlushAudio();

	// ---- speed
	void SetWarp(bool on) { _warp.store(on); }
	bool Warp() const { return _warp.load(); }

	// ---- the debugger's stop (from PLATFORM_Exit(TRUE))
	bool Stopped() const { return _stopped.load(); }
	// Lets a parked machine go; returns once it has left the park (or
	// parked again). A Frame job releases a parked machine too, and breaks
	// are then ignored until the frame ends.
	void Resume();
	void SuppressBreaks(bool s) { _suppressBreaks.store(s); }
	// Moves on every park and every release.
	unsigned ParkSerial() const { return _parkSerial.load(); }
	// Emulation thread, inside a job that waits while parked: run the
	// other Any jobs meanwhile.
	void RunParkedJobs() { RunJobs(true); }

	// core-side callbacks (a8host.h)
	void OnSound(const void* pcm, unsigned bytes);
	void OnBreak();
	void OnConfig();

private:
	static void* ThreadMain(void* self);
	void ThreadBody();
	void RunJobs(bool parked);
	void Pace();
	void PublishFrame();
	void DrainLog();

	Callbacks _cb;
	pthread_t _thread {};
	bool _threadStarted = false;
	std::atomic<bool> _running { false };
	std::atomic<bool> _quit { false };
	std::atomic<bool> _powered { true };
	std::atomic<bool> _warp { false };
	std::atomic<bool> _stopped { false };
	std::atomic<bool> _resume { false };
	std::atomic<bool> _suppressBreaks { false };
	std::atomic<bool> _breakHold { false };     // until the frame ends
	std::atomic<unsigned> _parkSerial { 0 };

	std::mutex _startLock;
	std::condition_variable _startCv;
	int _startState = 0;               // 0 starting, 1 running, -1 failed
	std::string _startError;

	// frame slot
	mutable std::mutex _frameLock;
	std::vector<uint32_t> _frame;
	std::vector<uint32_t> _render;
	int _frameW = 336, _frameH = 240, _framePar = 1000;
	uint64_t _frameSerial = 0;

	// pacing
	std::mutex _vsyncLock;
	std::condition_variable _vsyncCv;
	int64_t _vsyncLastNs = 0;
	int64_t _vsyncPeriodNs = 0;
	uint64_t _vsyncSerial = 0;
	int64_t _nextFrameNs = 0;

	// audio: SPSC stereo int16 ring
	static constexpr uint32_t AudioRingFrames = 16384;
	static constexpr uint32_t AudioTargetFrames = 2400;   // 50 ms
	std::vector<int16_t> _ring;
	std::atomic<uint32_t> _ringRead { 0 };
	std::atomic<uint32_t> _ringWrite { 0 };
	std::atomic<int> _volume { 100 };
	std::atomic<bool> _muted { false };
	std::atomic<bool> _flush { false };
	double _resamplePos = 0.0;
	int16_t _lastSample[2] = { 0, 0 };
	std::vector<int16_t> _stereo;

	// jobs
	std::mutex _jobLock;
	std::condition_variable _jobCv;
	std::deque<std::function<void()>> _frameJobs;
	std::deque<std::function<void()>> _anyJobs;
	std::atomic<bool> _jobsPending { false };
	uint64_t _jobsDone = 0;
};

#endif
