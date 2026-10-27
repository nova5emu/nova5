#pragma once
#include <cstdint>

namespace Common::FrameTrace {
// Bounded even when the guest stops flipping or emits a pathological number of draws.
class Window {
public:
	static constexpr uint32_t MaxFrames = 8, MaxDraws = 4096;
	static constexpr uint64_t MaxMs = 2000;
	void Start(uint64_t now) { active = true; started = now; frames = draws = 0; }
	bool Active(uint64_t now) const { return active && now - started < MaxMs; }
	bool Draw(int frame, uint64_t now) {
		if (!Active(now) || draws == MaxDraws) { active = false; return false; }
		if (draws == 0 || frame != last_frame) {
			if (frames == MaxFrames) { active = false; return false; }
			++frames;
			last_frame = frame;
		}
		++draws;
		return true;
	}
	uint32_t Frames() const { return frames; }
	uint32_t Draws() const { return draws; }
private:
	bool active = false;
	uint64_t started = 0;
	uint32_t frames = 0, draws = 0;
	int last_frame = 0;
};

// KYTY_FRAME_TRACE=1 arms a capture on a NEW timestamp of frame-trace.trigger in cwd.
// No guest reads, readback, or state modifications. IDs join draws to their resources.
uint64_t BeginDraw(int frame);
uint64_t ActiveId();
#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void Print(uint64_t id, const char* format, ...);
}
