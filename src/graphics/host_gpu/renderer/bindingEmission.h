#ifndef NOVA5_GRAPHICS_BINDING_EMISSION_H
#define NOVA5_GRAPHICS_BINDING_EMISSION_H

#include "graphics/host_gpu/vulkanCommon.h"
#include <cstddef>

namespace Libs::Graphics::VkRecording {
class Encoder;

// Native values only. No executor, cache, guest addresses or scheduler access.
struct BindingState {
	vk::PipelineLayout layout {};
	vk::PipelineBindPoint point {};
	vk::DescriptorSet heap_set {}, set {};
	uint32_t heap_index = 0;
	vk::ShaderStageFlags stages {};
	uint32_t constant_offset = 0, constant_bytes = 0;
	bool push_descriptors = false;
};

// Producer-side view: valid only until the next binding preparation or mutation
// of PreparedBindings. Consume synchronously with Emit or Enqueue. Enqueue copies
// the ENTIRE emission atomically into one packet; the worker never retains this
// view or any of its nested descriptor pointers. It does not pin Vulkan objects.
struct BindingEmission {
	BindingState state;
	std::span<const std::byte> constants;
	std::span<const vk::WriteDescriptorSet> writes;
	void Emit(vk::Device device, vk::CommandBuffer command) const;
	[[nodiscard]] bool Enqueue(Encoder& encoder) const;
};
} // namespace Libs::Graphics::VkRecording
#endif
