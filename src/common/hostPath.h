#pragma once
#include <filesystem>
#include <system_error>

namespace Common {
// Only for host OS calls, never guest mount names or archive member paths.
// Windows: resolve relative paths and dot components before adding the extended prefix.
// Optional error preserves normalization failures (notably Windows error 206).
std::filesystem::path NativeHostPath(const std::filesystem::path& path, std::error_code* error = nullptr);
void ReportHostFileError(const char* operation, const std::filesystem::path& path,
                         const std::error_code& error);
}
