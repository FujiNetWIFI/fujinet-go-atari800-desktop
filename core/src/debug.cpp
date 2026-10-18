/*
 * debug.cpp -- the debugger engine (a800debug.h) over atari800's monitor.
 *
 * atari800 breaks into its monitor through PLATFORM_Exit(TRUE) from inside
 * CPU_GO; A800Host parks the emulation thread there and runs "Any" jobs
 * while parked, so everything below runs on the emulation thread with the
 * registers in CPU_reg*. Breakpoints are encoded into atari800's condition
 * table (monitor.h), which it checks before each instruction unless it is
 * single-stepping: resuming from a table breakpoint steps over it first.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "a800debug.h"
#include "session_impl.h"

extern "C" {
#include "antic.h"
#include "atari.h"
#include "cartridge.h"
#include "cartridge_info.h"
#include "cpu.h"
#include "gtia.h"
#include "memory.h"
#include "monitor.h"
#include "pia.h"
#include "pokey.h"
#include "sio.h"
#include "statesav.h"
#include "a8video.h"
}

namespace {

enum class Intent { None, Pause, Step, Over, Out, Frame, RunTo, Monitor };

struct Bp
{
	int id;
	int type;
	uint16_t start, end;
	bool enabled;
	std::string condition;
};

// One parsed condition: a register or memory byte against a value.
struct Cond
{
	int what;       // MONITOR_BREAKPOINT_A ... or MEMORY
	int ops;        // LESS | EQUAL | GREATER: any of them holding passes
	int value;
	int m_addr;
};

constexpr int FrameLine = 248;      // ANTIC's first line of vertical blank
constexpr int YposOff = 999;
constexpr uint16_t BreakAddrOff = 0xd000;
constexpr long ConsoleKeep = 256 * 1024;

} // namespace

struct a800debug
{
	a800session* s = nullptr;
	SessionImpl* im = nullptr;
	std::atomic<bool> attached { false };
	std::atomic<unsigned> generation { 1 };

	std::mutex mtx;
	std::vector<Bp> bps;
	int nextBp = 1, nextWp = 1001;
	std::string reason;
	int reasonPc = -1;
	Intent intent = Intent::None;
	uint16_t runTo = 0;
	bool atTableBp = false;      // stopped by the condition table
	bool skipping = false;       // stepping off it
	bool frameChase = false;     // frame step begun on the frame line

	// the monitor console: written and read on the emulation thread
	std::mutex cmdMtx;
	std::condition_variable cmdCv;
	FILE* out = nullptr;
	long outRead = 0;
	bool cmdRunning = false, cmdDone = false, waitingInput = false, haveInput = false;
	int cmdRc = 0;
	std::string input;

	// the SIO trace
	std::deque<a800debug_sio> sio;

	std::string snapshot[4];
	bool saved[4] = {};
};

namespace {

a800debug* g_debug;   // for the SIO hook and the monitor's input hook
UWORD g_dlistStart;   // the display list this frame began with

// ANTIC_dlist is where ANTIC is fetching: the list's start only once the
// JVB has run (vertical blank, where the OS also rewrites it).
UWORD dlist_start()
{
	return ANTIC_ypos >= FrameLine ? ANTIC_dlist : g_dlistStart;
}

std::string fmt(const char* f, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof buf, f, ap);
	va_end(ap);
	return buf;
}

// ---- breakpoints into atari800's table ------------------------------------

bool parse_conditions(const std::string& text, std::vector<Cond>& out, std::string& err)
{
	size_t pos = 0;
	while (pos < text.size()) {
		size_t amp = text.find("&&", pos);
		std::string part = text.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
		pos = amp == std::string::npos ? text.size() : amp + 2;
		part.erase(std::remove(part.begin(), part.end(), ' '), part.end());
		if (part.empty())
			continue;
		for (auto& c : part)
			c = (char)toupper((unsigned char)c);
		Cond c {0, 0, 0, 0};
		size_t i = 0;
		if (part.rfind("MEM[", 0) == 0) {
			size_t close = part.find(']');
			if (close == std::string::npos) {
				err = "MEM[addr] needs its ]";
				return false;
			}
			std::string a = part.substr(4, close - 4);
			if (!a.empty() && a[0] == '$')
				a.erase(0, 1);
			c.what = MONITOR_BREAKPOINT_MEMORY;
			c.m_addr = (int)strtol(a.c_str(), nullptr, 16);
			i = close + 1;
		}
		else {
			switch (part[0]) {
			case 'A': c.what = MONITOR_BREAKPOINT_A; break;
			case 'X': c.what = MONITOR_BREAKPOINT_X; break;
			case 'Y': c.what = MONITOR_BREAKPOINT_Y; break;
			case 'S': c.what = MONITOR_BREAKPOINT_S; break;
			default:
				err = "conditions are on A, X, Y, S or MEM[addr]";
				return false;
			}
			i = 1;
		}
		std::string op;
		while (i < part.size() && strchr("<>=!", part[i]))
			op += part[i++];
		if (op == "=" || op == "==")
			c.ops = MONITOR_BREAKPOINT_EQUAL;
		else if (op == "<")
			c.ops = MONITOR_BREAKPOINT_LESS;
		else if (op == ">")
			c.ops = MONITOR_BREAKPOINT_GREATER;
		else if (op == "<=")
			c.ops = MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL;
		else if (op == ">=")
			c.ops = MONITOR_BREAKPOINT_GREATER | MONITOR_BREAKPOINT_EQUAL;
		else if (op == "!=" || op == "<>")
			c.ops = MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_GREATER;
		else {
			err = "compare with =, !=, <, >, <= or >=";
			return false;
		}
		std::string v = part.substr(i);
		int base = 10;
		if (!v.empty() && v[0] == '$') {
			v.erase(0, 1);
			base = 16;
		}
		char* end = nullptr;
		long value = v.empty() ? -1 : strtol(v.c_str(), &end, base);
		if (v.empty() || *end || value < 0 || value > 0xffff) {
			err = "compare with a number ($10 is hex)";
			return false;
		}
		c.value = (int)value;
		out.push_back(c);
	}
	return true;
}

// The entries a breakpoint needs, appended to rows.
void encode(const Bp& b, std::vector<MONITOR_breakpoint_cond>& rows)
{
	auto add = [&](int cond, int value, int m_addr) {
		MONITOR_breakpoint_cond r;
		r.enabled = 1;
		r.condition = (UWORD)cond;
		r.value = (UWORD)value;
		r.m_addr = (UWORD)m_addr;
		rows.push_back(r);
	};
	int what = b.type == A800DEBUG_BP_EXEC ? MONITOR_BREAKPOINT_PC
	           : b.type == A800DEBUG_BP_READ ? MONITOR_BREAKPOINT_READ
	           : b.type == A800DEBUG_BP_WRITE ? MONITOR_BREAKPOINT_WRITE
	                                          : MONITOR_BREAKPOINT_ACCESS;
	if (!rows.empty())
		add(MONITOR_BREAKPOINT_OR, 0, 0);
	if (b.start == b.end)
		add(what | MONITOR_BREAKPOINT_EQUAL, b.start, 0);
	else {
		add(what | MONITOR_BREAKPOINT_GREATER | MONITOR_BREAKPOINT_EQUAL, b.start, 0);
		add(what | MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL, b.end, 0);
	}
	std::vector<Cond> conds;
	std::string err;
	parse_conditions(b.condition, conds, err);
	for (auto& c : conds)
		add(c.what | c.ops, c.value, c.m_addr);
}

std::vector<MONITOR_breakpoint_cond> encode_all(const std::vector<Bp>& bps)
{
	std::vector<MONITOR_breakpoint_cond> rows;
	for (auto& b : bps)
		if (b.enabled)
			encode(b, rows);
	return rows;
}

// Emulation thread: the table from the list (empty while detached).
void arm(a800debug* d)
{
	std::vector<MONITOR_breakpoint_cond> rows;
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		if (d->attached.load())
			rows = encode_all(d->bps);
	}
	int n = (int)std::min<size_t>(rows.size(), MONITOR_BREAKPOINT_TABLE_MAX);
	for (int i = 0; i < n; i++)
		MONITOR_breakpoint_table[i] = rows[i];
	MONITOR_breakpoint_table_size = n;
	MONITOR_breakpoints_enabled = TRUE;
}

void clear_breaks()
{
	MONITOR_break_step = FALSE;
	MONITOR_break_ret = FALSE;
	MONITOR_break_addr = BreakAddrOff;
	ANTIC_break_ypos = YposOff;
}

void bump(a800debug* d)
{
	d->generation.fetch_add(1);
}

// The address the instruction at pc reads or writes, as cpu.c works it out
// for the table.
int effective_address(uint16_t pc, int* optype)
{
	UBYTE insn = MEMORY_SafeGetByte(pc);
	UBYTE ot = MONITOR_optype6502[insn];
	auto byte = [&](int off) { return MEMORY_SafeGetByte((UWORD)(pc + off)); };
	auto word = [&](int off) { return byte(off) | (byte(off + 1) << 8); };
	auto zword = [](int zp) { return MEMORY_SafeGetByte((UWORD)zp) | (MEMORY_SafeGetByte((UWORD)((zp + 1) & 0xff)) << 8); };
	*optype = ot;
	switch (ot >> 4) {
	case 1: return word(1);
	case 2: return byte(1);
	case 3: return (word(1) + CPU_regX) & 0xffff;
	case 4: return (word(1) + CPU_regY) & 0xffff;
	case 5: return zword((byte(1) + CPU_regX) & 0xff);
	case 6: return (zword(byte(1)) + CPU_regY) & 0xffff;
	case 7: return (byte(1) + CPU_regX) & 0xff;
	case 8: return (byte(1) + CPU_regY) & 0xff;
	default: return -1;
	}
}

// Which of ours the table fired for (under d->mtx).
std::string table_reason(a800debug* d, uint16_t pc)
{
	for (auto& b : d->bps)
		if (b.enabled && b.type == A800DEBUG_BP_EXEC && pc >= b.start && pc <= b.end)
			return fmt("Breakpoint %d", b.id);
	int optype = 0;
	int ea = effective_address(pc, &optype);
	for (auto& b : d->bps) {
		if (!b.enabled || b.type == A800DEBUG_BP_EXEC || ea < b.start || ea > b.end)
			continue;
		bool reads = (optype & 4) && (b.type & A800DEBUG_BP_READ);
		bool writes = (optype & 8) && (b.type & A800DEBUG_BP_WRITE);
		if (reads || writes)
			return fmt("Watchpoint %d: %s $%04X", b.id, writes ? "write" : "read", ea);
	}
	return "Breakpoint";
}

// ---- the SIO trace -----------------------------------------------------------

const char* sio_command_name(uint8_t dev, uint8_t cmd)
{
	if (dev >= 0x31 && dev <= 0x38) {
		switch (cmd) {
		case 0x52: return "READ SECTOR";
		case 0x50: return "PUT SECTOR";
		case 0x57: return "WRITE SECTOR";
		case 0x53: return "STATUS";
		case 0x21: return "FORMAT";
		case 0x22: return "FORMAT ENHANCED";
		case 0x4e: return "READ PERCOM";
		case 0x4f: return "WRITE PERCOM";
		case 0x3f: return "HSIO INDEX";
		default: break;
		}
	}
	if (dev == 0x70)
		return "FUJINET";
	if (dev >= 0x71 && dev <= 0x78)
		return "NETWORK";
	return "";
}

void sio_hook(const UBYTE* frame, int path)
{
	a800debug* d = g_debug;
	if (!d || !d->attached.load())
		return;
	a800debug_sio e;
	memset(&e, 0, sizeof e);
	e.frame = (uint32_t)Atari800_nframes;
	e.device = frame[0];
	e.command = frame[1];
	e.aux1 = frame[2];
	e.aux2 = frame[3];
	snprintf(e.path, sizeof e.path, "%s",
	         path == SIO_TRACE_NETSIO ? "NetSIO" : path == SIO_TRACE_PATCH ? "patch" : "serial");
	const char* name = sio_command_name(e.device, e.command);
	char dev[8];
	if (e.device >= 0x31 && e.device <= 0x38)
		snprintf(dev, sizeof dev, "D%d:", e.device - 0x30);
	else
		snprintf(dev, sizeof dev, "$%02X", e.device);
	if (*name && e.device >= 0x31 && e.device <= 0x38)
		snprintf(e.text, sizeof e.text, "%s %s %d", dev, name, e.aux1 | (e.aux2 << 8));
	else if (*name)
		snprintf(e.text, sizeof e.text, "%s %s $%02X", dev, name, e.command);
	else
		snprintf(e.text, sizeof e.text, "%s $%02X %02X %02X", dev, e.command, e.aux1, e.aux2);
	std::lock_guard<std::mutex> lock(d->mtx);
	d->sio.push_back(e);
	if (d->sio.size() > 512)
		d->sio.pop_front();
}

// ---- the monitor's input -----------------------------------------------------

// Emulation thread, inside a monitor command (A, the assembler): waits for
// the next a800debug_command line, running other inspection jobs meanwhile.
int gets_hook(char* buffer, size_t size, const char* prompt)
{
	a800debug* d = g_debug;
	if (!d)
		return 0;
	if (prompt && *prompt) {
		fputs(prompt, mon_output);
		fflush(mon_output);
	}
	std::unique_lock<std::mutex> lock(d->cmdMtx);
	d->waitingInput = true;
	d->cmdCv.notify_all();
	while (!d->haveInput && d->attached.load() && !d->im->host.Quitting()) {
		d->cmdCv.wait_for(lock, std::chrono::milliseconds(20));
		if (!d->haveInput) {
			lock.unlock();
			d->im->host.RunParkedJobs();
			lock.lock();
		}
	}
	d->waitingInput = false;
	if (!d->haveInput)
		return 0;
	d->haveInput = false;
	snprintf(buffer, size, "%s", d->input.c_str());
	return 1;
}

void console_open(a800debug* d)
{
	if (!d->out) {
		d->out = tmpfile();
		d->outRead = 0;
	}
	if (d->out)
		mon_output = d->out;
}

// Emulation thread: the console text written since the last read.
std::string console_take(a800debug* d)
{
	std::string text;
	if (!d->out)
		return text;
	fflush(d->out);
	fseek(d->out, d->outRead, SEEK_SET);
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, d->out)) > 0)
		text.append(buf, n);
	fseek(d->out, 0, SEEK_END);
	d->outRead = ftell(d->out);
	if (d->outRead > ConsoleKeep) {
		fclose(d->out);
		d->out = nullptr;
		console_open(d);
	}
	return text;
}

} // namespace

// ---- stops -------------------------------------------------------------------

// Emulation thread, parked in PLATFORM_Exit(TRUE): why, and stay or go on.
void session_debug_stopped(struct a800session* s)
{
	SessionImpl* im = Impl(s);
	a800debug* d = static_cast<a800debug*>(s->debugger);
	CPU_GetStatus();
	uint16_t pc = CPU_regPC;
	bool attached = d && d->attached.load();

	if (CPU_cim_encountered) {
		std::string why = fmt("CIM: the CPU jammed on $%02X at $%04X", MEMORY_SafeGetByte(pc), pc);
		if (attached)
			MONITOR_Enter();
		CPU_cim_encountered = 0;
		clear_breaks();
		if (d) {
			std::lock_guard<std::mutex> lock(d->mtx);
			d->reason = why;
			d->reasonPc = pc;
			d->intent = Intent::None;
			d->atTableBp = d->skipping = d->frameChase = false;
			bump(d);
		}
		if (!attached)
			im->Notice("The Atari crashed (" + why + "). Power Cycle, or open the Debugger.");
		return;   // stays parked
	}
	if (!attached) {
		clear_breaks();
		im->host.Resume();
		return;
	}
	std::string why;
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		bool returned = d->intent == Intent::Out && MONITOR_break_ret && MONITOR_ret_nesting <= 0;
		if (d->skipping && !returned) {
			// stepped off a table breakpoint: on with whatever else is set
			d->skipping = false;
			MONITOR_break_step = FALSE;
			im->host.Resume();
			return;
		}
		d->skipping = false;
		if (d->frameChase && ANTIC_break_ypos == ANTIC_ypos) {
			d->frameChase = false;
			ANTIC_break_ypos = FrameLine;
			im->host.Resume();
			return;
		}
		if (MONITOR_break_step) {
			switch (d->intent) {
			case Intent::Pause: why = "Paused"; break;
			case Intent::Over: why = "Step over"; break;
			case Intent::Out: why = "Returned"; break;
			case Intent::Monitor: why = MONITOR_break_ret ? "Returned" : "Step"; break;
			default: why = "Step"; break;
			}
		}
		else if (ANTIC_break_ypos == ANTIC_ypos)
			why = d->intent == Intent::Frame ? "Frame" : fmt("Scanline %d", ANTIC_ypos);
		else if (MONITOR_break_addr == pc)
			why = d->intent == Intent::RunTo ? fmt("Run to $%04X", pc)
			      : d->intent == Intent::Over ? std::string("Step over")
			                                  : fmt("Breakpoint at $%04X", pc);
		else if (MONITOR_break_brk && MEMORY_SafeGetByte((UWORD)(pc - 1)) == 0x00)
			why = fmt("BRK at $%04X", (uint16_t)(pc - 1));
		else if (MONITOR_breakpoint_table_size > 0) {
			why = table_reason(d, pc);
			d->atTableBp = true;
		}
		else
			why = "Stopped";
		d->reason = why;
		d->reasonPc = pc;
		d->intent = Intent::None;
	}
	MONITOR_Enter();
	fflush(mon_output);
	clear_breaks();
	bump(d);
}

void session_debug_after_frame(void)
{
	g_dlistStart = ANTIC_dlist;
}

// Pause / Break-to-debugger: attaches, which stops.
void session_debug_pause(struct a800session* s)
{
	a800debug* d = a800debug_get(s);
	if (!d->attached.load())
		a800debug_attach(d);
	else
		a800debug_stop(d);
}

void session_debug_free(struct a800session* s)
{
	a800debug* d = static_cast<a800debug*>(s->debugger);
	if (!d)
		return;
	if (g_debug == d) {
		g_debug = nullptr;
		SIO_trace_hook = nullptr;
		MONITOR_gets_hook = nullptr;
		mon_output = stdout;
	}
	for (auto& p : d->snapshot)
		if (!p.empty())
			remove(p.c_str());
	if (d->out)
		fclose(d->out);
	delete d;
	s->debugger = nullptr;
}

a800debug* a800session_debugger(a800session* s)
{
	return a800debug_get(s);
}

a800debug* a800debug_get(a800session* s)
{
	if (!s->debugger) {
		a800debug* d = new a800debug;
		d->s = s;
		d->im = Impl(s);
		s->debugger = d;
	}
	return static_cast<a800debug*>(s->debugger);
}

void a800debug_attach(a800debug* d)
{
	if (d->attached.exchange(true))
		return;
	g_debug = d;
	d->im->host.Call([d] {
		SIO_trace_hook = sio_hook;
		MONITOR_gets_hook = gets_hook;
		console_open(d);
		arm(d);
	}, A800Host::Kind::Any);
	bump(d);
	a800debug_stop(d);
}

void a800debug_detach(a800debug* d)
{
	if (!d->attached.exchange(false))
		return;
	{
		std::lock_guard<std::mutex> lock(d->cmdMtx);
		d->cmdCv.notify_all();   // a monitor waiting for input gives up
	}
	d->im->host.Call([d] {
		MONITOR_breakpoint_table_size = 0;
		SIO_trace_hook = nullptr;
		clear_breaks();
		std::lock_guard<std::mutex> lock(d->mtx);
		d->atTableBp = d->skipping = d->frameChase = false;
		d->intent = Intent::None;
	}, A800Host::Kind::Any);
	if (d->im->host.Stopped())
		d->im->host.Resume();
	bump(d);
}

int a800debug_is_attached(a800debug* d)
{
	return d->attached.load() ? 1 : 0;
}

int a800debug_is_stopped(a800debug* d)
{
	return d->im->host.Stopped() ? 1 : 0;
}

// Sets the machine going under intent; setup arms its break (emulation
// thread, d->mtx held).
static void go(a800debug* d, Intent intent, const std::function<void()>& setup)
{
	d->im->host.Call([&] {
		std::lock_guard<std::mutex> lock(d->mtx);
		d->intent = intent;
		d->frameChase = false;
		setup();
		if (d->atTableBp && !MONITOR_break_step) {
			// the table would fire again before the instruction runs
			d->skipping = true;
			MONITOR_break_step = TRUE;
		}
		d->atTableBp = false;
	}, A800Host::Kind::Any);
	if (d->im->host.Stopped())
		d->im->host.Resume();
	bump(d);
}

void a800debug_stop(a800debug* d)
{
	if (d->im->host.Stopped() || !d->im->host.IsRunning())
		return;
	d->im->host.Call([d] {
		std::lock_guard<std::mutex> lock(d->mtx);
		d->intent = Intent::Pause;
		MONITOR_break_step = TRUE;
	}, A800Host::Kind::Any);
	if (A800Host::OnEmulationThread())
		return;   // it stops after this frame's next instruction
	for (int i = 0; i < 100 && !d->im->host.Stopped(); i++) {
		struct timespec ts = {0, 5000000};
		nanosleep(&ts, nullptr);
	}
}

void a800debug_resume(a800debug* d)
{
	go(d, Intent::None, [] {});
}

void a800debug_step(a800debug* d)
{
	go(d, Intent::Step, [] { MONITOR_break_step = TRUE; });
}

void a800debug_step_over(a800debug* d)
{
	go(d, Intent::Over, [] {
		UBYTE op = MEMORY_SafeGetByte(CPU_regPC);
		if (op == 0x20)   // JSR: to the instruction after it
			MONITOR_break_addr = (UWORD)(CPU_regPC + 3);
		else
			MONITOR_break_step = TRUE;
	});
}

void a800debug_step_out(a800debug* d)
{
	go(d, Intent::Out, [] {
		MONITOR_break_ret = TRUE;
		MONITOR_ret_nesting = 1;
	});
}

void a800debug_frame(a800debug* d)
{
	go(d, Intent::Frame, [d] {
		if (ANTIC_ypos == FrameLine) {
			// it would fire at once: past this line first
			d->frameChase = true;
			ANTIC_break_ypos = FrameLine + 1;
		}
		else
			ANTIC_break_ypos = FrameLine;
	});
}

void a800debug_run_to(a800debug* d, uint16_t addr)
{
	go(d, Intent::RunTo, [d, addr] {
		d->runTo = addr;
		MONITOR_break_addr = addr;
	});
}

void a800debug_break_on_brk(a800debug* d, int on)
{
	d->im->host.Call([on] { MONITOR_break_brk = on ? TRUE : FALSE; }, A800Host::Kind::Any);
	bump(d);
}

int a800debug_breaks_on_brk(a800debug* d)
{
	int on = 0;
	d->im->host.Call([&] { on = MONITOR_break_brk; }, A800Host::Kind::Any);
	return on;
}

int a800debug_stop_reason(a800debug* d, char* dst, int dstsz, int* address)
{
	std::lock_guard<std::mutex> lock(d->mtx);
	if (address)
		*address = d->reasonPc;
	if (!d->im->host.Stopped())
		return snprintf(dst, (size_t)dstsz, "Running");
	return snprintf(dst, (size_t)dstsz, "%s", d->reason.c_str());
}

unsigned a800debug_generation(a800debug* d)
{
	return d->generation.load() + d->im->host.ParkSerial();
}

// ---- the monitor console -----------------------------------------------------

int a800debug_command(a800debug* d, const char* line, char* dst, int dstsz)
{
	if (dst && dstsz > 0)
		*dst = 0;
	if (!d->im->host.IsRunning()) {
		if (dst && dstsz > 0)
			snprintf(dst, (size_t)dstsz, "The Atari is not running.\n");
		return A800DBG_CMD_OK;
	}
	std::unique_lock<std::mutex> lock(d->cmdMtx);
	if (d->waitingInput) {
		d->input = line ? line : "";
		d->haveInput = true;
		d->cmdCv.notify_all();
	}
	else {
		if (d->cmdRunning)
			return A800DBG_CMD_OK;
		d->cmdRunning = true;
		d->cmdDone = false;
		std::string copy = line ? line : "";
		d->im->host.Post([d, copy] {
			std::vector<char> buf(copy.begin(), copy.end());
			buf.push_back(0);
			console_open(d);
			int rc = MONITOR_Execute(buf.data());
			fflush(mon_output);
			std::lock_guard<std::mutex> l(d->cmdMtx);
			d->cmdRc = rc;
			d->cmdDone = true;
			d->cmdCv.notify_all();
		}, A800Host::Kind::Any);
	}
	// until it finishes or asks for more (once it has taken this line)
	while (!(d->cmdDone || (d->waitingInput && !d->haveInput))) {
		if (d->im->host.Quitting() || !d->im->host.IsRunning()) {
			d->cmdRunning = false;
			return A800DBG_CMD_OK;
		}
		d->cmdCv.wait_for(lock, std::chrono::milliseconds(50));
	}
	bool done = d->cmdDone;
	int rc = d->cmdRc;
	if (done)
		d->cmdRunning = false;
	lock.unlock();

	std::string text;
	d->im->host.Call([&] { text = console_take(d); }, A800Host::Kind::Any);
	if (dst && dstsz > 0)
		snprintf(dst, (size_t)dstsz, "%s", text.c_str());
	if (!done)
		return A800DBG_CMD_NEEDS_INPUT;
	if (rc == TRUE) {
		// G, R, O, CONT: armed by the monitor
		go(d, Intent::Monitor, [d] {
			std::vector<MONITOR_breakpoint_cond> rows;
			if (d->attached.load())
				rows = encode_all(d->bps);
			int n = (int)std::min<size_t>(rows.size(), MONITOR_BREAKPOINT_TABLE_MAX);
			for (int i = 0; i < n; i++)
				MONITOR_breakpoint_table[i] = rows[i];
			MONITOR_breakpoint_table_size = n;
		});
		return A800DBG_CMD_RESUMED;
	}
	bump(d);
	return A800DBG_CMD_OK;
}

int a800debug_completions(a800debug* d, const char* prefix, char* dst, int dstsz)
{
	static const char* const cmds[] = {
		"CONT", "SHOW", "STACK", "SET", "SETPC", "SETA", "SETX", "SETY", "SETS", "SETN", "SETV",
		"SETD", "SETI", "SETZ", "SETC", "BBRK", "BPC", "HISTORY", "JUMPS", "G", "R", "O", "BLINE",
		"BREAK", "YBREAK", "D", "M", "F", "S", "C", "SUM", "LOOP", "A", "DLIST", "LABELS", "ANTIC",
		"GTIA", "PIA", "POKEY", "CART", "ROM", "RAM", "HARDWARE", "READ", "WRITE", "SSTR", "SSCR",
		"FP", "MFP", "CFP", "HELP", nullptr};
	(void)d;
	std::string out, p = prefix ? prefix : "";
	for (auto& c : p)
		c = (char)toupper((unsigned char)c);
	for (int i = 0; cmds[i]; i++)
		if (!strncmp(cmds[i], p.c_str(), p.size()))
			out += std::string(out.empty() ? "" : " ") + cmds[i];
	return snprintf(dst, (size_t)dstsz, "%s", out.c_str());
}

// ---- the CPU ---------------------------------------------------------------------

void a800debug_cpu_get(a800debug* d, a800debug_cpu* o)
{
	memset(o, 0, sizeof *o);
	d->im->host.Call([o] {
		CPU_GetStatus();
		o->pc = CPU_regPC;
		o->sp = CPU_regS;
		o->a = CPU_regA;
		o->x = CPU_regX;
		o->y = CPU_regY;
		o->p = CPU_regP;
		o->n = !!(CPU_regP & CPU_N_FLAG);
		o->v = !!(CPU_regP & CPU_V_FLAG);
		o->d = !!(CPU_regP & CPU_D_FLAG);
		o->i = !!(CPU_regP & CPU_I_FLAG);
		o->z = !!(CPU_regP & CPU_Z_FLAG);
		o->c = !!(CPU_regP & CPU_C_FLAG);
		o->scanline = ANTIC_ypos;
		o->cycle = ANTIC_xpos;
		o->frame = (uint32_t)Atari800_nframes;
	}, A800Host::Kind::Any);
}

void a800debug_cpu_set(a800debug* d, int reg, int value)
{
	d->im->host.Call([reg, value] {
		CPU_GetStatus();
		auto flag = [&](UBYTE f) {
			CPU_regP = (UBYTE)(value ? (CPU_regP | f) : (CPU_regP & ~f));
		};
		switch (reg) {
		case A800_REG_PC: CPU_regPC = (UWORD)value; break;
		case A800_REG_SP: CPU_regS = (UBYTE)value; break;
		case A800_REG_A: CPU_regA = (UBYTE)value; break;
		case A800_REG_X: CPU_regX = (UBYTE)value; break;
		case A800_REG_Y: CPU_regY = (UBYTE)value; break;
		case A800_REG_P: CPU_regP = (UBYTE)(value | 0x30); break;
		case A800_FLAG_N: flag(CPU_N_FLAG); break;
		case A800_FLAG_V: flag(CPU_V_FLAG); break;
		case A800_FLAG_D: flag(CPU_D_FLAG); break;
		case A800_FLAG_I: flag(CPU_I_FLAG); break;
		case A800_FLAG_Z: flag(CPU_Z_FLAG); break;
		case A800_FLAG_C: flag(CPU_C_FLAG); break;
		default: break;
		}
		CPU_PutStatus();
	}, A800Host::Kind::Any);
	bump(d);
}

static void split_line(const char* text, a800debug_line* l)
{
	// "ADDR: b1 b2 b3  MNEMONIC operand          ;label\n"
	std::string t = text;
	if (!t.empty() && t.back() == '\n')
		t.pop_back();
	std::string bytes = t.size() > 6 ? t.substr(6, 9) : "";
	std::string rest = t.size() > 16 ? t.substr(16) : "";
	std::string comment;
	size_t semi = rest.find(';');
	if (semi != std::string::npos) {
		comment = rest.substr(semi + 1);
		rest = rest.substr(0, semi);
	}
	while (!rest.empty() && rest.back() == ' ')
		rest.pop_back();
	while (!bytes.empty() && bytes.back() == ' ')
		bytes.pop_back();
	snprintf(l->bytes, sizeof l->bytes, "%s", bytes.c_str());
	snprintf(l->disasm, sizeof l->disasm, "%s", rest.c_str());
	snprintf(l->comment, sizeof l->comment, "%s", comment.c_str());
}

int a800debug_disassemble(a800debug* d, uint16_t addr, a800debug_line* out, int max, int* pc_line)
{
	int n = 0;
	if (pc_line)
		*pc_line = -1;
	struct Range { uint16_t start, end; bool enabled; };
	std::vector<Range> bpRanges;
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		for (auto& b : d->bps)
			if (b.type & A800DEBUG_BP_EXEC)
				bpRanges.push_back({b.start, b.end, b.enabled});
	}
	d->im->host.Call([&] {
		uint16_t pc = addr;
		for (n = 0; n < max; n++) {
			char buf[128];
			a800debug_line* l = &out[n];
			memset(l, 0, sizeof *l);
			l->address = pc;
			l->is_pc = pc == CPU_regPC;
			if (l->is_pc && pc_line)
				*pc_line = n;
			for (auto& r : bpRanges)
				if (pc >= r.start && pc <= r.end && l->has_breakpoint != 1)
					l->has_breakpoint = r.enabled ? 1 : 2;
			const char* lab = MONITOR_LabelFor(pc, 0);
			if (lab)
				snprintf(l->label, sizeof l->label, "%s", lab);
			uint16_t next = MONITOR_DisassembleLine(pc, buf, sizeof buf);
			split_line(buf, l);
			pc = next;
		}
	}, A800Host::Kind::Any);
	return n;
}

int a800debug_row_address(a800debug* d, uint16_t addr, int rows)
{
	int result = addr;
	d->im->host.Call([&] {
		char buf[128];
		if (rows >= 0) {
			uint16_t pc = addr;
			for (int i = 0; i < rows; i++)
				pc = MONITOR_DisassembleLine(pc, buf, sizeof buf);
			result = pc;
			return;
		}
		// backwards: the start that decodes cleanly into addr
		for (int back = -rows * 3; back >= -rows; back--) {
			uint16_t pc = (uint16_t)(addr - back);
			int count = 0;
			while (pc != addr && count < back) {
				pc = MONITOR_DisassembleLine(pc, buf, sizeof buf);
				count++;
				if ((uint16_t)(addr - pc) > (uint16_t)back)
					break;
			}
			if (pc == addr && count >= -rows) {
				uint16_t start = (uint16_t)(addr - back);
				for (int i = 0; i < count + rows; i++)
					start = MONITOR_DisassembleLine(start, buf, sizeof buf);
				result = start;
				return;
			}
		}
		result = (uint16_t)(addr + rows);
	}, A800Host::Kind::Any);
	return result;
}

int a800debug_history_get(a800debug* d, a800debug_history* out, int max)
{
	int n = 0;
	d->im->host.Call([&] {
		int total = CPU_REMEMBER_PC_STEPS < max ? CPU_REMEMBER_PC_STEPS : max;
		for (int i = CPU_REMEMBER_PC_STEPS - total; i < CPU_REMEMBER_PC_STEPS; i++) {
			int k = (CPU_remember_PC_curpos + i) % CPU_REMEMBER_PC_STEPS;
			uint16_t pc = CPU_remember_PC[k];
			// the bytes as they were when it ran
			UBYTE saved[3];
			char buf[128];
			for (int j = 0; j < 3; j++) {
				saved[j] = MEMORY_dGetByte((UWORD)(pc + j));
				MEMORY_dPutByte((UWORD)(pc + j), CPU_remember_op[k][j]);
			}
			MONITOR_DisassembleLine(pc, buf, sizeof buf);
			for (int j = 0; j < 3; j++)
				MEMORY_dPutByte((UWORD)(pc + j), saved[j]);
			a800debug_history* h = &out[n++];
			h->pc = pc;
			h->scanline = CPU_remember_xpos[k] >> 8;
			h->cycle = CPU_remember_xpos[k] & 0xff;
			std::string t = buf;
			if (!t.empty() && t.back() == '\n')
				t.pop_back();
			snprintf(h->disasm, sizeof h->disasm, "%s", t.size() > 16 ? t.c_str() + 16 : t.c_str());
		}
	}, A800Host::Kind::Any);
	return n;
}

// ---- memory --------------------------------------------------------------------------

int a800debug_read(a800debug* d, uint16_t addr, uint8_t* dst, int n)
{
	d->im->host.Call([&] {
		for (int i = 0; i < n; i++)
			dst[i] = MEMORY_SafeGetByte((UWORD)(addr + i));
	}, A800Host::Kind::Any);
	return n;
}

void a800debug_write(a800debug* d, uint16_t addr, uint8_t value)
{
	d->im->host.Call([&] { MEMORY_PutByte(addr, value); }, A800Host::Kind::Any);
	bump(d);
}

int a800debug_label_address(a800debug* d, const char* label)
{
	int v = -1;
	d->im->host.Call([&] { v = MONITOR_LabelValue(label); }, A800Host::Kind::Any);
	return v;
}

int a800debug_address_label(a800debug* d, uint16_t addr, char* dst, int dstsz)
{
	std::string out;
	d->im->host.Call([&] {
		const char* l = MONITOR_LabelFor(addr, 0);
		if (l)
			out = l;
	}, A800Host::Kind::Any);
	return snprintf(dst, (size_t)dstsz, "%s", out.c_str());
}

int a800debug_load_symbols(a800debug* d, const char* path, char* msg, int msgsz)
{
	// ca65/ld65 -Ln and VICE ("al C:1234 .name"), MADS .lab ("00 1234 NAME"),
	// atari800's own ("1234 NAME"): into the monitor with LABELS SET, beside
	// the OS labels (a path with spaces would not survive LABELS ADD)
	FILE* in = fopen(path, "r");
	if (!in) {
		snprintf(msg, (size_t)msgsz, "Could not open it");
		return -1;
	}
	std::vector<std::pair<std::string, unsigned>> labels;
	char line[512];
	while (fgets(line, sizeof line, in)) {
		unsigned addr;
		char name[256], bank[16];
		if (sscanf(line, "al %*[^:]:%x .%255s", &addr, name) == 2
		    || sscanf(line, "al %x .%255s", &addr, name) == 2
		    || sscanf(line, "%15s %x %255s", bank, &addr, name) == 3
		    || sscanf(line, "%x %255s", &addr, name) == 2
		    || sscanf(line, "%255s = $%x", name, &addr) == 2) {
			bool ok = isalpha((unsigned char)name[0]) || strchr("_@?", name[0]);
			for (char* c = name; ok && *c; c++)
				ok = isalnum((unsigned char)*c) || strchr("_@?.", *c);
			if (ok)
				labels.push_back({name, addr & 0xffff});
		}
	}
	fclose(in);
	if (labels.empty()) {
		snprintf(msg, (size_t)msgsz, "No labels in it");
		return -1;
	}
	d->im->host.Call([&] {
		char cmd[300];
		console_open(d);
		snprintf(cmd, sizeof cmd, "LABELS BUILTIN");
		MONITOR_Execute(cmd);
		for (auto& l : labels) {
			snprintf(cmd, sizeof cmd, "LABELS SET %s %04X", l.first.c_str(), l.second);
			MONITOR_Execute(cmd);
		}
		fflush(mon_output);
	}, A800Host::Kind::Any);
	snprintf(msg, (size_t)msgsz, "%d labels", (int)labels.size());
	bump(d);
	return (int)labels.size();
}

// ---- ANTIC and the display list ----------------------------------------------------------

void a800debug_antic_get(a800debug* d, a800debug_antic* o)
{
	memset(o, 0, sizeof *o);
	d->im->host.Call([o] {
		o->dmactl = ANTIC_DMACTL;
		o->chactl = ANTIC_CHACTL;
		o->hscrol = ANTIC_HSCROL;
		o->vscrol = ANTIC_VSCROL;
		o->pmbase = ANTIC_PMBASE;
		o->chbase = ANTIC_CHBASE;
		o->nmien = ANTIC_NMIEN;
		o->nmist = ANTIC_NMIST;
		o->dlist = dlist_start();
		o->scanline = ANTIC_ypos;
		o->cycle = ANTIC_xpos;
		o->vcount = ANTIC_GetByte(0x0b, TRUE);
		o->playfield_width = ANTIC_DMACTL & 3;
		o->dl_dma = !!(ANTIC_DMACTL & 0x20);
		o->pm_dma = !!(ANTIC_DMACTL & 0x08);
		o->missile_dma = !!(ANTIC_DMACTL & 0x04);
		o->pm_single_line = !!(ANTIC_DMACTL & 0x10);
	}, A800Host::Kind::Any);
}

int a800debug_dlist_text(a800debug* d, char* dst, int dstsz)
{
	std::string out;
	d->im->host.Call([&] {
		UWORD pc = dlist_start();
		std::string last;
		int repeat = 0;
		UWORD lastAddr = 0;
		auto flush = [&]() {
			if (last.empty())
				return;
			out += fmt("$%04X  ", lastAddr);
			if (repeat > 1)
				out += fmt("%dx ", repeat);
			out += last + "\n";
			last.clear();
			repeat = 0;
		};
		for (int count = 0; count < 300; count++) {
			UWORD at = pc;
			UBYTE ir = MEMORY_SafeGetByte(pc);
			pc = (UWORD)((pc & 0xfc00) | ((pc + 1) & 0x03ff));   // ANTIC's 1K wrap
			std::string text, flags;
			if (ir & 0x80)
				flags += " DLI";
			UBYTE mode = ir & 0x0f;
			if (mode == 0)
				text = fmt("BLANK %d", ((ir >> 4) & 7) + 1);
			else if (mode == 1) {
				UWORD target = MEMORY_SafeGetByte(pc) | (MEMORY_SafeGetByte((UWORD)(pc + 1)) << 8);
				text = fmt("%s $%04X", (ir & 0x40) ? "JVB" : "JMP", target);
				flush();
				out += fmt("$%04X  %s%s\n", at, text.c_str(), flags.c_str());
				if (ir & 0x40)
					break;
				pc = target;
				continue;
			}
			else {
				if (ir & 0x40) {
					UWORD lms = MEMORY_SafeGetByte(pc) | (MEMORY_SafeGetByte((UWORD)(pc + 1)) << 8);
					pc = (UWORD)(pc + 2);
					flags += fmt(" LMS $%04X", lms);
				}
				if (ir & 0x20)
					flags += " VSCROL";
				if (ir & 0x10)
					flags += " HSCROL";
				text = fmt("MODE %X", mode);
			}
			std::string full = text + flags;
			if (full == last && full.find("LMS") == std::string::npos)
				repeat++;
			else {
				flush();
				last = full;
				lastAddr = at;
				repeat = 1;
			}
		}
		flush();
	}, A800Host::Kind::Any);
	return snprintf(dst, (size_t)dstsz, "%s", out.c_str());
}

// ---- GTIA ------------------------------------------------------------------------------------

void a800debug_gtia_get(a800debug* d, a800debug_gtia* o)
{
	memset(o, 0, sizeof *o);
	d->im->host.Call([o] {
		const UBYTE hp[4] = {GTIA_HPOSP0, GTIA_HPOSP1, GTIA_HPOSP2, GTIA_HPOSP3};
		const UBYTE hm[4] = {GTIA_HPOSM0, GTIA_HPOSM1, GTIA_HPOSM2, GTIA_HPOSM3};
		const UBYTE sp[4] = {GTIA_SIZEP0, GTIA_SIZEP1, GTIA_SIZEP2, GTIA_SIZEP3};
		const UBYTE gp[4] = {GTIA_GRAFP0, GTIA_GRAFP1, GTIA_GRAFP2, GTIA_GRAFP3};
		const UBYTE cp[4] = {GTIA_COLPM0, GTIA_COLPM1, GTIA_COLPM2, GTIA_COLPM3};
		const UBYTE cf[4] = {GTIA_COLPF0, GTIA_COLPF1, GTIA_COLPF2, GTIA_COLPF3};
		for (int i = 0; i < 4; i++) {
			o->hposp[i] = hp[i];
			o->hposm[i] = hm[i];
			o->sizep[i] = sp[i];
			o->grafp[i] = gp[i];
			o->colpm[i] = cp[i];
			o->colpf[i] = cf[i];
			o->mpf[i] = GTIA_GetByte((UWORD)(0x00 + i), TRUE);
			o->ppf[i] = GTIA_GetByte((UWORD)(0x04 + i), TRUE);
			o->mpl[i] = GTIA_GetByte((UWORD)(0x08 + i), TRUE);
			o->ppl[i] = GTIA_GetByte((UWORD)(0x0c + i), TRUE);
			o->trig[i] = GTIA_GetByte((UWORD)(0x10 + i), TRUE);
		}
		o->sizem = GTIA_SIZEM;
		o->grafm = GTIA_GRAFM;
		o->colbk = GTIA_COLBK;
		o->prior = GTIA_PRIOR;
		o->vdelay = GTIA_VDELAY;
		o->gractl = GTIA_GRACTL;
		o->consol = GTIA_GetByte(0x1f, TRUE);
	}, A800Host::Kind::Any);
}

uint32_t a800debug_color(a800debug* d, uint8_t index)
{
	uint32_t c = 0;
	d->im->host.Call([&] { c = a8video_colour(index); }, A800Host::Kind::Any);
	return c & 0xffffff;
}

int a800debug_palette_image(a800debug* d, uint32_t* dst)
{
	uint32_t pal[256];
	d->im->host.Call([&] {
		for (int i = 0; i < 256; i++)
			pal[i] = a8video_colour(i);
	}, A800Host::Kind::Any);
	// 16 hues down, 16 lumas across; each colour 16 x 4 pixels
	for (int y = 0; y < A800DEBUG_PALETTE_HEIGHT; y++)
		for (int x = 0; x < A800DEBUG_PALETTE_WIDTH; x++)
			dst[y * A800DEBUG_PALETTE_WIDTH + x] = pal[((y / 4) << 4) | (x / 16)] | 0xff000000u;
	return 1;
}

// ---- POKEY and PIA ------------------------------------------------------------------------

void a800debug_pokey_get(a800debug* d, int chip, a800debug_pokey* o)
{
	memset(o, 0, sizeof *o);
	d->im->host.Call([o, chip] {
		int c = chip ? 1 : 0;
		for (int i = 0; i < 4; i++) {
			o->audf[i] = POKEY_AUDF[c * 4 + i];
			o->audc[i] = POKEY_AUDC[c * 4 + i];
		}
		o->audctl = POKEY_AUDCTL[c];
		o->second = c;
		o->kbcode = POKEY_KBCODE;
		o->skstat = POKEY_SKSTAT;
		o->skctl = POKEY_SKCTL;
		o->irqen = POKEY_IRQEN;
		o->irqst = POKEY_IRQST;
		o->random = POKEY_GetByte(0x0a, TRUE);
		for (int i = 0; i < 8; i++)
			o->pot[i] = POKEY_POT_input[i];
	}, A800Host::Kind::Any);
}

void a800debug_pia_get(a800debug* d, a800debug_pia* o)
{
	memset(o, 0, sizeof *o);
	d->im->host.Call([o] {
		o->porta = PIA_PORTA;
		o->portb = PIA_PORTB;
		o->pactl = PIA_PACTL;
		o->pbctl = PIA_PBCTL;
		o->porta_mask = PIA_PORTA_mask;
		o->portb_mask = PIA_PORTB_mask;
		o->ca1 = PIA_CA1;
		o->ca2 = PIA_CA2;
		o->cb1 = PIA_CB1;
		o->cb2 = PIA_CB2;
		o->irq = PIA_IRQ;
	}, A800Host::Kind::Any);
}

// ---- the memory map ------------------------------------------------------------------------

void a800debug_map_get(a800debug* d, a800debug_map* o)
{
	memset(o, 0, sizeof *o);
	snprintf(o->machine, sizeof o->machine, "%s", a800_machine_name(prefs_machine(d->s)));
	d->im->host.Call([o] {
		o->xl = Atari800_machine_type == Atari800_MACHINE_XLXE;
		o->ram_kb = MEMORY_ram_size;
		if (o->xl) {
			UBYTE pb = (UBYTE)(PIA_PORTB | PIA_PORTB_mask);
			o->portb = pb;
			o->os_rom = pb & 0x01;
			o->basic_rom = !(pb & 0x02) && Atari800_builtin_basic;
			o->selftest = MEMORY_selftest_enabled;
			o->xe_cpu_bank = (pb & 0x10) ? -1 : MEMORY_xe_bank;
			o->xe_antic_bank = (pb & 0x20) ? -1 : MEMORY_xe_bank;
		}
		else {
			o->os_rom = 1;
			o->basic_rom = MEMORY_have_basic && !Atari800_disable_basic;
			o->xe_cpu_bank = o->xe_antic_bank = -1;
		}
		o->cart_type = CARTRIDGE_main.type > 0 ? CARTRIDGE_main.type : 0;
		if (o->cart_type) {
			snprintf(o->cart_name, sizeof o->cart_name, "%.95s", CARTRIDGE_main.filename);
			snprintf(o->cart_desc, sizeof o->cart_desc, "%s", CARTRIDGES[o->cart_type].description);
			o->cart_bank = CARTRIDGE_main.state;
		}
		o->piggyback_type = CARTRIDGE_piggyback.type > 0 ? CARTRIDGE_piggyback.type : 0;
		if (o->piggyback_type)
			snprintf(o->piggyback_name, sizeof o->piggyback_name, "%.95s", CARTRIDGE_piggyback.filename);
	}, A800Host::Kind::Any);
}

// ---- breakpoints -------------------------------------------------------------------------

static void rearm(a800debug* d)
{
	d->im->host.Call([d] { arm(d); }, A800Host::Kind::Any);
	bump(d);
}

int a800debug_breakpoint_add(a800debug* d, int type, uint16_t start, uint16_t end,
                             const char* condition, char* msg, int msgsz)
{
	Bp b;
	std::vector<Cond> conds;
	std::string err;
	if (!type)
		type = A800DEBUG_BP_EXEC;
	if (end < start)
		std::swap(start, end);
	if (condition && !parse_conditions(condition, conds, err)) {
		if (msg && msgsz > 0)
			snprintf(msg, (size_t)msgsz, "%s", err.c_str());
		return -1;
	}
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		b.id = type == A800DEBUG_BP_EXEC ? d->nextBp++ : d->nextWp++;
		b.type = type;
		b.start = start;
		b.end = end;
		b.enabled = true;
		b.condition = condition ? condition : "";
		std::vector<Bp> trial = d->bps;
		trial.push_back(b);
		if (encode_all(trial).size() > MONITOR_BREAKPOINT_TABLE_MAX) {
			if (msg && msgsz > 0)
				snprintf(msg, (size_t)msgsz, "atari800 holds %d breakpoint conditions; this one does not fit",
				         MONITOR_BREAKPOINT_TABLE_MAX);
			return -1;
		}
		d->bps.push_back(b);
	}
	rearm(d);
	return b.id;
}

int a800debug_breakpoint_toggle(a800debug* d, uint16_t addr)
{
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		for (size_t i = 0; i < d->bps.size(); i++)
			if (d->bps[i].type == A800DEBUG_BP_EXEC && d->bps[i].start == addr && d->bps[i].end == addr) {
				d->bps.erase(d->bps.begin() + (long)i);
				goto removed;
			}
	}
	return a800debug_breakpoint_add(d, A800DEBUG_BP_EXEC, addr, addr, "", nullptr, 0) > 0 ? 1 : -1;
removed:
	rearm(d);
	return 0;
}

int a800debug_breakpoint_check(a800debug* d, uint16_t addr)
{
	std::lock_guard<std::mutex> lock(d->mtx);
	for (auto& b : d->bps)
		if ((b.type & A800DEBUG_BP_EXEC) && addr >= b.start && addr <= b.end)
			return b.enabled ? 1 : 2;
	return 0;
}

void a800debug_breakpoint_remove(a800debug* d, int id)
{
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		d->bps.erase(std::remove_if(d->bps.begin(), d->bps.end(), [id](const Bp& b) { return b.id == id; }),
		             d->bps.end());
	}
	rearm(d);
}

void a800debug_breakpoint_enable(a800debug* d, int id, int enabled)
{
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		for (auto& b : d->bps)
			if (b.id == id)
				b.enabled = enabled != 0;
	}
	rearm(d);
}

int a800debug_breakpoint_list(a800debug* d, a800debug_breakpoint* out, int max)
{
	std::lock_guard<std::mutex> lock(d->mtx);
	int n = 0;
	for (auto& b : d->bps) {
		if (n >= max)
			break;
		out[n].id = b.id;
		out[n].type = b.type;
		out[n].start = b.start;
		out[n].end = b.end;
		out[n].enabled = b.enabled;
		snprintf(out[n].condition, sizeof out[n].condition, "%s", b.condition.c_str());
		n++;
	}
	return n;
}

void a800debug_breakpoint_clear(a800debug* d)
{
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		d->bps.clear();
	}
	rearm(d);
}

int a800debug_breakpoint_budget(a800debug* d, int* max)
{
	if (max)
		*max = MONITOR_BREAKPOINT_TABLE_MAX;
	std::lock_guard<std::mutex> lock(d->mtx);
	return (int)encode_all(d->bps).size();
}

// ---- the SIO trace -------------------------------------------------------------------------

int a800debug_sio_trace(a800debug* d, a800debug_sio* out, int max)
{
	std::lock_guard<std::mutex> lock(d->mtx);
	int n = 0;
	size_t first = d->sio.size() > (size_t)max ? d->sio.size() - (size_t)max : 0;
	for (size_t i = first; i < d->sio.size(); i++)
		out[n++] = d->sio[i];
	return n;
}

void a800debug_sio_clear(a800debug* d)
{
	std::lock_guard<std::mutex> lock(d->mtx);
	d->sio.clear();
}

// ---- snapshots ------------------------------------------------------------------------------

int a800debug_snapshot_save(a800debug* d, int slot)
{
	if (slot < 0 || slot > 3)
		return -1;
	if (d->snapshot[slot].empty())
		d->snapshot[slot] = std::string(d->s->states_dir) + "/debug-" + std::to_string(slot) + ".a8s";
	int ok = 0;
	std::string path = d->snapshot[slot];
	d->im->host.Call([&] { ok = StateSav_SaveAtariState(path.c_str(), "wb", FALSE); },
	                 A800Host::Kind::Any);
	if (ok)
		d->saved[slot] = true;
	return ok ? 0 : -1;
}

int a800debug_snapshot_restore(a800debug* d, int slot)
{
	if (slot < 0 || slot > 3 || !d->saved[slot])
		return -1;
	int ok = 0;
	std::string path = d->snapshot[slot];
	d->im->host.Call([&] { ok = StateSav_ReadAtariState(path.c_str(), "rb"); }, A800Host::Kind::Any);
	bump(d);
	return ok ? 0 : -1;
}

int a800debug_snapshot_exists(a800debug* d, int slot)
{
	return slot >= 0 && slot < 4 && d->saved[slot];
}

int a800debug_save(a800debug* d, const char* kind, const char* path, char* msg, int msgsz)
{
	FILE* f = fopen(path, "wb");
	if (!f) {
		snprintf(msg, (size_t)msgsz, "Could not write %s", path);
		return -1;
	}
	if (!strcmp(kind, "ram")) {
		std::vector<uint8_t> mem(65536);
		a800debug_read(d, 0, mem.data(), 65536);
		fwrite(mem.data(), 1, mem.size(), f);
		snprintf(msg, (size_t)msgsz, "64K saved");
	}
	else {
		a800debug_cpu cpu;
		a800debug_cpu_get(d, &cpu);
		std::vector<a800debug_line> lines(256);
		int n = a800debug_disassemble(d, (uint16_t)cpu.pc, lines.data(), 256, nullptr);
		for (int i = 0; i < n; i++)
			fprintf(f, "%04X  %-9s %-24s %s%s%s\n", lines[i].address, lines[i].bytes, lines[i].disasm,
			        lines[i].label[0] ? lines[i].label : "", lines[i].comment[0] ? " ;" : "",
			        lines[i].comment);
		snprintf(msg, (size_t)msgsz, "%d lines saved", n);
	}
	fclose(f);
	return 0;
}
