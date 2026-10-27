#include "graphics/host_gpu/renderer/bindingEmission.h"

#include "common/assert.h"
#include "graphics/host_gpu/renderer/vkRecorderPackets.h"

namespace Libs::Graphics::VkRecording {
void BindingEmission::Emit(vk::Device device, vk::CommandBuffer command) const {
	EXIT_NOT_IMPLEMENTED(constants.size() != state.constant_bytes);
	if (state.heap_set) {
		command.bindDescriptorSets(state.point, state.layout, state.heap_index, 1, &state.heap_set, 0, nullptr);
	}
	if (!constants.empty()) {
		command.pushConstants(state.layout, state.stages, state.constant_offset,
		                      state.constant_bytes, constants.data());
	}
	if (writes.empty()) return;
	const auto count = static_cast<uint32_t>(writes.size());
	if (state.push_descriptors) {
		command.pushDescriptorSetKHR(state.point, state.layout, 0, count, writes.data());
	} else {
		EXIT_NOT_IMPLEMENTED(!state.set);
		device.updateDescriptorSets(count, writes.data(), 0, nullptr);
		command.bindDescriptorSets(state.point, state.layout, 0, 1, &state.set, 0, nullptr);
	}
}

bool BindingEmission::Enqueue(Encoder& encoder) const {
	return encoder.Bindings(state, constants, writes);
}
} // namespace Libs::Graphics::VkRecording
