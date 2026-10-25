#pragma once

#include <cstdint>

// Runtime side of Common::SubmitProfile: file gates, the optional 1 ms timer-period A/B
// switch and the bounded periodic report. Only the guest GPU thread calls Tick/Shutdown.
namespace Libs::Graphics::SubmitProfileRuntime {

// Polls the gates (at most every 250 ms) and emits a report every 5 s while recording.
// Must be called without holding emulator locks.
void Tick();

// Ends recording and releases the timer period if it was requested.
void Shutdown();

[[nodiscard]] uint32_t ThreadId();

// Generic A/B switch for the candidate under test (KYTY_EXPERIMENT_GATE file), mirrored in
// Common::SubmitProfile::ExperimentActive() for code outside the graphics runtime.
// Raw trigger state; behavior-changing callers must additionally select an
// experiment via Common::SubmitProfile::ExperimentActive(Experiment).
[[nodiscard]] bool ExperimentActive();

} // namespace Libs::Graphics::SubmitProfileRuntime
