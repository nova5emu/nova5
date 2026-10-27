#pragma once
#include "common/commandRecorder.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include <type_traits>
namespace Libs::Graphics {
class CommandBuffer;
namespace VkRecording { class Encoder; }
// Stable command facade, not a stored Vulkan handle. Cached values continue to
// address the scheduler's CURRENT command after stream wraps or gate changes.
// Raw construction is for independently owned pools and existing test fixtures.
class NativeCommands {
public:
	using Drain = Common::CommandRecording::Drain;
	NativeCommands(vk::CommandBuffer native): m_native(native) {}
	explicit NativeCommands(const CommandBuffer& owner): m_owner(&owner) {}
	template <typename F> void Synchronous(F&& function, Drain reason = Drain::Native, int rendering = 0) const {
		using Callable = std::remove_reference_t<F>;
		Call([](void* data, vk::CommandBuffer native) { (*static_cast<Callable*>(data))(native); },
		     &function, reason, rendering);
	}
	void pipelineBarrier(vk::PipelineStageFlags source, vk::PipelineStageFlags destination, vk::DependencyFlags flags,
	    uint32_t memories, const vk::MemoryBarrier* memory, uint32_t buffers, const vk::BufferMemoryBarrier* buffer,
	    uint32_t images, const vk::ImageMemoryBarrier* image) const;
	void pipelineBarrier(vk::PipelineStageFlags source, vk::PipelineStageFlags destination, vk::DependencyFlags flags,
	    vk::ArrayProxy<const vk::MemoryBarrier> memory, vk::ArrayProxy<const vk::BufferMemoryBarrier> buffers,
	    vk::ArrayProxy<const vk::ImageMemoryBarrier> images) const {
		pipelineBarrier(source, destination, flags, memory.size(), memory.data(), buffers.size(), buffers.data(), images.size(), images.data());
	}
	void pipelineBarrier2(const vk::DependencyInfo* info) const;
	void pipelineBarrier2(const vk::DependencyInfo& info) const { pipelineBarrier2(&info); }
	void beginRendering(const vk::RenderingInfo* info) const;
	void beginRendering(const vk::RenderingInfo& info) const { beginRendering(&info); }
	void endRendering() const;
	void fillBuffer(vk::Buffer buffer, uint64_t offset, uint64_t size, uint32_t value) const;
	void bindPipeline(vk::PipelineBindPoint point, vk::Pipeline pipeline) const;
	void dispatch(uint32_t x, uint32_t y, uint32_t z) const;
	void draw(uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) const;
	void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, uint32_t offset, uint32_t size, const void* data) const;
	void pushDescriptorSetKHR(vk::PipelineBindPoint point, vk::PipelineLayout layout, uint32_t set, uint32_t count, const vk::WriteDescriptorSet* writes) const;
	void pushDescriptorSetKHR(vk::PipelineBindPoint point, vk::PipelineLayout layout, uint32_t set, vk::ArrayProxy<const vk::WriteDescriptorSet> writes) const {
		pushDescriptorSetKHR(point, layout, set, writes.size(), writes.data());
	}
	void bindDescriptorSets(vk::PipelineBindPoint point, vk::PipelineLayout layout, uint32_t first,
	    uint32_t count, const vk::DescriptorSet* sets, uint32_t offsets, const uint32_t* dynamic) const;
	void bindDescriptorSets(vk::PipelineBindPoint point, vk::PipelineLayout layout, uint32_t first,
	    vk::ArrayProxy<const vk::DescriptorSet> sets, vk::ArrayProxy<const uint32_t> offsets) const {
		bindDescriptorSets(point, layout, first, sets.size(), sets.data(), offsets.size(), offsets.data());
	}
	void UpdateDescriptors(vk::Device device, uint32_t count, const vk::WriteDescriptorSet* writes) const;
	void beginConditionalRenderingEXT(const vk::ConditionalRenderingBeginInfoEXT* info) const;
	void beginConditionalRenderingEXT(const vk::ConditionalRenderingBeginInfoEXT& info) const { beginConditionalRenderingEXT(&info); }
	void endConditionalRenderingEXT() const;
	void clearAttachments(uint32_t count, const vk::ClearAttachment* attachments, uint32_t rects, const vk::ClearRect* rectangles) const;
	void copyBuffer(vk::Buffer source, vk::Buffer destination, uint32_t count, const vk::BufferCopy* data) const;
	void copyBuffer(vk::Buffer source, vk::Buffer destination, vk::ArrayProxy<const vk::BufferCopy> data) const { copyBuffer(source, destination, data.size(), data.data()); }
	void copyBufferToImage(vk::Buffer source, vk::Image destination, vk::ImageLayout layout, uint32_t count, const vk::BufferImageCopy* data) const;
	void copyBufferToImage(vk::Buffer source, vk::Image destination, vk::ImageLayout layout, vk::ArrayProxy<const vk::BufferImageCopy> data) const { copyBufferToImage(source, destination, layout, data.size(), data.data()); }
	void copyImageToBuffer(vk::Image source, vk::ImageLayout layout, vk::Buffer destination, uint32_t count, const vk::BufferImageCopy* data) const;
	void copyImageToBuffer(vk::Image source, vk::ImageLayout layout, vk::Buffer destination, vk::ArrayProxy<const vk::BufferImageCopy> data) const { copyImageToBuffer(source, layout, destination, data.size(), data.data()); }
	void copyImage(vk::Image source, vk::ImageLayout from, vk::Image destination, vk::ImageLayout to, uint32_t count, const vk::ImageCopy* data) const;
	void copyImage(vk::Image source, vk::ImageLayout from, vk::Image destination, vk::ImageLayout to, vk::ArrayProxy<const vk::ImageCopy> data) const { copyImage(source, from, destination, to, data.size(), data.data()); }
	void resolveImage(vk::Image source, vk::ImageLayout from, vk::Image destination, vk::ImageLayout to, uint32_t count, const vk::ImageResolve* data) const;
	void resolveImage(vk::Image source, vk::ImageLayout from, vk::Image destination, vk::ImageLayout to, vk::ArrayProxy<const vk::ImageResolve> data) const { resolveImage(source, from, destination, to, data.size(), data.data()); }
	void blitImage(vk::Image source, vk::ImageLayout from, vk::Image destination, vk::ImageLayout to, uint32_t count, const vk::ImageBlit* data, vk::Filter filter) const;
	void blitImage(vk::Image source, vk::ImageLayout from, vk::Image destination, vk::ImageLayout to, vk::ArrayProxy<const vk::ImageBlit> data, vk::Filter filter) const { blitImage(source, from, destination, to, data.size(), data.data(), filter); }
	void clearColorImage(vk::Image image, vk::ImageLayout layout, const vk::ClearColorValue* clear, uint32_t count, const vk::ImageSubresourceRange* data) const;
	void clearDepthStencilImage(vk::Image image, vk::ImageLayout layout, const vk::ClearDepthStencilValue* clear, uint32_t count, const vk::ImageSubresourceRange* data) const;
	void setViewport(uint32_t first, uint32_t count, const vk::Viewport* data) const;
	void setViewport(uint32_t first, vk::ArrayProxy<const vk::Viewport> data) const { setViewport(first, data.size(), data.data()); }
	void setScissor(uint32_t first, uint32_t count, const vk::Rect2D* data) const;
	void setScissor(uint32_t first, vk::ArrayProxy<const vk::Rect2D> data) const { setScissor(first, data.size(), data.data()); }
private:
	VkRecording::Encoder* Recording() const;
	vk::CommandBuffer Direct() const;
	void Call(void (*function)(void*, vk::CommandBuffer), void* data, Drain reason, int rendering) const;
	const CommandBuffer* m_owner = nullptr;
	vk::CommandBuffer m_native {};
};
} // namespace Libs::Graphics

