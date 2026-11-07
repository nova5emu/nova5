#pragma once

#include "graphics/host_gpu/renderer/pipeline/preparedResourceReads.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include <type_traits>
#include <utility>

namespace Libs::Graphics {

// Jetsku's PreparedState/MaterializeReusing (pipelineCache.cpp, 6a7f10ee), adapted
// to Nova5's runtime callbacks. No projected keys or history: all user data and
// every observed byte must match. Mapping-end probes are dependencies too.
class ResourceReuse {
public:
	using Runtime = ShaderRecompiler::IR::SrtRuntime;
	using Snapshot = ShaderRecompiler::IR::ResourceSnapshot;
	using Specialization = ShaderRecompiler::IR::ResourceSpecialization;

	template <typename Reader>
	bool Matches(const Runtime& runtime, std::span<uint8_t, PreparedResourceReads::MaxBytes> scratch,
	             Reader&& clean_read) {
		if (!m_reads.Matches(runtime.user_data, runtime.shader_base, {}, false)) return false;
		for (const auto& [address, end]: m_mapping_ends) {
			if (!runtime.mapping_end || runtime.mapping_end(runtime.userdata, address) != end) {
				m_reads.Invalidate();
				return false;
			}
		}
		if (m_reads.Validate(scratch, clean_read)) return true;
		m_reads.Invalidate();
		return false;
	}

	Runtime Begin(const Runtime& runtime, std::span<uint8_t, PreparedResourceReads::MaxBytes> scratch) {
		m_original = runtime;
		m_reads.Begin(runtime.user_data, runtime.shader_base, scratch);
		m_mapping_ends.clear();
		m_failed = false;
		auto result = runtime;
		result.userdata = this;
		result.read_memory = ReadRaw;
		result.read_specialization_memory = runtime.read_specialization_memory ? ReadClean : nullptr;
		result.try_read_memory_span = runtime.try_read_memory_span ? ReadSpan : nullptr;
		result.sync_memory = runtime.sync_memory ? Sync : nullptr;
		result.mapping_end = runtime.mapping_end ? MappingEnd : nullptr;
		return result;
	}

	void Finish(bool success) { m_reads.Finish(success && !m_failed); m_original = {}; }
	void Invalidate() { m_reads.Invalidate(); }
	bool Valid() const { return m_reads.Valid(); }
	size_t CapacityBytes() const {
		const auto bytes = [](const auto& v) { return v.capacity() * sizeof(typename std::decay_t<decltype(v)>::value_type); };
		return sizeof(*this) + m_reads.CapacityBytes() + bytes(m_mapping_ends) +
		       bytes(resources.buffers) + bytes(resources.images) + bytes(resources.samplers) +
		       bytes(resources.flattened_srt) + bytes(resources.user_data) +
		       bytes(resources.bindless_heaps) +
		       bytes(specialization.buffers) + bytes(specialization.images);
	}
	Snapshot resources;
	Specialization specialization;

	static bool Equal(const Snapshot& a, const Snapshot& b) {
		return a.buffers == b.buffers && a.images == b.images && a.samplers == b.samplers &&
		       a.flattened_srt == b.flattened_srt && a.user_data == b.user_data && a.uniform_fill == b.uniform_fill &&
		       a.bindless_heaps == b.bindless_heaps;
	}

private:
	static bool Read(void* data, uint64_t address, uint32_t* value, bool clean) {
		auto& self = *static_cast<ResourceReuse*>(data);
		const auto reader = clean ? self.m_original.read_specialization_memory : self.m_original.read_memory;
		bool ok = true;
		if (reader) ok = reader(self.m_original.userdata, address, value);
		else std::memcpy(value, reinterpret_cast<const void*>(address), sizeof(*value));
		// Failed reads invalidate capture without inspecting an uninitialized value.
		PreparedResourceReads::Observe(&self.m_reads, address, ok ? std::span<const uint32_t>(value, 1) : std::span<const uint32_t>{}, ok);
		return ok;
	}
	static bool ReadRaw(void* data, uint64_t address, uint32_t* value) { return Read(data, address, value, false); }
	static bool ReadClean(void* data, uint64_t address, uint32_t* value) { return Read(data, address, value, true); }
	static bool ReadSpan(void* data, uint64_t address, uint32_t* values, uint32_t count, bool clean) {
		auto& self = *static_cast<ResourceReuse*>(data);
		const auto ok = self.m_original.try_read_memory_span(self.m_original.userdata, address, values, count, clean);
		// A rejected optional span is followed by scalar reads; it did not observe bytes.
		if (ok) PreparedResourceReads::Observe(&self.m_reads, address, {values, count}, true);
		return ok;
	}
	static bool Sync(void* data, uint64_t address, uint64_t size) {
		auto& self = *static_cast<ResourceReuse*>(data);
		const auto ok = self.m_original.sync_memory(self.m_original.userdata, address, size);
		// A sync has side effects even if no later read observes the bytes. Never skip it.
		self.m_failed = true;
		return ok;
	}
	static uint64_t MappingEnd(void* data, uint64_t address) {
		auto& self = *static_cast<ResourceReuse*>(data);
		const auto end = self.m_original.mapping_end(self.m_original.userdata, address);
		if (self.m_mapping_ends.size() >= PreparedResourceReads::MaxRanges) self.m_failed = true;
		else self.m_mapping_ends.emplace_back(address, end);
		return end;
	}
	PreparedResourceReads m_reads;
	std::vector<std::pair<uint64_t, uint64_t>> m_mapping_ends;
	Runtime m_original;
	bool m_failed = false;
};
} // namespace Libs::Graphics
