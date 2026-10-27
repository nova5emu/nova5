#include "common/common.h"

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// #error "KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS"
#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep

#include "common/platform/sysFileIO.h"
#include "common/platform/sysTimer.h"
#include "common/stringUtils.h"
#include "common/hostPath.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// NOLINTNEXTLINE(readability-identifier-naming)
enum sys_file_type_t {
	SYS_FILE_ERROR,       // NOLINT(readability-identifier-naming)
	SYS_FILE_MEMORY_STAT, // NOLINT(readability-identifier-naming)
	SYS_FILE_FILE,        // NOLINT(readability-identifier-naming)
	SYS_FILE_MEMORY_DYN   // NOLINT(readability-identifier-naming)
};

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_file_mem_buf_t {
	uint8_t* base;
	uint8_t* ptr;
	uint32_t size;
};

// NOLINTNEXTLINE(readability-identifier-naming)
struct sys_file_t {
	sys_file_type_t type;
	union {
		HANDLE              handle;
		sys_file_mem_buf_t* buf;
	};
};

// IWYU pragma: no_include <fileapi.h>
// IWYU pragma: no_include <handleapi.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <winbase.h>

constexpr DWORD FILE_SHARE_POSIX = static_cast<DWORD>(FILE_SHARE_READ) |
                                   static_cast<DWORD>(FILE_SHARE_WRITE) |
                                   static_cast<DWORD>(FILE_SHARE_DELETE);

static bool FileResult(bool ok, const char* operation, const std::filesystem::path& path) {
	if (!ok) {
		const DWORD error = GetLastError();
		Common::ReportHostFileError(operation, path, std::error_code(error, std::system_category()));
		SetLastError(error);
	}
	return ok;
}

static DWORD GetCacheAccessType(sys_file_cache_type_t t) {
	if (t == SYS_FILE_CACHE_RANDOM_ACCESS) {
		return FILE_FLAG_RANDOM_ACCESS;
	}

	if (t == SYS_FILE_CACHE_SEQUENTIAL_SCAN) {
		return FILE_FLAG_SEQUENTIAL_SCAN;
	}

	return FILE_ATTRIBUTE_NORMAL;
}

void SysFileRead(void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_read) {
	if (f.type == SYS_FILE_FILE) {
		DWORD      w  = 0;
		const BOOL ok = ReadFile(f.handle, data, size, &w, nullptr);
		const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
		if (!ok) {
			std::fprintf(stderr, "[FileRead] Windows error=%lu requested=%u read=%lu destination=%p\n",
			             static_cast<unsigned long>(error), size, static_cast<unsigned long>(w), data);
			std::fflush(stderr);
		}
		if (!ok && w == 0 && size != 0 &&
		    (error == ERROR_NOACCESS || error == ERROR_INVALID_USER_BUFFER)) {
			// Kernel writes cannot trigger the guest-memory fault handler. GPU tracking
			// may protect this destination again after its explicit invalidation.
			// Copy through host memory so the normal user-space handler tracks the write.
			// Windows can also reject a guest destination with ERROR_INVALID_USER_BUFFER;
			// retry only a failed zero-byte transfer, never a successful EOF or partial read.
			std::vector<uint8_t> staging((std::min)(size, uint32_t {64 * 1024}));
			while (w < size) {
				const DWORD request =
				    (std::min)(static_cast<DWORD>(size) - w, static_cast<DWORD>(staging.size()));
				DWORD read = 0;
				const BOOL staged_ok = ReadFile(f.handle, staging.data(), request, &read, nullptr);
				const DWORD staged_error = staged_ok ? ERROR_SUCCESS : GetLastError();
				if (!staged_ok || read == 0) {
					std::fprintf(stderr, "[FileRead] staging stopped error=%lu requested=%lu read=%lu\n",
					             static_cast<unsigned long>(staged_error), static_cast<unsigned long>(request),
					             static_cast<unsigned long>(read));
					break;
				}
				std::memcpy(static_cast<uint8_t*>(data) + w, staging.data(), read);
				w += read;
				if (read < request) {
					break;
				}
			}
			std::fprintf(stderr, "[FileRead] staging result requested=%u read=%lu\n", size,
			             static_cast<unsigned long>(w));
			std::fflush(stderr);
		}
		if (bytes_read != nullptr) {
			*bytes_read = w;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT) {
		uint32_t s = size;
		if (f.buf->size != 0u) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		}
		std::memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr) {
			*bytes_read = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN) {
		uint32_t s = size;
		if (f.buf->size != 0u) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		} else {
			s = 0;
		}
		std::memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr) {
			*bytes_read = s;
		}
	}
}

