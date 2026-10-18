/*
 * A800Host -- see A800Host.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif

#include "A800Host.h"

extern "C" {
#include "config.h"
#include "atari.h"
#include "log.h"
#include "screen.h"
#include "sound.h"
#include "a8host.h"
#include "a8video.h"
}

namespace {

int64_t MonoNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A vsync stream counts as live if a tick arrived this recently; asked before
// blocking, so a frontend without a frame clock never waits on one.
constexpr int64_t VsyncRecentNs = 250'000'000;

// macOS gives secondary threads 512K; the core and its monitor want more.
constexpr size_t EmulationThreadStack = 8u << 20;

constexpr size_t MaxPixels = 672 * 300;

thread_local bool tOnEmulationThread = false;

A800Host* gHost = nullptr;

void CbSound(void*, const void* pcm, unsigned bytes)
{
	if (gHost)
		gHost->OnSound(pcm, bytes);
}

void CbBreak(void*)
{
	if (gHost)
		gHost->OnBreak();
}

void CbConfig(void*)
{
	if (gHost)
		gHost->OnConfig();
}

} // namespace

A800Host::A800Host()
	: _frame(MaxPixels, 0xff000000u), _render(MaxPixels, 0xff000000u),
	  _ring(AudioRingFrames * 2, 0)
{
}

A800Host::~A800Host()
{
	Stop();
}

bool A800Host::OnEmulationThread()
{
	return tOnEmulationThread;
}

bool A800Host::Start(std::string& error)
{
	if (_running.load())
		return true;
	if (gHost && gHost != this) {
		error = "another emulator is already running in this process";
		return false;
	}
	gHost = this;
	_quit.store(false);
	_stopped.store(false);
	_startState = 0;
	_startError.clear();

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, EmulationThreadStack);
	_running.store(true);
	if (pthread_create(&_thread, &attr, &A800Host::ThreadMain, this) != 0) {
		pthread_attr_destroy(&attr);
		_running.store(false);
		error = "could not start the emulation thread";
		return false;
	}
	pthread_attr_destroy(&attr);
	_threadStarted = true;

	std::unique_lock<std::mutex> lock(_startLock);
	_startCv.wait(lock, [this] { return _startState != 0; });
	if (_startState < 0) {
		lock.unlock();
		error = _startError;
		Stop();
		return false;
	}
	return true;
}

void A800Host::Stop()
{
	if (!_threadStarted)
		return;
	_quit.store(true);
	_resume.store(true);
	{
		std::lock_guard<std::mutex> lock(_jobLock);
	}
	_jobCv.notify_all();
	_vsyncCv.notify_all();
	pthread_join(_thread, nullptr);
	_threadStarted = false;
	_running.store(false);
	if (gHost == this)
		gHost = nullptr;
	// whatever is still queued never runs; wake its waiters
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		_frameJobs.clear();
		_anyJobs.clear();
		_jobsPending.store(false);
	}
	_jobCv.notify_all();
}

void* A800Host::ThreadMain(void* self)
{
	static_cast<A800Host*>(self)->ThreadBody();
	return nullptr;
}

void A800Host::ThreadBody()
{
	tOnEmulationThread = true;
#if defined(_WIN32)
	timeBeginPeriod(1);
#endif
	a8host_callbacks cb = {};
	cb.sound = CbSound;
	cb.brk = CbBreak;
	cb.config = CbConfig;
	a8host_set_callbacks(&cb);

	std::string err;
	bool ok = _cb.init ? _cb.init(err) : false;
	{
		std::lock_guard<std::mutex> lock(_startLock);
		_startState = ok ? 1 : -1;
		_startError = err;
	}
	_startCv.notify_all();
	if (!ok) {
		a8host_set_callbacks(nullptr);
		tOnEmulationThread = false;
#if defined(_WIN32)
		timeEndPeriod(1);
#endif
		return;
	}

	_nextFrameNs = MonoNs();
	while (!_quit.load()) {
		RunJobs(false);
		if (_quit.load())
			break;
		if (!_powered.load()) {
			std::unique_lock<std::mutex> lock(_jobLock);
			_jobCv.wait_for(lock, std::chrono::milliseconds(50),
			                [this] { return _jobsPending.load() || _quit.load() || _powered.load(); });
			_nextFrameNs = MonoNs();
			continue;
		}
		if (_cb.beforeFrame)
			_cb.beforeFrame();
		Atari800_Frame();
		_breakHold.store(false);
		DrainLog();
		if (Atari800_display_screen)
			PublishFrame();
		if (_cb.afterFrame)
			_cb.afterFrame();
		if (!_warp.load())
			Pace();
		else
			_nextFrameNs = MonoNs();
	}

	a8host_set_callbacks(nullptr);
	tOnEmulationThread = false;
#if defined(_WIN32)
	timeEndPeriod(1);
#endif
}

void A800Host::DrainLog()
{
	if (!Log_buffer[0])
		return;
	if (_cb.onLog)
		_cb.onLog(Log_buffer);
	Log_buffer[0] = 0;
}

// ---- jobs ------------------------------------------------------------------

namespace {
struct Job
{
	std::function<void()> fn;
	bool done = false;
};
}

bool A800Host::Call(const std::function<void()>& fn, Kind kind)
{
	if (tOnEmulationThread) {
		fn();
		return true;
	}
	if (!_running.load())
		return false;
	auto job = std::make_shared<Job>();
	job->fn = fn;
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		auto wrapped = [job] { job->fn(); job->done = true; };
		if (kind == Kind::Any)
			_anyJobs.push_back(wrapped);
		else
			_frameJobs.push_back(wrapped);
		_jobsPending.store(true);
	}
	_jobCv.notify_all();
	_vsyncCv.notify_all();

	std::unique_lock<std::mutex> lock(_jobLock);
	_jobCv.wait(lock, [&] { return job->done || !_running.load() || _quit.load(); });
	return job->done;
}

void A800Host::Post(std::function<void()> fn, Kind kind)
{
	if (tOnEmulationThread && kind == Kind::Any) {
		fn();
		return;
	}
	if (!_running.load())
		return;
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		if (kind == Kind::Any)
			_anyJobs.push_back(std::move(fn));
		else
			_frameJobs.push_back(std::move(fn));
		_jobsPending.store(true);
	}
	_jobCv.notify_all();
	_vsyncCv.notify_all();
}

void A800Host::RunJobs(bool parked)
{
	if (!_jobsPending.load())
		return;
	for (;;) {
		std::function<void()> fn;
		{
			std::lock_guard<std::mutex> lock(_jobLock);
			if (!_anyJobs.empty()) {
				fn = std::move(_anyJobs.front());
				_anyJobs.pop_front();
			}
			else if (!parked && !_frameJobs.empty()) {
				fn = std::move(_frameJobs.front());
				_frameJobs.pop_front();
			}
			else {
				_jobsPending.store(!_frameJobs.empty() || !_anyJobs.empty());
				break;
			}
		}
		fn();
		{
			std::lock_guard<std::mutex> lock(_jobLock);
			_jobsDone++;
		}
		_jobCv.notify_all();
	}
}

void A800Host::SetPowered(bool on)
{
	_powered.store(on);
	if (!on) {
		ClearFrame();
		FlushAudio();
	}
	_jobCv.notify_all();
}

// ---- the debugger's stop ---------------------------------------------------

// Called inside Atari800_Frame (CPU_GO), registers synced to CPU_reg*.
void A800Host::OnBreak()
{
	if (_suppressBreaks.load() || _breakHold.load() || _quit.load())
		return;
	_resume.store(false);
	if (_cb.onStopped)
		_cb.onStopped();
	if (_resume.load())
		return;   // it let the machine go on (stepping off a breakpoint ...)
	_parkSerial.fetch_add(1);
	_stopped.store(true);
	FlushAudio();
	while (!_resume.load() && !_quit.load()) {
		RunJobs(true);
		std::unique_lock<std::mutex> lock(_jobLock);
		if (!_frameJobs.empty()) {
			// power, media: they run at the frame's end
			_breakHold.store(true);
			break;
		}
		_jobCv.wait_for(lock, std::chrono::milliseconds(100), [this] {
			return !_anyJobs.empty() || !_frameJobs.empty() || _resume.load() || _quit.load();
		});
	}
	{
		std::lock_guard<std::mutex> lock(_jobLock);
		_stopped.store(false);
		_parkSerial.fetch_add(1);
	}
	_jobCv.notify_all();
	_nextFrameNs = MonoNs();
}

void A800Host::OnConfig()
{
	if (_cb.applyConfig)
		_cb.applyConfig();
}

void A800Host::Resume()
{
	const unsigned serial = _parkSerial.load();
	const bool wasStopped = _stopped.load();
	_resume.store(true);
	_jobCv.notify_all();
	if (!wasStopped || tOnEmulationThread)
		return;
	std::unique_lock<std::mutex> lock(_jobLock);
	_jobCv.wait_for(lock, std::chrono::seconds(2), [&] {
		return _parkSerial.load() != serial || !_running.load() || _quit.load();
	});
}

// ---- video and pacing ------------------------------------------------------

void A800Host::PublishFrame()
{
	int area = A8VIDEO_AREA_TV;
	bool show80 = false;
	if (_cb.videoOptions)
		_cb.videoOptions(area, show80);
	a8video_opts o;
	o.area = area;
	o.show_80col = show80 ? 1 : 0;
	int w = 0, h = 0, par = 1000;
	if (!a8video_render(_render.data(), 672, 300, &w, &h, &par, &o))
		return;
	std::lock_guard<std::mutex> lock(_frameLock);
	memcpy(_frame.data(), _render.data(), (size_t)w * h * sizeof(uint32_t));
	_frameW = w;
	_frameH = h;
	_framePar = par;
	_frameSerial++;
}

void A800Host::ClearFrame()
{
	std::lock_guard<std::mutex> lock(_frameLock);
	std::fill(_frame.begin(), _frame.begin() + (size_t)_frameW * _frameH, 0xff000000u);
	_frameSerial++;
}

bool A800Host::CopyFrame(uint32_t* dst, size_t maxPixels, int& width, int& height,
                         int& parMilli, uint64_t* serialInOut)
{
	std::lock_guard<std::mutex> lock(_frameLock);
	if (serialInOut && *serialInOut == _frameSerial && *serialInOut != 0)
		return false;
	const size_t n = std::min((size_t)_frameW * _frameH, maxPixels);
	if (dst)
		memcpy(dst, _frame.data(), n * sizeof(uint32_t));
	width = _frameW;
	height = _frameH;
	parMilli = _framePar;
	if (serialInOut)
		*serialInOut = _frameSerial;
	return true;
}

double A800Host::Fps() const
{
	return Atari800_tv_mode == Atari800_TV_PAL ? Atari800_FPS_PAL : Atari800_FPS_NTSC;
}

void A800Host::NotifyVsync(int64_t)
{
	const int64_t now = MonoNs();
	{
		std::lock_guard<std::mutex> lock(_vsyncLock);
		if (_vsyncLastNs) {
			const int64_t period = now - _vsyncLastNs;
			_vsyncPeriodNs = _vsyncPeriodNs ? (_vsyncPeriodNs * 7 + period) / 8 : period;
		}
		_vsyncLastNs = now;
		_vsyncSerial++;
	}
	_vsyncCv.notify_all();
}

// Phase-lock to the frontend's frame clock when it runs at the Atari's own
// rate (within 5%); otherwise an absolute deadline that resyncs, rather than
// fast-forwards, when badly behind. Jobs run while waiting.
void A800Host::Pace()
{
	const int64_t frameNs = (int64_t)(1e9 / Fps());
	{
		std::unique_lock<std::mutex> lock(_vsyncLock);
		const int64_t now = MonoNs();
		const bool locked = _vsyncLastNs && now - _vsyncLastNs < VsyncRecentNs && _vsyncPeriodNs
		                    && std::fabs((double)_vsyncPeriodNs - (double)frameNs) < frameNs * 0.05;
		if (locked) {
			const uint64_t seen = _vsyncSerial;
			const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(2 * frameNs);
			while (_vsyncSerial == seen && !_quit.load()) {
				if (_jobsPending.load()) {
					lock.unlock();
					RunJobs(false);
					lock.lock();
					continue;
				}
				if (_vsyncCv.wait_until(lock, until) == std::cv_status::timeout)
					break;
			}
			_nextFrameNs = MonoNs();
			if (_vsyncSerial != seen)
				return;
		}
	}

	_nextFrameNs += frameNs;
	int64_t behind = MonoNs() - _nextFrameNs;
	if (behind > 4 * frameNs) {
		_nextFrameNs = MonoNs();
		return;
	}
	while (behind < 0 && !_quit.load()) {
		{
			std::unique_lock<std::mutex> lock(_vsyncLock);
			_vsyncCv.wait_for(lock, std::chrono::nanoseconds(-behind),
			                  [this] { return _jobsPending.load() || _quit.load(); });
		}
		if (_jobsPending.load())
			RunJobs(false);
		behind = MonoNs() - _nextFrameNs;
	}
}

// ---- audio -----------------------------------------------------------------

// atari800 makes exactly one frame of samples per frame at 48 kHz of
// emulated time; the device runs on its own clock. A linear resampler steps
// up to 0.5% faster or slower to hold the ring near its target fill.
void A800Host::OnSound(const void* pcm, unsigned bytes)
{
	const int channels = Sound_out.channels == 2 ? 2 : 1;
	const int frames = (int)(bytes / (2 * channels));
	if (frames <= 0 || _warp.load())
		return;
	const int16_t* in = static_cast<const int16_t*>(pcm);
	if (channels == 1) {
		_stereo.resize((size_t)frames * 2);
		for (int i = 0; i < frames; i++)
			_stereo[i * 2] = _stereo[i * 2 + 1] = in[i];
		in = _stereo.data();
	}

	uint32_t w = _ringWrite.load(std::memory_order_relaxed);
	const uint32_t r = _ringRead.load(std::memory_order_acquire);
	const uint32_t fill = (w + AudioRingFrames - r) % AudioRingFrames;
	const double err = ((double)fill - (double)AudioTargetFrames) / (double)AudioTargetFrames;
	const double step = 1.0 + std::max(-0.005, std::min(0.005, err * 0.005));

	auto sample = [&](int index, int ch) -> int {
		return index == 0 ? _lastSample[ch] : in[(index - 1) * 2 + ch];
	};
	double pos = _resamplePos;
	while (pos < (double)frames) {
		const int i = (int)pos;
		const double frac = pos - i;
		const uint32_t next = (w + 1) % AudioRingFrames;
		if (next != r) {
			for (int ch = 0; ch < 2; ch++) {
				const double v = sample(i, ch) + (sample(i + 1, ch) - sample(i, ch)) * frac;
				_ring[(size_t)w * 2 + ch] = (int16_t)std::lround(v);
			}
			w = next;
		}
		pos += step;
	}
	_resamplePos = pos - (double)frames;
	_lastSample[0] = in[(frames - 1) * 2];
	_lastSample[1] = in[(frames - 1) * 2 + 1];
	_ringWrite.store(w, std::memory_order_release);
}

void A800Host::FillAudio(float* out, uint32_t frames)
{
	uint32_t r = _ringRead.load(std::memory_order_relaxed);
	const uint32_t w = _ringWrite.load(std::memory_order_acquire);
	if (_flush.exchange(false))
		r = w;
	const float gain = _muted.load() ? 0.0f
	                   : (float)_volume.load(std::memory_order_relaxed) / (100.0f * 32768.0f);
	uint32_t i = 0;
	for (; i < frames && r != w; i++) {
		out[i * 2] = _ring[(size_t)r * 2] * gain;
		out[i * 2 + 1] = _ring[(size_t)r * 2 + 1] * gain;
		r = (r + 1) % AudioRingFrames;
	}
	for (; i < frames; i++)
		out[i * 2] = out[i * 2 + 1] = 0.0f;
	_ringRead.store(r, std::memory_order_release);
}

void A800Host::SetVolume(int percent)
{
	_volume.store(std::max(0, std::min(100, percent)));
}

void A800Host::FlushAudio()
{
	_flush.store(true);
}
