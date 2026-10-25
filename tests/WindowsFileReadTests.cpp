#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "common/platform/sysFileIO.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <windows.h>

static void *protected_buffer = nullptr;
static size_t protected_size = 0;
static unsigned write_faults = 0;

static LONG CALLBACK RestoreWriteAccess(EXCEPTION_POINTERS *fault) {
  const auto *record = fault->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
      record->NumberParameters < 2 || record->ExceptionInformation[0] != 1) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const auto address = record->ExceptionInformation[1];
  const auto base = reinterpret_cast<uintptr_t>(protected_buffer);
  if (address < base || address >= base + protected_size) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  DWORD previous = 0;
  if (!VirtualProtect(protected_buffer, protected_size, PAGE_READWRITE,
                      &previous)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  ++write_faults;
  return EXCEPTION_CONTINUE_EXECUTION;
}

int main() {
  const auto path =
      std::filesystem::temp_directory_path() /
      (L"kyty-read-test-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
  std::vector<unsigned char> expected(160003);
  for (size_t i = 0; i < expected.size(); ++i)
    expected[i] = static_cast<unsigned char>(i * 37 + 11);
  auto *file = SysFileCreate(path);
  uint32_t count = 0;
  SysFileWrite(expected.data(), static_cast<uint32_t>(expected.size()), *file,
               &count);
  if (count != expected.size())
    return 2;
  SysFileClose(file);
  file = SysFileOpenR(path);
  protected_size = 200704;
  protected_buffer = VirtualAlloc(nullptr, protected_size,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!protected_buffer)
    return 3;
  auto *handler = AddVectoredExceptionHandler(1, RestoreWriteAccess);
  int failed = 0;
  for (DWORD protection : {PAGE_READWRITE, PAGE_READONLY, PAGE_NOACCESS}) {
    std::memset(protected_buffer, 0, protected_size);
    SysFileSeek(*file, 7);
    DWORD previous = 0;
    VirtualProtect(protected_buffer, protected_size, protection, &previous);
    write_faults = 0;
    count = 0;
    SetLastError(0);
    SysFileRead(protected_buffer, 180000, *file, &count);
    const auto error = GetLastError();
    VirtualProtect(protected_buffer, protected_size, PAGE_READWRITE, &previous);
    const bool ok =
        count == expected.size() - 7 &&
        std::memcmp(protected_buffer, expected.data() + 7, count) == 0 &&
        SysFileTell(*file) == expected.size();
    std::printf("protection=%lu bytes=%u error=%lu write_faults=%u %s\n",
                protection, count, error, write_faults, ok ? "PASS" : "FAIL");
    failed += !ok;
    count = 99;
    SysFileRead(protected_buffer, 1, *file, &count);
    if (ok && count != 0)
      ++failed;
  }
  // Exercise the large request observed when loading the first stage, including
  // EOF, without requiring a second game-sized fixture on disk.
  VirtualFree(protected_buffer, 0, MEM_RELEASE);
  protected_size = 159958528;
  protected_buffer = VirtualAlloc(nullptr, protected_size,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!protected_buffer)
    return 3;
  SysFileSeek(*file, 7);
  DWORD previous = 0;
  if (!VirtualProtect(protected_buffer, protected_size, PAGE_READONLY, &previous))
    return 4;
  write_faults = 0;
  count = 0;
  SysFileRead(protected_buffer, static_cast<uint32_t>(protected_size), *file, &count);
  const bool large_ok = count == expected.size() - 7 &&
      std::memcmp(protected_buffer, expected.data() + 7, count) == 0 &&
      SysFileTell(*file) == expected.size() && write_faults > 0;
  std::printf("large request=%zu bytes=%u write_faults=%u %s\n",
              protected_size, count, write_faults, large_ok ? "PASS" : "FAIL");
  failed += !large_ok;

  // A genuine EOF and a zero-sized request must leave the destination alone.
  count = 99;
  SysFileRead(protected_buffer, 1, *file, &count);
  failed += count != 0;
  SysFileSeek(*file, 7);
  count = 99;
  SysFileRead(protected_buffer, 0, *file, &count);
  failed += count != 0 || SysFileTell(*file) != 7;
  RemoveVectoredExceptionHandler(handler);
  VirtualFree(protected_buffer, 0, MEM_RELEASE);
  SysFileClose(file);
  std::filesystem::remove(path);
  return failed ? 1 : 0;
}