void SysFileWrite(const void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_written) {
	if (f.type == SYS_FILE_FILE) {
		DWORD w = 0;
		WriteFile(f.handle, data, size, &w, nullptr);
		if (bytes_written != nullptr) {
			*bytes_written = w;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT) {
		uint32_t s = size;
		if (f.buf->size != 0u) {
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l) {
				s = l;
			}
		}
		std::memcpy(f.buf->ptr, data, s);
		f.buf->ptr += s;
		if (bytes_written != nullptr) {
			*bytes_written = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN) {
		uint32_t pos = f.buf->ptr - f.buf->base;
		if (f.buf->size < pos + size) {
			f.buf->base = static_cast<uint8_t*>(std::realloc(f.buf->base, pos + size));
			f.buf->ptr  = f.buf->base + pos;
			f.buf->size = pos + size;
		}
		std::memcpy(f.buf->ptr, data, size);
		f.buf->ptr += size;
		if (bytes_written != nullptr) {
			*bytes_written = size;
		}
	}
}

sys_file_t* SysFileCreate(const std::filesystem::path& file_name) {
	auto* ret = new sys_file_t;

	auto   wide   = Common::NativeHostPath(file_name).wstring();
	HANDLE h_file = nullptr;
	h_file = CreateFileW(wide.c_str(),
	                     static_cast<DWORD>(GENERIC_READ) | static_cast<DWORD>(GENERIC_WRITE) |
	                         static_cast<DWORD>(DELETE),
	                     FILE_SHARE_POSIX, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

	FileResult(h_file != INVALID_HANDLE_VALUE, "CreateFileW", file_name);
	ret->handle = h_file;
	ret->type   = SYS_FILE_FILE;

	return ret;
}

sys_file_t* SysFileOpenR(const std::filesystem::path& file_name, sys_file_cache_type_t cache_type) {
	auto* ret = new sys_file_t;

	auto   wide   = Common::NativeHostPath(file_name).wstring();
	HANDLE h_file = nullptr;
	h_file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_POSIX, nullptr, OPEN_EXISTING,
	                     GetCacheAccessType(cache_type), nullptr);

	if (h_file == INVALID_HANDLE_VALUE) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	FileResult(h_file != INVALID_HANDLE_VALUE, "CreateFileW", file_name);
	ret->handle = h_file;

	return ret;
}

sys_file_t* SysFileOpen(uint8_t* buf, uint32_t buf_size) {
	auto* ret = new sys_file_t;

	ret->type      = SYS_FILE_MEMORY_STAT;
	ret->buf       = new sys_file_mem_buf_t;
	ret->buf->base = buf;
	ret->buf->ptr  = buf;
	ret->buf->size = buf_size;

	return ret;
}

sys_file_t* SysFileCreate() {
	auto* ret = new sys_file_t;

	ret->type      = SYS_FILE_MEMORY_DYN;
	ret->buf       = new sys_file_mem_buf_t;
	ret->buf->base = nullptr;
	ret->buf->ptr  = nullptr;
	ret->buf->size = 0;

	return ret;
}

sys_file_t* SysFileOpenW(const std::filesystem::path& file_name, sys_file_cache_type_t cache_type) {
	auto* ret = new sys_file_t;

	auto   wide   = Common::NativeHostPath(file_name).wstring();
	HANDLE h_file = nullptr;
	h_file        = CreateFileW(
	    wide.c_str(), static_cast<DWORD>(GENERIC_WRITE) | static_cast<DWORD>(DELETE),
	    FILE_SHARE_POSIX, nullptr, OPEN_EXISTING, GetCacheAccessType(cache_type), nullptr);

	if (h_file == INVALID_HANDLE_VALUE) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	FileResult(h_file != INVALID_HANDLE_VALUE, "CreateFileW", file_name);
	ret->handle = h_file;

	return ret;
}

sys_file_t* SysFileOpenRw(const std::filesystem::path& file_name,
                          sys_file_cache_type_t        cache_type) {
	auto* ret = new sys_file_t;

	auto   wide   = Common::NativeHostPath(file_name).wstring();
	HANDLE h_file = nullptr;
	h_file = CreateFileW(wide.c_str(),
	                     static_cast<DWORD>(GENERIC_READ) | static_cast<DWORD>(GENERIC_WRITE) |
	                         static_cast<DWORD>(DELETE),
	                     FILE_SHARE_POSIX, nullptr, OPEN_EXISTING, GetCacheAccessType(cache_type),
	                     nullptr);

	if (h_file == INVALID_HANDLE_VALUE) {
		ret->type = SYS_FILE_ERROR;
	} else {
		ret->type = SYS_FILE_FILE;
	}

	FileResult(h_file != INVALID_HANDLE_VALUE, "CreateFileW", file_name);
	ret->handle = h_file;

	return ret;
}

void SysFileClose(sys_file_t* f) {
	if (f->type == SYS_FILE_FILE) {
		CloseHandle(f->handle);
	} else if (f->type == SYS_FILE_MEMORY_STAT) {
		delete f->buf;
	} else if (f->type == SYS_FILE_MEMORY_DYN) {
		std::free(f->buf->base);
		delete f->buf;
	}

	// f.type = SYS_FILE_ERROR;
	delete f;
}

uint64_t SysFileSize(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s;
		GetFileSizeEx(f.handle, &s);
		return s.QuadPart;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		return f.buf->size;
	}

	return 0;
}

