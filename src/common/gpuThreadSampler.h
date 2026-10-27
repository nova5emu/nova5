#pragma once
#include "common/guestCpuProfile.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Sampling and boundary counters share one window. Hooks never read the clock.
namespace Common::GpuThreadSampler {

enum class Phase : uint8_t { Other, Prepare, DrawEmit, DispatchEmit, Submit, Immediate, Wait, Count };
enum class Boundary : uint8_t { Copy, Barrier, OtherNative, Readback, Flush, Wait, TimelineWait, PriorityWait, Submit, Count };
inline constexpr size_t PhaseCount = static_cast<size_t>(Phase::Count);
inline constexpr size_t BoundaryCount = static_cast<size_t>(Boundary::Count);
static_assert(std::atomic<Phase>::is_always_lock_free); // Read while the GPU lane is suspended.
inline constexpr const char* PhaseNames[] = {"other", "prepare", "draw_emit", "dispatch_emit", "submit", "immediate", "wait"};
inline constexpr const char* BoundaryNames[] = {"copy_upload", "barrier", "other_native", "readback", "flush_finish", "wait_request", "timeline_wait", "priority_wait", "submit"};

struct BoundaryFrame {
	uint64_t frame = 0, window = 0, draws = 0, dispatches = 0;
	uint64_t events[BoundaryCount] {}, breaks[BoundaryCount] {};
	uint64_t batches = 0, batch_packets = 0, longest = 0, ge8 = 0, ge32 = 0;
	uint64_t frame_tail = 0, interruptions = 0;
	bool complete = false;
};

// One producer (the GPU lane). Completed rows are immutable and published with
// release/acquire; no reader locks or suspension are needed for these counters.
// Fixed capacity across the session; overflow is explicit, never overwrite a row.
struct BoundaryState {
	static constexpr size_t Capacity = 16384;
	std::atomic<unsigned> requested_window {0};
	std::atomic<Phase> phase {Phase::Other};
	std::atomic<size_t> published {0};
	std::atomic<uint64_t> dropped {0};
	std::array<BoundaryFrame, Capacity> frames {};
	struct Anchor { uintptr_t pc = 0; const char* name = nullptr; };
	std::array<Anchor, 16> anchors {};
	std::atomic<size_t> anchor_count {0};
	BoundaryFrame current {};
	unsigned window = 0;
	uint64_t frame = 0, batch = 0;
	bool complete = false;

