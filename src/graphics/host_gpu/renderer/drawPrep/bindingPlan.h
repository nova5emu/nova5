#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace Libs::Graphics::DrawPrep {

// First, serial slice of Jetsku d4cef64f's binding plans. Inputs are an immutable
// compiled layout and an owned ResourceSnapshot, not guest addresses to read.
// No cache handles, VM queries, uploads, callbacks or renderer state belong here.
// The consumer still clamps/acquires each range in the original binding order.
// Parallel preparation certifies the snapshot before using this plan.
struct StageBindingPlan {
	struct Range {
		uint64_t address = 0;
		uint64_t requested_size = 0;
		bool operator==(const Range&) const = default;
	};
	bool valid = false;
	std::vector<Range> ranges;
	std::vector<uint32_t> user_data;
	std::vector<uint32_t> lod_stats;

	void Reset() {
		valid = false;
		ranges.clear();
		user_data.clear();
		lod_stats.clear();
	}
};

// Refusal leaves valid=false, including when a previous invocation succeeded.
// Partial scratch output on refusal is never consumed. All malformed inputs are
// left to the ordered serial path, which retains its existing diagnostics.
inline bool BuildStageBindingPlan(const ShaderRecompiler::IR::BindingLayout& layout,
                                  uint32_t user_data_base, size_t buffer_count,
                                  const ShaderRecompiler::IR::ResourceSnapshot& resources,
                                  StageBindingPlan& plan) {
	plan.Reset();
	const uint64_t buffer_words = uint64_t {layout.memory_offset_dword} +
	                              (uint64_t {layout.memory_offset_count} + 3u) / 4u;
	const uint64_t lod_words = buffer_words + layout.buffer_word_count;
	const uint64_t total = lod_words + layout.lod_stats_count + layout.table_block_dwords;
	if (total > UINT32_MAX || layout.user_data_registers.size() > layout.memory_offset_dword ||
	    buffer_count > resources.buffers.size() || layout.lod_stats_count > resources.images.size())
		return false;
	plan.user_data.reserve(layout.user_data_registers.size());
	for (const auto reg: layout.user_data_registers) {
		if (reg < user_data_base || reg - user_data_base >= resources.user_data.size()) return false;
		plan.user_data.push_back(resources.user_data[reg - user_data_base]);
	}
	plan.ranges.reserve(buffer_count);
	for (size_t i = 0; i < buffer_count; ++i) {
		const auto& value = resources.buffers[i];
		ShaderBufferResource descriptor {};
		if (value.dword_count < sizeof(descriptor) / sizeof(uint32_t)) return false;
		std::memcpy(&descriptor, value.dwords.data(), sizeof(descriptor));
		const auto address = descriptor.Base48();
		const auto stride = descriptor.Stride();
		const auto records = descriptor.NumRecords();
		const uint64_t bytes = stride != 0 ? uint64_t {stride} * records : records;
		plan.ranges.push_back(address != 0 && bytes != 0 ? StageBindingPlan::Range {address, bytes}
		                                               : StageBindingPlan::Range {});
	}
	plan.lod_stats.reserve(layout.lod_stats_count);
	for (uint32_t i = 0; i < layout.lod_stats_count; ++i) {
		const auto& value = resources.images[i];
		ShaderTextureResource descriptor {};
		if (value.dword_count < sizeof(descriptor) / sizeof(uint32_t)) return false;
		std::memcpy(&descriptor, value.dwords.data(), sizeof(descriptor));
		plan.lod_stats.push_back(descriptor.MipStatsCntEn()
		    ? (0x80000000u | descriptor.MipStatsCntId() | (uint32_t {descriptor.BaseLevel()} << 8u) |
		       (uint32_t {descriptor.MinLodWarn5()} << 12u)) : 0u);
	}
	plan.valid = true;
	return true;
}

} // namespace Libs::Graphics::DrawPrep
