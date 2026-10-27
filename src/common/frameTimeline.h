#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace Common::FrameTimeline {
// Signed modular distance from a calibration anchor. The caller must keep the
// capture shorter than half the native counter's wrap period. Scale the small
// interval before adding the host epoch, preserving large integer epochs.
inline bool ConvertClock(uint64_t native, uint64_t native_anchor, uint64_t host_anchor,
                         uint64_t mask, double period_ns, uint64_t frequency, uint64_t& host) {
    if (!mask || !frequency || !std::isfinite(period_ns) || period_ns <= 0) return false;
    const auto delta = (native - native_anchor) & mask;
    if (delta == (mask >> 1) + 1) return false; // Ambiguous exactly half wrap.
    const bool forward = delta <= (mask >> 1);
    const auto magnitude = forward ? delta : (native_anchor - native) & mask;
    const auto scaled = static_cast<long double>(magnitude) * period_ns * frequency / 1.0e9L;
    const auto rounded = forward ? std::floor(scaled) : std::ceil(scaled);
    if (!std::isfinite(rounded) || rounded < 0 || rounded >= 18446744073709551616.0L) return false;
    const auto ticks = static_cast<uint64_t>(rounded);
    if (forward ? ticks > UINT64_MAX - host_anchor : ticks > host_anchor) return false;
    host = forward ? host_anchor + ticks : host_anchor - ticks;
    return true;
}
// One bounded capture, armed by a NEW frame-timeline.trigger in cwd.
// Host events and calibrated native samples use QPC ticks (not guest TSC).
enum class Kind : uint32_t {
    ThreadName, Enqueue, SuspendEnter, SuspendReturn, SuspendRelease,
    SliceBegin, SliceEnd, CpuFrameEnd, FrameBoundary,
    FlipLink, FlipReady, FlipPresented, CpuFlipCall,
    NativeSubmit, NativeBegin, NativeEnd, Calibration, NativeUnavailable,
    WaitRegMemBlocked, WaitRegMemPassed, WaitFlipBlocked, WaitFlipPassed,
    EqueueBegin, EqueueEnd, EqueueEvent, EventTrigger,
    VblankBegin, VblankEnd, PriorityBegin, PriorityEnd,
    PriorityDrainBegin, PriorityDrainEnd, LabelWrite, ClockPublish,
    FrameNativeBegin, FrameNativeEnd, FrameCpuComplete, NativeWaitBegin, NativeWaitEnd
};
inline std::atomic<bool> active {false};
struct Context { uint64_t frame = 0, submission = 0; };
inline thread_local Context context;
bool Enabled();
uint64_t Now();
void Start();
void Stop();
void Emit(Kind kind, uint64_t frame, uint64_t submission, uint64_t tick,
          uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t at);
inline bool Recording() { return active.load(std::memory_order_relaxed); }
inline void Event(Kind kind, uint64_t frame = 0, uint64_t submission = 0, uint64_t tick = 0,
                  uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t at = 0) {
    if (Recording()) Emit(kind, frame, submission, tick, a, b, c, d, at);
}
inline void Current(Kind kind, uint64_t tick = 0, uint64_t a = 0, uint64_t b = 0,
                    uint64_t c = 0, uint64_t d = 0) {
    if (Recording()) Emit(kind, context.frame, context.submission, tick, a, b, c, d, 0);
}
inline thread_local bool thread_named = false;
inline bool NeedsName() { return Recording() && !thread_named; }
void NameImpl(const char* name);
inline void Name(const char* name) { if (NeedsName()) NameImpl(name); }
class ContextScope {
public:
    ContextScope(uint64_t frame, uint64_t submission): enabled(Enabled()) {
        if (enabled) { previous = context; context = {frame, submission}; }
    }
    ~ContextScope() { if (enabled) context = previous; }
private:
    bool enabled;
    Context previous;
};
class WaitScope {
public:
    WaitScope(Kind begin, Kind end, uint64_t object = 0): end(end), object(object), armed(Recording()) {
        if (armed) Current(begin, 0, object);
    }
    ~WaitScope() { if (armed) Current(end, 0, object); }
private:
    Kind end;
    uint64_t object;
    bool armed;
};
class Session {
public:
    Session() { if (Enabled()) Start(); }
    ~Session() { if (Enabled()) Stop(); }
};
} // namespace Common::FrameTimeline