bool SysFileGetInfo(const std::filesystem::path& name, bool* is_file, uint64_t* size) {
	return SysFileGetInfo(name, is_file, size, nullptr);
}

bool SysFileGetInfo(const std::filesystem::path& name, bool* is_file, uint64_t* size, std::error_code* error) {
	if (error) { error->clear(); }
	const bool directory_path = !name.empty() && !name.has_filename() && name != name.root_path();
	WIN32_FILE_ATTRIBUTE_DATA info {};
	std::error_code normalization_error;
	const auto native = Common::NativeHostPath(directory_path ? name.parent_path() : name, &normalization_error);
	if (normalization_error) {
		if (error) { *error = normalization_error; }
		return false;
	}
	if (GetFileAttributesExW(native.c_str(),
	                         GetFileExInfoStandard, &info) == 0) {
		const DWORD failure = GetLastError();
		const std::error_code code(failure, std::system_category());
		if (error) { *error = code; }
		Common::ReportHostFileError("GetFileAttributesExW", name, code);
		SetLastError(failure);
		return false;
	}
	const bool file = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	if (directory_path && file) {
		if (error) { *error = std::make_error_code(std::errc::not_a_directory); }
		return false;
	}
	*is_file = file;
	*size = *is_file ? ((static_cast<uint64_t>(info.nFileSizeHigh) << 32u) | info.nFileSizeLow) : 0;
	return true;
}

uint64_t SysFileSize(const std::filesystem::path& file_name) {
	bool is_file;
	uint64_t size = 0;
	SysFileGetInfo(file_name, &is_file, &size);
	return size;
}

