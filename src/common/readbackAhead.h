#pragma once

#include <charconv>
#include <cstdlib>
#include <string_view>

namespace Common::ReadbackAhead {
inline constexpr unsigned DefaultPages = 4;
inline constexpr unsigned MaxPages = 16;

// Strict bounded parsing: no signs, trailing text or silently oversized windows.
// Invalid/unset values preserve the measured four-page configuration.
inline unsigned ParsePages(std::string_view text) {
	if (text.empty()) return DefaultPages;
	unsigned pages = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), pages);
	return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
	       pages >= 2 && pages <= MaxPages ? pages : DefaultPages;
}

// Read once per process. The experiment gate can toggle live; these limits cannot.
inline unsigned WritePages() {
	static const unsigned pages = [] {
		const auto* value = std::getenv("KYTY_WRITE_READBACK_AHEAD_PAGES");
		return ParsePages(value ? value : "");
	}();
	return pages;
}
inline unsigned ReadPages() {
	static const unsigned pages = [] {
		const auto* value = std::getenv("KYTY_READ_READBACK_AHEAD_PAGES");
		return ParsePages(value ? value : "");
	}();
	return pages;
}
} // namespace Common::ReadbackAhead
