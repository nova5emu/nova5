#include "common/frameTrace.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
using Common::FrameTrace::Window;
static void Check(bool value) { if (!value) std::abort(); }
int main(int argc, char**) {
#ifdef _WIN32
	_putenv_s("KYTY_FRAME_TRACE", argc > 1 ? "0" : "1");
#else
	setenv("KYTY_FRAME_TRACE", argc > 1 ? "0" : "1", 1);
#endif
	if (argc > 1) {
		Check(Common::FrameTrace::BeginDraw(1) == 0 && Common::FrameTrace::ActiveId() == 0);
		std::puts("FrameTraceTests: disabled passed");
		return 0;
	}
	Window window;
	Check(!window.Draw(0, 0));
	window.Start(100);
	for (int i = 0; i < Window::MaxFrames; ++i) {
		Check(window.Draw(20 + i, 101));
		Check(window.Draw(20 + i, 101));
	}
	Check(window.Draw(27, 101)); // All draws in the last frame are included.
	Check(!window.Draw(28, 101) && !window.Active(101));
	window.Start(200);
	for (unsigned i = 0; i < Window::MaxDraws; ++i) Check(window.Draw(5, 201));
	Check(!window.Draw(5, 201)); // No flips cannot produce an unbounded log.
	window.Start(300);
	Check(window.Draw(8, 300 + Window::MaxMs - 1));
	Check(!window.Draw(8, 300 + Window::MaxMs));
	window.Start(5000);
	Check(window.Draw(1, 5000) && window.Frames() == 1 && window.Draws() == 1);
	const auto original = std::filesystem::current_path();
	const auto fixture = std::filesystem::temp_directory_path() /
	    ("nova5-frame-trace-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	Check(std::filesystem::create_directory(fixture));
	std::filesystem::current_path(fixture);
	const auto marker = fixture / "frame-trace.trigger";
	{ std::ofstream file(marker); file << "stale"; }
	Check(Common::FrameTrace::BeginDraw(1) == 0); // Ignore an old capture request at startup.
	auto stamp = std::filesystem::last_write_time(marker);
	auto trigger = [&] {
		stamp += std::chrono::seconds(1);
		std::filesystem::last_write_time(marker, stamp);
		uint64_t id = 0;
		for (unsigned i = 0; i < 50 && !id; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			id = Common::FrameTrace::BeginDraw(1);
		}
		Check(id != 0);
		return id;
	};
	const auto first = trigger();
	Check(Common::FrameTrace::ActiveId() == (first & ~uint64_t{0xffffffff}));
	Common::FrameTrace::Print(first, "test capture");
	for (int i = 2; i <= Window::MaxFrames; ++i) Check(Common::FrameTrace::BeginDraw(i) != 0);
	Check(Common::FrameTrace::BeginDraw(Window::MaxFrames + 1) == 0);
	std::this_thread::sleep_for(std::chrono::milliseconds(270));
	Check(Common::FrameTrace::BeginDraw(1) == 0); // Same marker cannot retrigger forever.
	Check(trigger() > first); // A new timestamp permits a menu/scene comparison in one run.
	std::filesystem::current_path(original);
	Check(std::filesystem::remove(marker));
	Check(std::filesystem::remove(fixture)); // Empty private fixture, no recursive deletion.
	std::puts("FrameTraceTests: frame/draw/time bounds and restart passed");
}
