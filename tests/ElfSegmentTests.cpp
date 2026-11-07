// Exercises the real ELF/SELF reader with generated files; no game or GPU needed.
#include "common/assert.h"
#include "common/file.h"
#include "loader/elf.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace {
void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "ElfSegmentTests: %s\n", message);
		std::abort();
	}
}

template <typename F>
void ExpectError(F&& action, std::string_view message) {
	Common::RecoverableExitScope scope;
	try {
		action();
	} catch (const Common::RecoverableExit& error) {
		Check(error.message.find(message) != std::string::npos, "expected loader error");
		return;
	}
	Check(false, "invalid read must not succeed silently");
}

struct Fixture {
	std::filesystem::path path;
	static constexpr uint64_t ElfOffset = 0x4000;
	static constexpr uint64_t SelfOffset = 0x1000;
	// The size and page offset of libcohtml's final PT_LOAD in Oscar's copy.
	static constexpr size_t PayloadSize = 0x270a80;
	static constexpr size_t DestinationOffset = 0x2c0;

	explicit Fixture(bool self, bool truncated = false) {
		const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
		path = std::filesystem::temp_directory_path() /
		       ("nova5_elf_segments_" + std::to_string(unique) + (self ? ".self" : ".elf"));
		const auto payload_offset = self ? SelfOffset : ElfOffset;
		std::vector<uint8_t> bytes(payload_offset + PayloadSize, 0);
		uint64_t header_offset = 0;
		if (self) {
			Loader::SelfHeader header {};
			const uint8_t magic[] = {0x4f, 0x15, 0x3d, 0x1d, 0x00, 0x01, 0x01, 0x12, 0x01, 0x01, 0x00, 0x00};
			std::memcpy(header.ident, magic, sizeof(magic));
			header.unknown = 0x22;
			header.segments_num = 1;
			header.file_size = bytes.size();
			std::memcpy(bytes.data(), &header, sizeof(header));
			Loader::SelfSegment segment {0x800, SelfOffset, PayloadSize, PayloadSize};
			std::memcpy(bytes.data() + sizeof(header), &segment, sizeof(segment));
			header_offset = sizeof(header) + sizeof(segment);
		}
		Loader::Elf64_Ehdr header {};
		std::memcpy(header.e_ident, "\x7f" "ELF", 4);
		header.e_ident[Loader::EI_CLASS] = Loader::ELFCLASS64;
		header.e_ident[Loader::EI_DATA] = Loader::ELFDATA2LSB;
		header.e_ident[Loader::EI_VERSION] = Loader::EV_CURRENT;
		header.e_ident[Loader::EI_OSABI] = Loader::ELFOSABI_FREEBSD;
		header.e_ident[Loader::EI_ABIVERSION] = 2;
		header.e_type = Loader::ET_DYNAMIC;
		header.e_machine = Loader::EM_X86_64;
		header.e_version = Loader::EV_CURRENT;
		header.e_ehsize = sizeof(header);
		header.e_phoff = sizeof(header);
		header.e_phentsize = sizeof(Loader::Elf64_Phdr);
		header.e_phnum = 2;
		std::memcpy(bytes.data() + header_offset, &header, sizeof(header));
		Loader::Elf64_Phdr phdr[2] {};
		phdr[0] = {Loader::PT_LOAD, 0, ElfOffset, 0x175e2c0, 0, PayloadSize, PayloadSize, 0x4000};
		phdr[1] = {Loader::PT_LOAD, Loader::PF_R | Loader::PF_W, ElfOffset + PayloadSize,
		           0x2000000, 0, 0, 0x4000, 0x4000};
		std::memcpy(bytes.data() + header_offset + sizeof(header), phdr, sizeof(phdr));
		for (size_t i = 0; i < PayloadSize; ++i) bytes[payload_offset + i] = Pattern(i);
		if (truncated) bytes.resize(bytes.size() - 16);
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		file.close();
		Check(static_cast<bool>(file), "write synthetic module");
	}
	~Fixture() {
		std::error_code error;
		std::filesystem::remove(path, error);
	}
	static uint8_t Pattern(size_t i) { return static_cast<uint8_t>((i * 13 + i / 4096) & 255); }
};

