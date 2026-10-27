#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_

#include "common/bitArray.h"
#include "common/common.h"
#include "kernel/memory.h"

#include <compare>

namespace Libs::Graphics {

constexpr uint64_t TRACKER_PAGE_SIZE    = 4ull * 1024ull;
constexpr uint64_t TRACKER_REGION_SIZE  = 4ull * 1024ull * 1024ull;
constexpr uint64_t TRACKER_ADDRESS_SIZE = 1ull << 44u;
constexpr uint64_t LOWER_ADDRESS_SIZE   = 1ull << 40u;
constexpr size_t   TRACKER_REGION_PAGES = TRACKER_REGION_SIZE / TRACKER_PAGE_SIZE;

struct GuestRange {
	uint64_t address = 0;
	uint64_t size    = 0;

	[[nodiscard]] constexpr bool Empty() const noexcept { return address == 0 && size == 0; }
	[[nodiscard]] constexpr bool Valid() const noexcept {
		if (address == 0 || size == 0) return false;
		if (address < LOWER_ADDRESS_SIZE) return size <= LOWER_ADDRESS_SIZE - address;
		constexpr auto base = LibKernel::Memory::kExtendedMemoryBase;
		constexpr auto length = LibKernel::Memory::kExtendedMemorySize;
		return address >= base && address - base < length && size <= length - (address - base);
	}
	[[nodiscard]] constexpr bool     ValidOrEmpty() const noexcept { return Empty() || Valid(); }
	[[nodiscard]] constexpr uint64_t End() const noexcept { return address + size; }
	auto                             operator<=>(const GuestRange&) const = default;
};

enum class DirtySource { Cpu, Gpu };
// GPU-lane history. Three busy frames in a twelve-frame window includes rings
// rotating every third frame; one loading burst cannot promote an owner.
struct HotBufferHistory {
	std::array<uint64_t, 12> faults {};
	size_t cursor = 0, frames = 0;
	void Observe(uint64_t count) { faults[cursor] = count; cursor = (cursor + 1) % faults.size(); if (frames < faults.size()) ++frames; }
	uint64_t Total() const { uint64_t n = 0; for (auto count: faults) n += count; return n; }
	bool Ready() const { unsigned busy = 0; for (auto count: faults) busy += count >= 256; return frames == faults.size() && busy >= 3; }
	uint64_t Mean() const { return frames ? Total() / frames : 0; }
};
using RegionBits = Common::BitArray<TRACKER_REGION_PAGES>;
struct FaultCoarsenStats {
	uint64_t faults = 0, unique = 0, repeat = 0, refault_upload = 0;
	uint64_t expanded = 0, dirty_bytes = 0, new_dirty_bytes = 0, extra_dirty_upper = 0;
	uint64_t upload_bytes = 0, extra_upload_upper = 0;
	uint64_t reject_gpu = 0, reject_import = 0, reject_watcher = 0, reject_readback = 0;
	FaultCoarsenStats& operator+=(const FaultCoarsenStats& s) {
		faults += s.faults; unique += s.unique; repeat += s.repeat; refault_upload += s.refault_upload;
		expanded += s.expanded; dirty_bytes += s.dirty_bytes; new_dirty_bytes += s.new_dirty_bytes;
		extra_dirty_upper += s.extra_dirty_upper; upload_bytes += s.upload_bytes; extra_upload_upper += s.extra_upload_upper;
		reject_gpu += s.reject_gpu; reject_import += s.reject_import;
		reject_watcher += s.reject_watcher; reject_readback += s.reject_readback;
		return *this;
	}
};
static_assert(sizeof(RegionBits) == TRACKER_REGION_PAGES / 8);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONDEFINITIONS_H_
