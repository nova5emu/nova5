#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Common {
struct AudioSignal {
	size_t scanned = 0, nonzero = 0, nonfinite = 0;
	double peak = 0, rms = 0;
};

// Diagnostic only: caller supplies a valid PCM buffer. Bound work even for large batches.
inline AudioSignal MeasureAudioSignal(const void* data, size_t samples, bool floating) {
	AudioSignal result;
	result.scanned      = std::min<size_t>(samples, 65536);
	const auto* bytes   = static_cast<const uint8_t*>(data);
	double      squares = 0;
	for (size_t i = 0; i < result.scanned; ++i) {
		double sample;
		if (floating) {
			float value;
			std::memcpy(&value, bytes + i * sizeof(value), sizeof(value));
			sample = value;
		} else {
			int16_t value;
			std::memcpy(&value, bytes + i * sizeof(value), sizeof(value));
			sample = value / 32768.0;
		}
		if (!std::isfinite(sample)) {
			++result.nonfinite;
			continue;
		}
		result.nonzero += sample != 0;
		result.peak = std::max(result.peak, std::abs(sample));
		squares += sample * sample;
	}
	const auto finite = result.scanned - result.nonfinite;
	result.rms        = finite == 0 ? 0 : std::sqrt(squares / finite);
	return result;
}
} // namespace Common
