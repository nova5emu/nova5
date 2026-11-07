#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace Loader::LoadTrace {
// Diagnostic only: no change to memory protections or the I/O path.
inline bool Enabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("NOVA5_LOADER_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

inline long long ClockMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace Loader::LoadTrace
