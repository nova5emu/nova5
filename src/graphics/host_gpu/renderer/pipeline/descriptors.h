#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/drawPrep/bindingPlan.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace Libs::Graphics {

// 1: reuse PreparedBindings storage across operations (fresh contents each time).
extern "C" volatile std::atomic_uint32_t kyty_local_binding_scratch_mode;
// 0 off, 1 consume pure serial plans, 2 compare/use serial, 3 compare/exit on mismatch.
extern "C" volatile std::atomic_uint32_t kyty_local_draw_prep_bindings_mode;
// 1: table dispatches (src/local/table-xpr.inc, KYTY_TABLE_DISPATCH).
extern "C" volatile std::atomic_uint32_t kyty_local_table_dispatch_mode;

struct ShaderStageRuntime;

struct TextureBinding {
	ImageId                    image_id;
	vk::ImageView              image_view = nullptr;
	TextureCache::ImageDesc    desc;
	vk::ImageLayout            layout = vk::ImageLayout::eUndefined;
	std::vector<vk::ImageView> mip_views;
	uint64_t                  memo_tag = 0;
	uint32_t                  memo_slot = 0;
};

struct PreparedBindings {
	struct BufferSource {
		uint64_t address = 0;
		uint64_t size    = 0;
		BufferId id;
	};

	// The draw owns the immutable compiled-program/runtime-snapshot association through commit.
	const ShaderStageRuntime* runtime = nullptr;
	std::vector<std::array<uint32_t, 3>> bindless_patches;
	std::vector<BindlessTable::Heap*> bindless_heaps;
	// Rebuilt for this invocation only; runtime's snapshot remains immutable until commit.
	DrawPrep::StageBindingPlan binding_plan;
	uint32_t binding_plan_mode = 0;
	// Keep the resolved guest range through cache preparation; only the host buffer ID may
	// become stale and need resolving again when bindings are rebound.
	std::vector<BufferSource>             buffer_sources;
	std::vector<vk::DescriptorBufferInfo> buffers;
	std::vector<TextureBinding>           images;
	std::vector<vk::Sampler>              samplers;
	vk::DescriptorBufferInfo              gds {nullptr, 0, VK_WHOLE_SIZE};
	vk::DescriptorBufferInfo              flattened_srt;
	vk::DescriptorBufferInfo              shader_data_buffer;
	std::vector<uint32_t>                 shader_data;
};

[[nodiscard]] vk::DescriptorType
NativeDescriptorType(ShaderRecompiler::IR::DescriptorBindingKind kind);
[[nodiscard]] uint32_t
NativeDescriptorCount(const ShaderRecompiler::IR::DescriptorBinding& binding);
[[nodiscard]] vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture,
                                                    uint32_t              element = 0);
// Set 0 only: resident images retain their resource indices but occupy no slots.
[[nodiscard]] uint32_t Set0ImageDescriptorCount(const PreparedBindings& prepared, size_t resource);
[[nodiscard]] bool ImageDescriptorCountsMatch(const PreparedBindings& prepared,
                                              std::span<const uint32_t> occurrences);
// CPU payload assembly shared with the direct commit regression (no Vulkan calls).
void AppendImageDescriptorInfos(const PreparedBindings& prepared,
                                const ShaderRecompiler::IR::DescriptorBinding& binding,
                                std::vector<uint32_t>& occurrences,
                                std::vector<vk::DescriptorImageInfo>& infos);
#ifdef KYTY_LOCAL_VULKAN_RECORDING
// CPU-only regression of the local packet planner; creates no Vulkan objects.
[[nodiscard]] bool TestLocalDescriptorPlanBindless();
#endif

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
[[nodiscard]] bool
IsSupportedSampledVideoOutView(const ShaderRecompiler::IR::ImageResource& resource,
                               const ShaderTextureResource& descriptor, const Image& image);
[[nodiscard]] std::string StorageTextureReport(const ShaderRecompiler::IR::ImageResource& resource,
                                               const ShaderTextureResource& descriptor, uint64_t size);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
