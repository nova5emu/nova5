#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

struct ShaderStageRuntime;

struct TextureBinding {
	ImageId                    image_id;
	vk::ImageView              image_view = nullptr;
	TextureCache::ImageDesc    desc;
	vk::ImageLayout            layout = vk::ImageLayout::eUndefined;
	std::vector<vk::ImageView> mip_views;
};

// Descriptor-derived data only, captured BEFORE FindImage adjusts the view.
// No ImageId, residency, memory contents or mutable image state is retained.
struct BuiltImageDescription {
	TextureCache::ImageDesc desc;
	bool shader_conversion = false;
	vk::Format pixel_format = vk::Format::eUndefined;
	vk::Format view_format = vk::Format::eUndefined;
};

// CPU-only construction for non-null, non-heap descriptors (also used by tests).
[[nodiscard]] BuiltImageDescription BuildTextureDescription(
    const ShaderRecompiler::IR::ImageResource& resource, const ShaderTextureResource& descriptor);

class ImageDescriptionCache {
public:
	static constexpr size_t MaxEntries = 4096;
	using Words = std::array<uint32_t, 8>;
	[[nodiscard]] const BuiltImageDescription* Find(uint64_t program, uint32_t resource,
	                                               const Words& words) const {
		const auto it = m_last.find({program, resource});
		return it != m_last.end() && it->second.words == words ? &it->second.built : nullptr;
	}
	// One last descriptor per immutable program/resource, bounded per executor.
	// Returns true only when a new pair exhausted the bound.
	bool Store(uint64_t program, uint32_t resource, const Words& words,
	           const BuiltImageDescription& built) {
		const Key key {program, resource};
		if (auto it = m_last.find(key); it != m_last.end()) {
			it->second = {words, built};
			return false;
		}
		const bool reset = m_last.size() == MaxEntries;
		if (reset) Clear();
		m_last.emplace(key, Entry {words, built});
		return reset;
	}
	void Clear() { m_last.clear(); }
	[[nodiscard]] size_t Size() const { return m_last.size(); }
private:
	struct Key {
		uint64_t program;
		uint32_t resource;
		bool operator==(const Key&) const = default;
	};
	struct Hash {
		size_t operator()(const Key& key) const {
			return static_cast<size_t>((key.program * 0x9e3779b97f4a7c15ull) ^ key.resource);
		}
	};
	struct Entry { Words words; BuiltImageDescription built; };
	std::unordered_map<Key, Entry, Hash> m_last;
};

struct PreparedBindings {
	struct BufferSource {
		uint64_t address = 0;
		uint64_t size    = 0;
		BufferId id;
	};

	// The draw owns the immutable compiled-program/runtime-snapshot association through commit.
	const ShaderStageRuntime* runtime = nullptr;
	// Keep the resolved guest range through cache preparation; only the host buffer ID may
	// become stale and need resolving again when bindings are rebound.
	std::vector<BufferSource>             buffer_sources;
	std::vector<vk::DescriptorBufferInfo> buffers;
	std::vector<TextureBinding>           images;
	std::vector<vk::Sampler>              samplers;
	vk::DescriptorBufferInfo              gds {nullptr, 0, VK_WHOLE_SIZE};
	vk::DescriptorBufferInfo              flattened_srt;
	vk::DescriptorBufferInfo              shader_data_buffer;
	vk::DescriptorBufferInfo              shared_memory;
	std::vector<uint32_t>                 shader_data;
};

// Scratch ranges rebuilt at each commit, after binding discovery. Only capacity
// survives between commits: guest memory and image ownership are never cached.
class BindingOverlapWrites {
public:
	enum class Hit { None, Image, Buffer };

	void Clear() { m_images.clear(); m_buffers.clear(); }
	[[nodiscard]] size_t Size() const { return m_images.size() + m_buffers.size(); }

	void AddImage(const ImageInfo& info, bool shader_write, bool is_target) {
		if (!shader_write && !is_target) return;
		for (const auto range: {info.data, info.stencil, info.metadata.range}) {
			if (range.size != 0) m_images.push_back(range);
		}
	}
	void AddBuffer(const ShaderRecompiler::IR::BufferResource& resource,
	               std::span<const PreparedBindings::BufferSource> sources) {
		if (!resource.written || resource.descriptor_index == UINT32_MAX) return;
		const auto& source = sources[resource.descriptor_index];
		// ADD_TID and swizzle can write beyond the fixed-offset extent.
		const bool linear = (resource.packed_stride & ((1u << 14u) | (1u << 20u))) == 0u;
		const auto size = linear ? std::min<uint64_t>(source.size, resource.write_extent) : source.size;
		if (size != 0) m_buffers.push_back({source.address, size});
	}
	[[nodiscard]] Hit CheckRead(uint64_t address, uint64_t size, bool count,
	                            uint64_t& image_comparisons, uint64_t& buffer_comparisons) const {
		// Preserve the old order (images before buffers) and its range validation.
		for (const auto range: m_images) {
			if (count) ++image_comparisons;
			if (ImageRangeOverlaps(address, size, range.address, range.size)) return Hit::Image;
		}
		for (const auto range: m_buffers) {
			if (count) ++buffer_comparisons;
			if (ImageRangeOverlaps(address, size, range.address, range.size)) return Hit::Buffer;
		}
		return Hit::None;
	}

private:
	std::vector<GuestRange> m_images;
	std::vector<GuestRange> m_buffers;
};

[[nodiscard]] vk::DescriptorType
NativeDescriptorType(ShaderRecompiler::IR::DescriptorBindingKind kind);
[[nodiscard]] vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture,
                                                    uint32_t              element = 0);

template <typename T>
[[nodiscard]] T DecodeNativeDescriptor(const ShaderRecompiler::IR::DescriptorValue& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	static_assert(sizeof(T) % sizeof(uint32_t) == 0);
	T result {};
	EXIT_IF(value.dword_count < sizeof(result) / sizeof(uint32_t));
	std::memcpy(&result, value.dwords.data(), sizeof(result));
	return result;
}

[[nodiscard]] bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor,
                                                   bool r128 = false);
void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