bool SysFileTruncate(sys_file_t& f, uint64_t size) {
	bool ok = false;
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s {};
		LARGE_INTEGER r {};
		s.QuadPart = 0;
		SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
		s.QuadPart = static_cast<LONGLONG>(size);
		ok         = (SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0 &&
		              SetEndOfFile(f.handle) != 0);
		SetFilePointerEx(f.handle, r, nullptr, FILE_BEGIN);
	}

	return ok;
}

bool SysFileUnlink(sys_file_t& f, const std::filesystem::path& name) {
	if (f.type == SYS_FILE_FILE) {
		FILE_DISPOSITION_INFO info {};
		info.DeleteFile = TRUE;
		if (SetFileInformationByHandle(f.handle, FileDispositionInfo, &info, sizeof(info)) != 0) {
			return true;
		}
	}

	return SysFileDeleteFile(name);
}

bool SysFileSeek(sys_file_t& f, uint64_t offset) {
	bool ok = true;
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s;
		s.QuadPart = static_cast<LONGLONG>(offset);
		ok         = (SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0);
		// printf("seek: %u\n", offset);
	} else if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		f.buf->ptr = f.buf->base + offset;
	}

	return ok;
}

uint64_t SysFileTell(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE) {
		LARGE_INTEGER s {};
		LARGE_INTEGER r {};
		s.QuadPart = 0;
		SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
		// printf("tell: %u\n", r.QuadPart);
		return r.QuadPart;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		return f.buf->ptr - f.buf->base;
	}

	return 0;
}

bool SysFileIsError(sys_file_t& f) {
	return f.type == SYS_FILE_ERROR ||
	       (f.type == SYS_FILE_FILE && f.handle == INVALID_HANDLE_VALUE);
}

bool SysFileIsDirectoryExisting(const std::filesystem::path& path) {
	bool is_file;
	uint64_t size;
	return SysFileGetInfo(path, &is_file, &size) && !is_file;
}

bool SysFileIsFileExisting(const std::filesystem::path& name) {
	bool is_file;
	uint64_t size;
	return SysFileGetInfo(name, &is_file, &size) && is_file;
}

bool SysFileCreateDirectory(const std::filesystem::path& path) {
	auto wide = Common::NativeHostPath(path).wstring();
	return FileResult(CreateDirectoryW(wide.c_str(), nullptr) != 0, "CreateDirectoryW", path);
}

bool SysFileDeleteDirectory(const std::filesystem::path& path) {
	auto wide = Common::NativeHostPath(path).wstring();
	return FileResult(RemoveDirectoryW(wide.c_str()) != 0, "RemoveDirectoryW", path);
}

bool SysFileDeleteFile(const std::filesystem::path& name) {
	auto wide = Common::NativeHostPath(name).wstring();
	return FileResult(DeleteFileW(wide.c_str()) != 0, "DeleteFileW", name);
}

bool SysFileFlush(sys_file_t& f) {
	if (f.type == SYS_FILE_FILE && f.handle != INVALID_HANDLE_VALUE) {
		return (FlushFileBuffers(f.handle) != 0);
	}

	return false;
}

SysFileTimeStruct SysFileGetLastAccessTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct r {};
	sys_file_t*       f = SysFileOpenR(name);
	r.is_invalid =
	    (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, &r.time, nullptr) == 0));
	SysFileClose(f);
	return r;
}

SysFileTimeStruct SysFileGetLastWriteTimeUtc(const std::filesystem::path& name) {
	SysFileTimeStruct r {};
	sys_file_t*       f = SysFileOpenR(name);
	r.is_invalid =
	    (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, nullptr, &r.time) == 0));
	SysFileClose(f);
	return r;
}

void SysFileGetLastAccessAndWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	// TODO() open file with dwDesiredAccess = 0
	// TODO() open directory
	sys_file_t* f = SysFileOpenR(name);
	a.is_invalid  = w.is_invalid =
	    (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, &a.time, &w.time) == 0));
	SysFileClose(f);
}

