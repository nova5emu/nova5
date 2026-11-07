// Resident image heaps adapted from mn3ema/KytyPS5-Frank (GPL-2.0), d6a807a4,
// with descriptor lifetime handling from 29bd4508 and write watches from c5a6961b.
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cstdio>

namespace Libs::Graphics {
namespace {
namespace IR = ShaderRecompiler::IR;
uint32_t BindingFor(const IR::ImageResource& resource) {
	using ShaderRecompiler::Decoder::ImageDimension;
	return resource.cube ? IR::BindlessImagesCube
	       : resource.dimension == ImageDimension::Dim3D ? IR::BindlessImages3D
	       : resource.dimension == ImageDimension::Dim2DArray ? IR::BindlessImages2DArray
	       : IR::BindlessImages2D;
}
bool Compatible(const ShaderTextureResource& descriptor, uint32_t binding) {
	if (descriptor.IsNull() || Prospero::SampledTextureNumericClass(descriptor.Format()) !=
	                               Prospero::TextureNumericClass::Float) return false;
	if (TextureGetSurfaceFormatInfo(descriptor.Format()).conversion_format != Prospero::BufferFormat::kInvalid)
		return false; // this path has no per-key shader conversion metadata yet
	switch (descriptor.Type()) {
		case Prospero::ImageType::kColor2D:
		case Prospero::ImageType::kColor2DArray:
			return binding == IR::BindlessImages2D || binding == IR::BindlessImages2DArray;
		// Nova5 represents the cube's faces as a 2D array. CubeAxis/CubeLayer
		// convert the instruction's coordinates; no per-key shader constants are needed.
		case Prospero::ImageType::kCube: return binding == IR::BindlessImagesCube;
		case Prospero::ImageType::kColor3D: return binding == IR::BindlessImages3D;
		default: return false;
	}
}
} // namespace

bool RenderExecutor::ResolveBindlessKey(BindlessTable::Heap& heap, uint32_t key) {
	if (key >= heap.guest_entries || heap.settled[key]) return false;
	auto& table = m_context.GetBindlessTable();
	IR::DescriptorValue value {.dword_count = 8};
	const auto address = heap.base + heap.table_offset + uint64_t {key} * 32u;
	if (!LibKernel::Memory::TryReadBacking(address, value.dwords.data(), 32u)) return false;
	const auto descriptor = DecodeNativeDescriptor<ShaderTextureResource>(value);
	if (!Compatible(descriptor, heap.binding)) {
		static uint32_t unsupported = 0;
		if (!descriptor.IsNull() && unsupported++ < 16)
			std::printf("[WOLVERINE][BINDLESS] unsupported descriptor heap=0x%llx key=%u binding=%u type=%u format=%u\n",
			            static_cast<unsigned long long>(heap.base), key, heap.binding,
			            static_cast<uint32_t>(descriptor.Type()), static_cast<uint32_t>(descriptor.Format()));
		heap.settled[key] = 1;
		table.SetTranslation(heap, key, 0);
		return false;
	}
	auto binding = ResolveTexture(heap.resource, value);
	auto& cache = m_context.GetTextureCache();
	auto* image = cache.m_slot_images.try_get(binding.image_id);
	if (image == nullptr || !image->registered || image->info.data.Empty() ||
	    image->depth_id || image->binding.needs_rebind) return false;
	BindImage(binding.image_id, false);
	const auto view = cache.FindTexture(binding.image_id, binding.desc);
	image = cache.m_slot_images.try_get(binding.image_id);
	if (image == nullptr || !image->registered || view == nullptr) return false;
	const auto slot = table.AllocateSlot(heap.binding);
	if (slot == 0) return false; // remain pending; released slots can become available later
	image->binding.bindless_general = true;
	image->binding.force_general = true;
	table.WriteSlot(heap.binding, slot, view, vk::ImageLayout::eGeneral);
	heap.settled[key] = 1;
	heap.slots[key] = slot;
	heap.images[key] = binding.image_id;
	image->usage.texture = true;
	if (std::ranges::find(heap.resolved, binding.image_id) == heap.resolved.end())
		heap.resolved.push_back(binding.image_id);
	table.AddImageReference(binding.image_id, heap, key);
	table.SetTranslation(heap, key, slot);
	static uint32_t logged = 0;
	if (logged++ < 16)
		std::printf("[WOLVERINE][BINDLESS] resolved heap=0x%llx key=%u slot=%u binding=%u\n",
		            static_cast<unsigned long long>(heap.base), key, slot, heap.binding);
	return true;
}

void RenderExecutor::PrepareBindlessHeaps(const ShaderStageRuntime& runtime, PreparedBindings& prepared) {
	prepared.bindless_patches.clear();
	prepared.bindless_heaps.clear();
	auto& table = m_context.GetBindlessTable();
	if (!table.Enabled() || runtime.resources.bindless_heaps.empty()) return;
	auto& scheduler = m_context.GetCommandScheduler();
	auto& resources = m_context.GetGpuResources();
	// CPU descriptor writes invalidate only the affected entries, including null entries.
	// Rearm before reading so a subsequent write is seen on the next preparation.
	std::vector<std::pair<uint64_t, uint64_t>> written;
	table.TakeWrittenRanges(written);
	for (auto& heap: table.Heaps()) {
		const auto start = heap.base + heap.table_offset;
		const auto end = start + uint64_t {heap.guest_entries} * 32;
		for (const auto& [first, last]: written) {
			const auto begin = std::max(first, start), finish = std::min(last, end);
			if (begin >= finish) continue;
			for (uint32_t key = static_cast<uint32_t>((begin - start) / 32);
			     key < (finish - start + 31) / 32; ++key) {
				if (heap.settled[key]) (void)table.ReleaseKey(heap, key);
			}
		}
	}
	if (table.SnapshotReady(scheduler)) {
		std::vector<uint32_t> requests;
		uint32_t budget = 256;
		for (auto& heap: table.Heaps()) {
			requests.clear();
			table.TakeRequests(heap, requests);
			for (const auto key: requests) {
				if (budget == 0) break; // unresolved keys request themselves again
				if (key >= heap.guest_entries || heap.settled[key]) continue;
				--budget;
				(void)ResolveBindlessKey(heap, key);
			}
		}
		table.ConsumeSnapshot();
	}
	for (const auto& use: runtime.resources.bindless_heaps) {
		const auto bytes = use.size > use.table_offset ? use.size - use.table_offset : 0;
		const auto mapped = LibKernel::Memory::ClampRangeSize(use.base + use.table_offset,
		                                                      std::min<uint64_t>(bytes, 32u << 18));
		const auto entries = static_cast<uint32_t>(mapped / 32);
		if (entries == 0 || !resources.IsMapped(use.base + use.table_offset, uint64_t {entries} * 32)) continue;
		const auto& resource = runtime.program->info.images.at(use.image);
		auto* heap = table.FindOrCreateHeap(use.base, use.table_offset, BindingFor(resource), entries, resource);
		if (heap == nullptr) continue;
		if (table.NeedsWatch(*heap)) table.WatchHeap(*heap);
		prepared.bindless_patches.push_back({use.mapping_offset, heap->region, std::min(entries, heap->entries)});
		if (std::ranges::find(prepared.bindless_heaps, heap) == prepared.bindless_heaps.end()) {
			prepared.bindless_heaps.push_back(heap);
			// Refresh existing images through our cache; Frank's cache and dirty-state shortcuts differ.
			const auto ids = heap->resolved;
			for (const auto id: ids) {
				auto& cache = m_context.GetTextureCache();
				auto* image = cache.m_slot_images.try_get(id);
				if (image == nullptr || !image->registered || image->binding.needs_rebind) {
					table.OnImageUnregistered(id);
					continue;
				}
				BindImage(id, false);
				image->binding.bindless_general = true;
				image->binding.force_general = true;
				std::scoped_lock lock {cache.m_lock};
				cache.TouchImage(*image);
				cache.RefreshImage(id);
			}
		}
	}
	if (!table.SnapshotRecorded()) table.RecordFeedbackSnapshot(scheduler);
}
} // namespace Libs::Graphics
