#pragma once

#include <string_view>

namespace Loader::Wolverine {
// Frank 6d6f10e0 enables --redzone. Apply that default only to this title;
// the explicit CLI option keeps its meaning even when the title default is off.
constexpr bool RedZoneProtectionEnabled(bool requested, std::string_view title, const char* setting) {
	return requested || (title == "PPSA03671" &&
	                     (setting == nullptr || std::string_view(setting) == "1"));
}
} // namespace Loader::Wolverine
