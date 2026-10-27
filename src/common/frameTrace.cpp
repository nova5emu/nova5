#include "common/frameTrace.h"
#include "common/hostPath.h"
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace Common::FrameTrace {
namespace {
uint64_t Now() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool Enabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_FRAME_TRACE");
		return value && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}
struct State {
	std::mutex mutex;
	Window window;
	uint64_t generation = 0, next_poll = 0;
	bool initialized = false, reported_end = true;
	std::filesystem::file_time_type stamp {};
	std::filesystem::path trigger;
};
State state;
std::atomic<uint32_t> lines {0};
constexpr uint32_t MaxLines = 32768;
void End() {
	if (!state.reported_end) {
		std::fprintf(stderr, "[FRAME_TRACE] end capture=%llu frames=%u draws=%u lines=%u\n",
		    static_cast<unsigned long long>(state.generation), state.window.Frames(), state.window.Draws(), lines.load());
		std::fflush(stderr);
		state.reported_end = true;
	}
}
}
uint64_t BeginDraw(int frame) {
	if (!Enabled()) return 0;
	std::lock_guard lock(state.mutex);
	const auto now = Now();
	if (now >= state.next_poll) {
		state.next_poll = now + 250;
		if (!state.initialized) state.trigger = NativeHostPath("frame-trace.trigger");
		std::error_code error;
		const auto stamp = std::filesystem::last_write_time(state.trigger, error);
		if (!state.initialized) {
			// Ignore a marker left by a previous run.
			state.initialized = true;
			if (!error) state.stamp = stamp;
			std::fprintf(stderr, "[FRAME_TRACE] armed trigger=frame-trace.trigger frames=%u draws=%u timeout_ms=%llu (existing marker ignored)\n",
			    Window::MaxFrames, Window::MaxDraws, static_cast<unsigned long long>(Window::MaxMs));
			std::fflush(stderr);
		} else if (!error && stamp != state.stamp) {
			state.stamp = stamp;
			End();
			state.window.Start(now);
			++state.generation;
			state.reported_end = false;
			lines.store(0);
			std::fprintf(stderr, "[FRAME_TRACE] begin capture=%llu frame=%d\n", static_cast<unsigned long long>(state.generation), frame);
		}
	}
	if (!state.window.Draw(frame, now)) { End(); return 0; }
	return (state.generation << 32) | state.window.Draws();
}
uint64_t ActiveId() {
	if (!Enabled()) return 0;
	std::lock_guard lock(state.mutex);
	if (!state.window.Active(Now())) { End(); return 0; }
	return state.generation << 32;
}
void Print(uint64_t id, const char* format, ...) {
	if (id == 0) return;
	const auto line = lines.fetch_add(1);
	if (line >= MaxLines) {
		if (line == MaxLines) std::fprintf(stderr, "[FRAME_TRACE] line limit reached\n");
		return;
	}
	char text[4096];
	va_list args;
	va_start(args, format);
	const auto count = std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	std::fprintf(stderr, "[FRAME_TRACE] id=%llu %s%s\n", static_cast<unsigned long long>(id), text,
	             count >= static_cast<int>(sizeof(text)) ? " [truncated]" : "");
	std::fflush(stderr);
}
}
