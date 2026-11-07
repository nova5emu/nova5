#pragma once

#include <string_view>

namespace Libs::Graphics::Wolverine {

// A title gate even when explicitly enabled: this workaround must not change
// Astro, Demon's Souls or Sparta. The caller reads the setting once at startup.
constexpr bool LinearReadbackEnabled(std::string_view title, const char* setting) {
	return title == "PPSA03671" && (setting == nullptr || std::string_view(setting) == "1");
}

constexpr bool ShaderRecoveryEnabled(std::string_view title, const char* setting) {
	return title == "PPSA03671" && (setting == nullptr || std::string_view(setting) == "1");
}

constexpr bool BindlessImagesEnabled(std::string_view title, const char* setting) {
	return title == "PPSA03671" && (setting == nullptr || std::string_view(setting) == "1");
}

constexpr bool CoherentBuffersEnabled(std::string_view title, const char* setting) {
	return title == "PPSA03671" && (setting == nullptr || std::string_view(setting) == "1");
}

constexpr bool LoopGuardEnabled(std::string_view title, const char* setting) {
	return title == "PPSA03671" && (setting == nullptr || std::string_view(setting) == "1");
}

} // namespace Libs::Graphics::Wolverine
