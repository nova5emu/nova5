#include "common/hostPath.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Common {
std::filesystem::path NativeHostPath(const std::filesystem::path& path, std::error_code* error) {
	if (error) { error->clear(); }
#ifdef _WIN32
	if (path.empty()) { return path; } // Empty must not become the working directory.
	auto input = path.native();
	std::replace(input.begin(), input.end(), L'/', L'\\');
	// Already-native/device paths retain their namespace and literal-name semantics.
	if (input.starts_with(L"\\\\?\\") || input.starts_with(L"\\\\.\\")) { return input; }
	std::vector<wchar_t> full(256);
	for (;;) {
		const DWORD size = GetFullPathNameW(input.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
		if (size == 0) {
			const DWORD failure = GetLastError();
			const std::error_code code(failure, std::system_category());
			if (error) { *error = code; }
			ReportHostFileError("GetFullPathNameW", path, code);
			SetLastError(failure);
			return path;
		}
		if (size >= full.size()) { full.resize(static_cast<size_t>(size) + 1); continue; }
		std::wstring absolute(full.data(), size);
		if (absolute.starts_with(L"\\\\")) { return L"\\\\?\\UNC\\" + absolute.substr(2); }
		return L"\\\\?\\" + absolute;
	}
#else
	return path;
#endif
}

void ReportHostFileError(const char* operation, const std::filesystem::path& path,
                         const std::error_code& error) {
	if (!error) { return; }
	static const bool enabled = [] {
		const auto* guest = std::getenv("KYTY_GUEST_LOG");
		const auto* bank = std::getenv("KYTY_BANK_IO_TRACE");
		return (guest && std::strcmp(guest, "1") == 0) || (bank && std::strcmp(bank, "1") == 0);
	}();
	if (!enabled) { return; }
	static std::atomic<unsigned> count {0};
	const auto index = count.fetch_add(1, std::memory_order_relaxed);
	if (index > 1024) { return; }
	if (index == 1024) { std::fprintf(stderr, "[HOST_FILE_ERROR] limit reached\n"); }
	else {
		const auto utf8 = path.u8string();
		std::string escaped;
		for (auto ch: utf8) {
			if (escaped.size() >= 4096) { escaped += "...[truncated]"; break; }
			if (ch == '\n') { escaped += "\\n"; }
			else if (ch == '\r') { escaped += "\\r"; }
			else if (ch == '"') { escaped += "\\\""; }
			else { escaped += static_cast<char>(ch); }
		}
		std::fprintf(stderr, "[HOST_FILE_ERROR] op=%s category=%s code=%d path_units=%zu path=\"%s\"\n",
		             operation, error.category().name(), error.value(), path.native().size(), escaped.c_str());
	}
	std::fflush(stderr);
}
}