void SysFileGetLastAccessAndWriteTimeUtc(sys_file_t& f, SysFileTimeStruct& a,
                                         SysFileTimeStruct& w) {
	if (f.type == SYS_FILE_FILE) {
		a.is_invalid = w.is_invalid = (GetFileTime(f.handle, nullptr, &a.time, &w.time) == 0);
	} else if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN) {
		SysTimeStruct t {};
		SysGetSystemTimeUtc(t);
		SysSystemToFileTimeUtc(t, a);
		SysSystemToFileTimeUtc(t, w);
	} else {
		a.is_invalid = w.is_invalid = true;
	}
}

bool SysFileSetLastAccessTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& access) {
	if (access.is_invalid) {
		return false;
	}

	bool ok = true;

	sys_file_t* f = SysFileOpenW(name);

	ok = !(f->type == SYS_FILE_ERROR ||
	       (SetFileTime(f->handle, nullptr, &access.time, nullptr) == 0));

	SysFileClose(f);

	return ok;
}

bool SysFileSetLastWriteTimeUtc(const std::filesystem::path& name, SysFileTimeStruct& write) {
	if (write.is_invalid) {
		return false;
	}

	bool ok = true;

	sys_file_t* f = SysFileOpenW(name);

	ok = !(f->type == SYS_FILE_ERROR ||
	       (SetFileTime(f->handle, nullptr, nullptr, &write.time) == 0));

	SysFileClose(f);

	return ok;
}

bool SysFileSetLastAccessAndWriteTimeUtc(const std::filesystem::path& name,
                                         SysFileTimeStruct& access, SysFileTimeStruct& write) {
	if (access.is_invalid || write.is_invalid) {
		return false;
	}

	bool ok = true;

	sys_file_t* f = SysFileOpenW(name);

	ok = !(f->type == SYS_FILE_ERROR ||
	       (SetFileTime(f->handle, nullptr, &access.time, &write.time) == 0));

	SysFileClose(f);

	return ok;
}

void SysFileGetDents(const std::filesystem::path& path, std::vector<sys_dir_entry_t>& out) {
	const auto pattern = Common::NativeHostPath(path / L"*");

	WIN32_FIND_DATAW data {};
	HANDLE h = FindFirstFileW(pattern.c_str(), &data);

	if (h == INVALID_HANDLE_VALUE) {
		FileResult(false, "FindFirstFileW", path);
		return;
	}

	do {
		std::filesystem::path file_name(data.cFileName);

		sys_dir_entry_t r {};

		r.is_file = ((data.dwFileAttributes & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) == 0u);
		r.name    = Common::PathToString(file_name);

		out.push_back(std::move(r));

	} while (FindNextFileW(h, &data) != 0);

	const DWORD error = GetLastError();
	if (error != ERROR_NO_MORE_FILES) {
		Common::ReportHostFileError("FindNextFileW", path, std::error_code(error, std::system_category()));
	}
	FindClose(h);
}

bool SysFileCopyFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto src_wide = Common::NativeHostPath(src).wstring();
	auto dst_wide = Common::NativeHostPath(dst).wstring();
	return FileResult(CopyFileW(src_wide.c_str(), dst_wide.c_str(), FALSE) != 0, "CopyFileW", dst);
}

bool SysFileRenameFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
	auto src_wide = Common::NativeHostPath(src).wstring();
	auto dst_wide = Common::NativeHostPath(dst).wstring();
	return FileResult(MoveFileW(src_wide.c_str(), dst_wide.c_str()) != 0, "MoveFileW", dst);
}

void SysFileRemoveReadonly(const std::filesystem::path& name) {
	auto wide = Common::NativeHostPath(name).wstring();
	const auto attributes = GetFileAttributesW(wide.c_str());
	if (!FileResult(attributes != INVALID_FILE_ATTRIBUTES, "GetFileAttributesW", name)) { return; }
	FileResult(SetFileAttributesW(wide.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)) != 0,
	           "SetFileAttributesW", name);
}

#endif
