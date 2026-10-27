#include "common/file.h"
#include "common/hostPath.h"
#include "common/platform/sysFileIO.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef CreateDirectory
#undef DeleteFile
#undef CopyFile

namespace {
using Common::File;
using Common::NativeHostPath;
void Check(bool ok, const char* what) {
	if (!ok) { std::fprintf(stderr, "WindowsLongPathTests: %s (win32=%lu)\n", what, GetLastError()); std::abort(); }
}
void Exercise(const std::filesystem::path& root, size_t length) {
	auto parent = root / L"[spaces and brackets]" / L"_SaveData" / L"se\u00f1al";
	while (length > parent.native().size() + 100) { parent /= std::wstring(40, L'd'); }
	Check(length > parent.native().size() + 6, "fixture prefix fits requested length");
	const auto file = parent / (std::wstring(length-parent.native().size()-6, L'b') + L".bank");
	Check(file.native().size() == length, "exact UTF16 path length");
	Check(File::CreateDirectories(parent), "recursive mkdir");
	File writer;
	Check(writer.Create(file), "create long file");
	const char payload[] = "RIFF-data-with-spaces-and-brackets";
	uint32_t bytes = 0;
	writer.Write(payload, sizeof(payload), &bytes);
	Check(bytes == sizeof(payload) && writer.Flush(), "write and flush");
	Check(writer.Size() == sizeof(payload), "fstat size");
	Check(writer.Truncate(17) && writer.Size() == 17, "truncate by handle");
	writer.Close();
	std::error_code error;
	const auto info = File::GetInfo(file, &error);
	Check(info && !error && info->is_file && info->size == 17, "stat by path");
	File reader;
	// Ordinary relative paths, forward slashes and dot components normalize before prefixing.
	auto relative = file.lexically_relative(std::filesystem::current_path());
	auto dotted = relative.parent_path() / L"." / L"unused" / L".." / relative.filename();
	Check(reader.Open(std::filesystem::path(dotted.generic_wstring()), File::Mode::Read), "relative dot/slash open");
	char buffer[32] {};
	reader.Read(buffer, sizeof(buffer), &bytes);
	Check(bytes == 17 && std::memcmp(buffer, payload, 17) == 0, "read actual content and EOF");
	Check(reader.Seek(5) && reader.Tell() == 5, "seek");
	reader.Close();
	const auto entries = File::GetDirEntries(parent);
	Check(std::any_of(entries.begin(), entries.end(), [&](const auto& e) {
		return e.is_file && std::filesystem::path(e.name) == file.filename();
	}), "directory enumeration");
	Check(!File::GetInfo(file / L"", &error) && error == std::errc::not_a_directory, "file trailing slash remains invalid");
	const auto renamed = parent / L"renamed [bank].bank";
	Check(File::RenameFile(file, renamed), "rename long source");
	Check(File::CopyFile(renamed, file), "copy long destination");
	Check(!SysFileRenameFile(renamed, file), "native rename still refuses replacement");
	Check(File::DeleteFile(renamed), "unlink");
	// SaveData's temporary-file replacement and std::filesystem traversal/resize route.
	auto temporary = file; temporary += ".tmp";
	Check(File::CopyFile(file, temporary), "save temporary copy");
	std::filesystem::rename(NativeHostPath(temporary), NativeHostPath(file), error);
	Check(!error, "save replacement rename");
	std::filesystem::resize_file(NativeHostPath(file), 7, error);
	Check(!error && File::GetInfo(file)->size == 7, "truncate by path");
	size_t files = 0;
	for (const auto& entry: std::filesystem::recursive_directory_iterator(NativeHostPath(root))) {
		if (entry.is_regular_file()) { ++files; }
	}
	Check(files == 1, "save recursive enumeration");
	Check(File::DeleteFile(file), "delete long path");
	Check(!File::GetInfo(file, &error) && error == std::errc::no_such_file_or_directory, "missing remains missing");
	Check(File::DeleteDirectory(parent), "rmdir long path");
	std::printf("WindowsLongPathTests: length=%zu passed\n", length);
}
}
int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	auto temp = std::filesystem::temp_directory_path();
	if (!temp.has_filename()) { temp = temp.parent_path(); }
	const auto root = temp / (L"nova5-long-path-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
	Check(!std::filesystem::exists(NativeHostPath(root)), "fresh isolated fixture");
	Check(NativeHostPath({}).empty(), "empty path does not become cwd");
	Check(NativeHostPath(L"//server/share/a/../b").native() == L"\\\\?\\UNC\\server\\share\\b", "UNC normalization");
	Check(NativeHostPath(L"C:/a/./b/../c").native() == L"\\\\?\\C:\\a\\c", "absolute normalization");
	const auto native = NativeHostPath(root);
	Check(NativeHostPath(native) == native, "prefix idempotence");
	for (size_t length: {259u, 260u, 261u, 512u, 1024u}) { Exercise(root, length); }
	std::error_code error;
	const auto oversized = root / std::wstring(300, L'x');
	Check(!File::GetInfo(oversized, &error) && error, "real OS metadata error is returned");
	std::printf("WindowsLongPathTests: invalid component error=%d category=%s\n", error.value(), error.category().name());
	auto oversized_path = root;
	for (int i=0; i<400; ++i) { oversized_path /= std::wstring(100, L'x'); }
	Check(!File::GetInfo(oversized_path, &error) && error == std::errc::filename_too_long,
	      "OS path-length error must not become missing-file");
	// Restrict recursive cleanup to this freshly-created temporary fixture.
	Check(root.parent_path() == temp && root.filename().native().starts_with(L"nova5-long-path-"), "cleanup confinement");
	std::filesystem::remove_all(NativeHostPath(root), error);
	Check(!error, "cleanup");
	return 0;
}
