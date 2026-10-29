#include "graphics/guest_gpu/submitProfileRuntime.h"

#include "common/readbackProfile.h"
#include "common/submitProfile.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#else
#include <pthread.h>
#endif

namespace Libs::Graphics::SubmitProfileRuntime {

namespace {

constexpr uint64_t GatePollNs   = 250'000'000ull;
constexpr uint64_t ReportNs     = 5'000'000'000ull;
constexpr uint32_t MaxReports   = 240;

// KYTY_SUBMIT_PROFILE_GATE: recording runs while this file exists (always, if unset).
// KYTY_TIMER_PERIOD_GATE: a 1 ms system timer period is held while this file exists.
struct State {
	bool        initialized = false;
	std::string profile_gate;
	std::string timer_gate;
	uint64_t    next_poll_ns    = 0;
	uint64_t    window_start_ns = 0;
	uint32_t    reports         = 0;
	bool        timer_active    = false;
	std::string experiment_gate;
};

std::atomic<bool> g_experiment {false};

State& GetState() {
	static State state;
	if (!state.initialized) {
		state.initialized = true;
		if (const auto* gate = std::getenv("KYTY_SUBMIT_PROFILE_GATE")) state.profile_gate = gate;
		if (const auto* gate = std::getenv("KYTY_TIMER_PERIOD_GATE")) state.timer_gate = gate;
		if (const auto* gate = std::getenv("KYTY_EXPERIMENT_GATE")) state.experiment_gate = gate;
	}
	return state;
}

bool TimerGated() {
	static const bool gated = std::getenv("KYTY_TIMER_PERIOD_GATE") != nullptr;
	return gated;
}

bool ExperimentGated() {
	static const bool gated = [] {
		const auto* path = std::getenv("KYTY_EXPERIMENT_GATE");
		return path != nullptr && path[0] != '\0';
	}();
	return gated;
}

bool FileExists(const std::string& path) {
	std::error_code error;
	return std::filesystem::exists(path, error) && !error;
}

// KYTY_SUBMIT_PROFILE_FILE: append reports to this file instead of stderr, for runs whose
// console is not captured (a game opened from the Nova5 launcher).
std::FILE* Output() {
	static std::FILE* const output = [] {
		const auto* path = std::getenv("KYTY_SUBMIT_PROFILE_FILE");
		auto*       file = path != nullptr ? std::fopen(path, "ab") : nullptr;
		return file != nullptr ? file : stderr;
	}();
	return output;
}

void Event(const char* text) {
	std::fprintf(Output(), "[SUBMIT_PROFILE_EVENT] %s\n", text);
	std::fflush(Output());
}

void Report(State& state, uint64_t now) {
	// Tick and Shutdown run on the GPU thread, outside readback WaitScopes.
	// Drain TLS histograms while recording and the old generation are still live.
	Common::ReadbackProfile::FlushCurrentThread(now);
	if (auto* hook = Common::SubmitProfile::Detail::window_close_hook.load(std::memory_order_acquire)) hook();
	if (state.window_start_ns != 0 && now > state.window_start_ns && state.reports < MaxReports) {
		++state.reports;
		Common::SubmitProfile::WriteReport(Output(), (now - state.window_start_ns) / 1e6,
		                                   state.timer_active, now,
		                                   false, // The occlusion bound gate is retired.
		                                   g_experiment.load(std::memory_order_relaxed));
		std::fflush(Output());
	} else {
		Common::SubmitProfile::Reset(now);
	}
	state.window_start_ns = now;
}

void SetTimer(State& state, bool active, uint64_t now) {
	if (active == state.timer_active) return;
	// Close the window so no report mixes both timer states.
	if (Common::SubmitProfile::Recording()) Report(state, now);
#if defined(_WIN32)
	if (active) {
		if (timeBeginPeriod(1) != TIMERR_NOERROR) {
			Event("timer_1ms request failed");
			return;
		}
		state.timer_active = true;
		Event("timer_1ms on");
	} else {
		timeEndPeriod(1);
		state.timer_active = false;
		Event("timer_1ms off");
	}
	// The next window starts once the new period is in effect; anything in flight is dropped.
	if (Common::SubmitProfile::Recording()) {
		const auto changed = Common::SubmitProfile::NowNs();
		Common::SubmitProfile::Reset(changed);
		state.window_start_ns = changed;
	}
#else
	(void)now;
#endif
}

void SetExperimentGate(State& state, bool active, uint64_t now) {
	if (active == g_experiment.load(std::memory_order_relaxed)) return;
	if (Common::SubmitProfile::Recording()) Report(state, now); // Never mix both states.
	g_experiment.store(active, std::memory_order_relaxed);
	Common::SubmitProfile::SetExperiment(active);
	Event(active ? "experiment on" : "experiment off");
	if (Common::SubmitProfile::Recording()) {
		const auto changed = Common::SubmitProfile::NowNs();
		Common::SubmitProfile::Reset(changed);
		state.window_start_ns = changed;
	}
}

} // namespace

bool ExperimentActive() {
	return g_experiment.load(std::memory_order_relaxed);
}

uint32_t ThreadId() {
#if defined(_WIN32)
	return static_cast<uint32_t>(GetCurrentThreadId());
#else
	return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
#endif
}

void Tick() {
	if (!Common::SubmitProfile::Armed() && !TimerGated() && !ExperimentGated()) return;
	auto&      state = GetState();
	const auto now   = Common::SubmitProfile::NowNs();
	if (now >= state.next_poll_ns) {
		state.next_poll_ns = now + GatePollNs;
		if (!state.timer_gate.empty()) SetTimer(state, FileExists(state.timer_gate), now);
		if (!state.experiment_gate.empty()) {
			SetExperimentGate(state, FileExists(state.experiment_gate), now);
		}
		if (Common::SubmitProfile::Armed()) {
			const bool open = state.profile_gate.empty() || FileExists(state.profile_gate);
			if (open != Common::SubmitProfile::Recording()) {
				if (open) {
					Common::SubmitProfile::Reset(now);
					state.window_start_ns = now;
					Common::SubmitProfile::SetRecording(true);
					Event("recording on");
				} else {
					Report(state, now);
					Common::SubmitProfile::SetRecording(false);
					state.window_start_ns = 0;
					Event("recording off");
				}
			}
		}
	}
	// Gate handling may have opened a window after `now` was read: re-read the clock.
	const auto after = Common::SubmitProfile::NowNs();
	if (Common::SubmitProfile::Recording() && after > state.window_start_ns &&
	    after - state.window_start_ns >= ReportNs) {
		Report(state, after);
	}
}

void Shutdown() {
	if (!Common::SubmitProfile::Armed() && !TimerGated() && !ExperimentGated()) return;
	auto&      state = GetState();
	const auto now   = Common::SubmitProfile::NowNs();
	if (Common::SubmitProfile::Recording()) {
		Report(state, now);
		Common::SubmitProfile::SetRecording(false);
	}
	SetTimer(state, false, now);
}

} // namespace Libs::Graphics::SubmitProfileRuntime
