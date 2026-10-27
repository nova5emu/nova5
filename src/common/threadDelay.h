#pragma once

#include "common/threads.h"
#include "common/timer.h"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

// Opt-in frame-boundary sensitivity diagnostic. No clocks, TLS, counters or
// sleeps on disabled sites. Environment is read once, not polled during play.
namespace Common::ThreadDelay {

enum class Site : size_t { Gpu, GuestMain, Completion, Count };
inline constexpr size_t SiteCount = static_cast<size_t>(Site::Count);
inline constexpr std::array<const char*, SiteCount> Variables {
    "KYTY_DELAY_GPU_THREAD_US", "KYTY_DELAY_GUEST_MAIN_US", "KYTY_DELAY_COMPLETION_US"};
inline constexpr std::array<const char*, SiteCount> Names {"gpu", "guest_main", "completion"};

inline const std::array<uint32_t, SiteCount>& Settings() {
	static const auto settings = [] {
		std::array<uint32_t, SiteCount> values {};
		for (size_t i = 0; i < SiteCount; ++i) {
			const char* text = std::getenv(Variables[i]);
			if (text == nullptr || *text == '\0') continue;
			uint32_t value = 0;
			bool valid = true;
			for (const char* p = text; *p != '\0'; ++p) {
				if (*p < '0' || *p > '9') { valid = false; break; }
				value = value * 10 + static_cast<uint32_t>(*p - '0');
				if (value > 1000000) { valid = false; break; }
			}
			if (!valid) {
				std::fprintf(stderr, "[THREAD_DELAY] invalid %s (expected integer 0..1000000 us); disabled\n", Variables[i]);
				continue;
			}
			values[i] = value;
			if (value != 0) {
				std::fprintf(stderr, "[THREAD_DELAY] configured site=%s requested_us=%u; waiting for boundary calls\n", Names[i], value);
			}
		}
		return values;
	}();
	return settings;
}

inline bool Enabled(Site site) {
	return Settings()[static_cast<size_t>(site)] != 0;
}

template <Site S>
inline void Apply(const char* boundary, const char* thread_name) {
	constexpr auto index = static_cast<size_t>(S);
	const auto micros = Settings()[index];
	if (micros == 0) return;

	struct Stats {
		std::string tid = Thread::GetThreadId();
		uint64_t frequency = Timer::QueryPerformanceFrequency();
		uint64_t window_start = Timer::QueryPerformanceCounter();
		uint64_t total_calls = 0, calls = 0, ticks = 0, max_ticks = 0;
	};
	// Both GPU flip paths share one counter on the GPU command thread. Stats
	// stay local to each actual caller; multiple guest callers are visible.
	thread_local Stats stats;
	const auto begin = Timer::QueryPerformanceCounter();
	Thread::SleepMicro(micros); // Existing Windows high-resolution waitable timer.
	const auto end = Timer::QueryPerformanceCounter();
	const auto elapsed = end - begin;
	++stats.total_calls;
	++stats.calls;
	stats.ticks += elapsed;
	if (elapsed > stats.max_ticks) stats.max_ticks = elapsed;
	// First call proves the hook was reached. Thereafter one line per 64 calls,
	// with actual wall time (including scheduling overshoot), not assumed N us.
	if (stats.total_calls == 1 || stats.calls == 64) {
		const double us_per_tick = 1000000.0 / static_cast<double>(stats.frequency);
		std::fprintf(stderr,
		    "[THREAD_DELAY] site=%s boundary=%s tid=%s thread=\"%s\" requested_us=%u"
		    " calls=%" PRIu64 " total_calls=%" PRIu64 " window_ms=%.3f"
		    " actual_ms=%.3f mean_us=%.3f max_us=%.3f\n",
		    Names[index], boundary, stats.tid.c_str(), thread_name, micros,
		    stats.calls, stats.total_calls, (end - stats.window_start) * us_per_tick / 1000.0,
		    stats.ticks * us_per_tick / 1000.0, stats.ticks * us_per_tick / stats.calls,
		    stats.max_ticks * us_per_tick);
		stats.calls = stats.ticks = stats.max_ticks = 0;
		stats.window_start = end;
	}
}

} // namespace Common::ThreadDelay