void CheckReader(bool self) {
	Fixture fixture(self);
	Loader::Elf64 elf;
	elf.Open(fixture.path);
	Check(elf.IsValid() && elf.IsSelf() == self, "open synthetic module");
	std::vector<uint8_t> destination(Fixture::DestinationOffset + Fixture::PayloadSize + 4096, 0xa5);
	const auto address = reinterpret_cast<uint64_t>(destination.data() + Fixture::DestinationOffset);
	elf.LoadSegment(address, Fixture::ElfOffset, Fixture::PayloadSize);
	for (size_t i = 0; i < Fixture::PayloadSize; ++i) {
		Check(destination[Fixture::DestinationOffset + i] == Fixture::Pattern(i), "exact unaligned payload");
	}
	Check(std::all_of(destination.begin(), destination.begin() + Fixture::DestinationOffset,
	                  [](uint8_t v) { return v == 0xa5; }), "preserve previous segment on shared page");
	Check(std::all_of(destination.begin() + Fixture::DestinationOffset + Fixture::PayloadSize, destination.end(),
	                  [](uint8_t v) { return v == 0xa5; }), "do not overwrite tail");
	// Empty file-backed range: no matching SELF record, no access to the destination.
	elf.LoadSegment(0, Fixture::ElfOffset + Fixture::PayloadSize, 0);
	// Subrange must resolve the ELF offset through the SELF record too.
	elf.LoadSegment(address, Fixture::ElfOffset + 4093, 33);
	for (size_t i = 0; i < 33; ++i) Check(destination[Fixture::DestinationOffset + i] == Fixture::Pattern(4093 + i), "subrange");
	if (!self) {
		ExpectError([&] { elf.LoadSegment(address, Fixture::ElfOffset + Fixture::PayloadSize - 1, 2); }, "out of bounds");
		ExpectError([&] { elf.LoadSegment(address, UINT64_MAX - 7, 16); }, "out of bounds");
		ExpectError([&] { elf.LoadSegment(address, Fixture::ElfOffset, uint64_t{1} << 32); }, "out of bounds");
		ExpectError([&] { elf.LoadSegment(UINT64_MAX - 7, Fixture::ElfOffset, 16); }, "out of bounds");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Win32 kernel writes cannot invoke the guest's user-mode page-fault handler.
		// The loader must report failure, never continue with an untouched destination.
		auto* protected_page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY);
		Check(protected_page != nullptr, "allocate read-only destination");
		ExpectError([&] { elf.LoadSegment(reinterpret_cast<uint64_t>(protected_page), Fixture::ElfOffset, 32); }, "short read");
		Check(VirtualFree(protected_page, 0, MEM_RELEASE) != 0, "free read-only destination");
#endif
	}
}

void CheckTruncated(bool self) {
	Fixture fixture(self, true);
	Loader::Elf64 elf;
	elf.Open(fixture.path);
	Check(elf.IsValid(), "truncated payload still has valid headers");
	std::vector<uint8_t> destination(Fixture::PayloadSize, 0xa5);
	ExpectError([&] { elf.LoadSegment(reinterpret_cast<uint64_t>(destination.data()),
	                                Fixture::ElfOffset, Fixture::PayloadSize); }, "out of bounds");
	Check(std::all_of(destination.begin(), destination.end(), [](uint8_t v) { return v == 0xa5; }),
	      "reject truncated file before partially overwriting destination");
}
} // namespace

int main() {
	CheckReader(false);
	CheckReader(true);
	CheckTruncated(false);
	CheckTruncated(true);
	std::printf("ElfSegmentTests: all cases passed\n");
}