	bool Sync() {
		const auto requested = requested_window.load(std::memory_order_relaxed);
		if (requested != window) {
			window = requested; current = {}; batch = 0; complete = false;
		}
		return window != 0;
	}
	void EndBatch() {
		if (!batch) return;
		++current.batches; current.batch_packets += batch;
		if (batch > current.longest) current.longest = batch;
		current.ge8 += batch >= 8; current.ge32 += batch >= 32;
		batch = 0;
	}
	void Event(Boundary kind, bool force = false) {
		const auto p = phase.load(std::memory_order_relaxed);
		const bool emission = p == Phase::DrawEmit || p == Phase::DispatchEmit;
		if (emission && !force) return;
		if (!Sync()) return;
		const auto i = static_cast<size_t>(kind);
		++current.events[i];
		if (emission) ++current.interruptions;
		if (batch) { ++current.breaks[i]; EndBatch(); }
	}
	void Packet(Phase p) {
		if (!Sync()) return;
		if (p == Phase::DrawEmit) ++current.draws;
		else ++current.dispatches;
		++batch;
	}
	void Flip() {
		++frame;
		if (!Sync()) return;
		current.frame_tail = batch; EndBatch();
		current.frame = frame; current.window = window; current.complete = complete;
		const auto index = published.load(std::memory_order_relaxed);
		if (index < Capacity) {
			frames[index] = current;
			published.store(index + 1, std::memory_order_release);
		} else dropped.fetch_add(1, std::memory_order_relaxed);
		current = {}; complete = true;
	}
};

// Null on every lane unless this GPU Session opted into the boundary diagnostic.
// Disabled hooks are a TLS null check: no atomics, allocations, timers or getenv.
inline thread_local BoundaryState* boundary_state = nullptr;
// Call directly at the instrumented function entry, not from an inline wrapper.
// The return PC supplies an exact pdata identity even in a publics-only LTO PDB.
void NameCaller(const char* name);
inline void NativeBoundary(Boundary kind) {
	if (auto* state = boundary_state) state->Event(kind);
}
inline void FrameBoundary() {
	if (auto* state = boundary_state) state->Flip();
}
class PhaseScope final {
public:
	explicit PhaseScope(Phase phase) : m_state(boundary_state) {
		if (m_state) {
			m_previous = m_state->phase.load(std::memory_order_relaxed);
			m_state->phase.store(phase, std::memory_order_relaxed);
			if (phase == Phase::DrawEmit || phase == Phase::DispatchEmit) m_state->Packet(phase);
		}
	}
	~PhaseScope() { if (m_state) m_state->phase.store(m_previous, std::memory_order_relaxed); }
	PhaseScope(const PhaseScope&) = delete;
	PhaseScope& operator=(const PhaseScope&) = delete;
private:
	BoundaryState* m_state;
	Phase m_previous = Phase::Other;
};
class ImmediateScope final {
public:
	explicit ImmediateScope(Boundary kind) : m_state(boundary_state) {
		if (m_state) {
			m_state->Event(kind, true);
			m_previous = m_state->phase.load(std::memory_order_relaxed);
			const bool wait = kind == Boundary::Wait || kind == Boundary::TimelineWait || kind == Boundary::PriorityWait;
			m_state->phase.store(wait ? Phase::Wait : kind == Boundary::Submit ? Phase::Submit : Phase::Immediate, std::memory_order_relaxed);
		}
	}
	~ImmediateScope() { if (m_state) m_state->phase.store(m_previous, std::memory_order_relaxed); }
	ImmediateScope(const ImmediateScope&) = delete;
	ImmediateScope& operator=(const ImmediateScope&) = delete;
private:
	BoundaryState* m_state;
	Phase m_previous = Phase::Other;
};

inline bool AnnounceCurrentThread(std::FILE* output = stdout) {
	const auto* enabled = std::getenv("KYTY_GPU_THREAD_SAMPLER");
	if ((enabled == nullptr || std::strcmp(enabled, "1") != 0) &&
	    !GuestCpuProfile::GuestCount() && !GuestCpuProfile::FaultEnabled()) return false;
#if defined(_WIN32)
	FILETIME process_start {}, thread_start {}, exit {}, kernel {}, user {};
	if (!GetProcessTimes(GetCurrentProcess(), &process_start, &exit, &kernel, &user) ||
	    !GetThreadTimes(GetCurrentThread(), &thread_start, &exit, &kernel, &user)) {
		std::fprintf(stderr, "[GPU_THREAD_SAMPLER_ERROR] cannot identify command lane: %lu\n", GetLastError());
		return false;
	}
	const auto ticks = [](FILETIME time) {
		return (uint64_t{time.dwHighDateTime} << 32u) | time.dwLowDateTime;
	};
	// Names are helpful in WPA; the PID/TID plus creation times are authoritative.
	(void)SetThreadDescription(GetCurrentThread(), L"Nova5 GPU command");
	const auto written = std::fprintf(output,
	    "[GPU_THREAD_SAMPLER] {\"schema\":1,\"backend\":\"inprocess\",\"pid\":%lu,\"tid\":%lu,"
	    "\"process_start_filetime\":\"%llu\",\"thread_start_filetime\":\"%llu\"}\n",
	    GetCurrentProcessId(), GetCurrentThreadId(),
	    static_cast<unsigned long long>(ticks(process_start)),
	    static_cast<unsigned long long>(ticks(thread_start)));
	return written > 0 && std::fflush(output) == 0;
#else
	std::fprintf(stderr, "[GPU_THREAD_SAMPLER_ERROR] sampler requires Windows x64\n");
	return false;
#endif
}

// Construct/destroy on the sampled thread, outside renderer locks. Destruction
// signals the worker and joins it; the worker always resumes before reporting.
class Session final {
public:
	// DbgHelp is process-global, even with distinct session handles. Callers
	// must exclude another active symbol engine (Nova5's opt-in Tracy client).
	explicit Session(bool another_symbol_engine_active = false);
	~Session();
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

private:
	struct Impl;
	Impl* m_impl = nullptr;
};

// The normal window-close path uses quick_exit, which skips ThreadRun's RAII.
// Call before entering CRT exit handlers, outside renderer/heap/stdio locks.
void StopAndFlush();

#if defined(KYTY_GPU_SAMPLER_TESTS) && defined(_WIN32) && defined(_M_X64)
bool CheckAggregationForTests();
#endif

} // namespace Common::GpuThreadSampler
