#include "graphics/host_gpu/renderer/nativeCommands.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/vkRecorderBackend.h"

#include <algorithm>

namespace Libs::Graphics {
using namespace VkRecording;
Encoder* NativeCommands::Recording() const {
	return m_owner && m_owner->m_recorder ? &m_owner->m_recorder->Commands() : nullptr;
}
vk::CommandBuffer NativeCommands::Direct() const { return m_owner ? m_owner->Handle() : m_native; }
void NativeCommands::Call(void (*function)(void*, vk::CommandBuffer), void* data, Drain reason, int rendering) const {
	if (m_owner && m_owner->m_recorder) m_owner->m_recorder->Call(function, data, reason, rendering);
	else function(data, Direct());
}
// Normal direct emission does not use Call/Synchronous: that type-erased
// callback is only needed by unsupported recorded packets and explicit islands.
// Keep identical native arguments and the stable facade across stream wraps.
void NativeCommands::copyBuffer(vk::Buffer source, vk::Buffer destination, uint32_t count, const vk::BufferCopy* data) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.source_buffer = source; p.destination_buffer = destination;
		if (encoder->TransferArray(Op::CopyBuffer, p, std::span(data, count))) return;
	} else { Direct().copyBuffer(source, destination, count, data); return; }
	Synchronous([&](vk::CommandBuffer native) { native.copyBuffer(source, destination, count, data); }, Drain::Upload);
}
void NativeCommands::copyBufferToImage(vk::Buffer source, vk::Image destination, vk::ImageLayout layout, uint32_t count, const vk::BufferImageCopy* data) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.source_buffer = source; p.destination_image = destination; p.destination_layout = layout;
		if (encoder->TransferArray(Op::BufferToImage, p, std::span(data, count))) return;
	} else { Direct().copyBufferToImage(source, destination, layout, count, data); return; }
	Synchronous([&](vk::CommandBuffer native) { native.copyBufferToImage(source, destination, layout, count, data); }, Drain::Upload);
}
void NativeCommands::copyImageToBuffer(vk::Image source, vk::ImageLayout layout, vk::Buffer destination, uint32_t count, const vk::BufferImageCopy* data) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.source_image = source; p.source_layout = layout; p.destination_buffer = destination;
		if (encoder->TransferArray(Op::ImageToBuffer, p, std::span(data, count))) return;
	} else { Direct().copyImageToBuffer(source, layout, destination, count, data); return; }
	Synchronous([&](vk::CommandBuffer native) { native.copyImageToBuffer(source, layout, destination, count, data); }, Drain::Upload);
}
void NativeCommands::copyImage(vk::Image source, vk::ImageLayout from, vk::Image destination, vk::ImageLayout to, uint32_t count, const vk::ImageCopy* data) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.source_image = source; p.source_layout = from; p.destination_image = destination; p.destination_layout = to;
		if (encoder->TransferArray(Op::CopyImage, p, std::span(data, count))) return;
	} else { Direct().copyImage(source, from, destination, to, count, data); return; }
	Synchronous([&](vk::CommandBuffer native) { native.copyImage(source, from, destination, to, count, data); }, Drain::Upload);
}
void NativeCommands::resolveImage(vk::Image source, vk::ImageLayout from, vk::Image destination, vk::ImageLayout to, uint32_t count, const vk::ImageResolve* data) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.source_image = source; p.source_layout = from; p.destination_image = destination; p.destination_layout = to;
		if (encoder->TransferArray(Op::ResolveImage, p, std::span(data, count))) return;
	} else { Direct().resolveImage(source, from, destination, to, count, data); return; }
	Synchronous([&](vk::CommandBuffer native) { native.resolveImage(source, from, destination, to, count, data); }, Drain::Upload);
}
void NativeCommands::blitImage(vk::Image source, vk::ImageLayout from, vk::Image destination, vk::ImageLayout to, uint32_t count, const vk::ImageBlit* data, vk::Filter filter) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.source_image = source; p.source_layout = from; p.destination_image = destination; p.destination_layout = to; p.filter = filter;
		if (encoder->TransferArray(Op::BlitImage, p, std::span(data, count))) return;
	} else { Direct().blitImage(source, from, destination, to, count, data, filter); return; }
	Synchronous([&](vk::CommandBuffer native) { native.blitImage(source, from, destination, to, count, data, filter); }, Drain::Upload);
}
void NativeCommands::clearColorImage(vk::Image image, vk::ImageLayout layout, const vk::ClearColorValue* clear, uint32_t count, const vk::ImageSubresourceRange* data) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.destination_image = image; p.destination_layout = layout; p.clear.color = *clear;
		if (encoder->TransferArray(Op::ClearColor, p, std::span(data, count))) return;
	} else { Direct().clearColorImage(image, layout, clear, count, data); return; }
	Synchronous([&](vk::CommandBuffer native) { native.clearColorImage(image, layout, clear, count, data); }, Drain::Upload);
}
void NativeCommands::clearDepthStencilImage(vk::Image image, vk::ImageLayout layout, const vk::ClearDepthStencilValue* clear, uint32_t count, const vk::ImageSubresourceRange* data) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.destination_image = image; p.destination_layout = layout; p.clear.depthStencil = *clear;
		if (encoder->TransferArray(Op::ClearDepth, p, std::span(data, count))) return;
	} else { Direct().clearDepthStencilImage(image, layout, clear, count, data); return; }
	Synchronous([&](vk::CommandBuffer native) { native.clearDepthStencilImage(image, layout, clear, count, data); }, Drain::Upload);
}
void NativeCommands::setViewport(uint32_t first, uint32_t count, const vk::Viewport* data) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.first = first;
		if (encoder->TransferArray(Op::Viewport, p, std::span(data, count))) return;
	} else { Direct().setViewport(first, count, data); return; }
	Synchronous([&](vk::CommandBuffer native) { native.setViewport(first, count, data); }, Drain::Native);
}
void NativeCommands::setScissor(uint32_t first, uint32_t count, const vk::Rect2D* data) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.first = first;
		if (encoder->TransferArray(Op::Scissor, p, std::span(data, count))) return;
	} else { Direct().setScissor(first, count, data); return; }
	Synchronous([&](vk::CommandBuffer native) { native.setScissor(first, count, data); }, Drain::Native);
}
void NativeCommands::pipelineBarrier(vk::PipelineStageFlags source, vk::PipelineStageFlags destination, vk::DependencyFlags flags,
    uint32_t memories, const vk::MemoryBarrier* memory, uint32_t buffers, const vk::BufferMemoryBarrier* buffer,
    uint32_t images, const vk::ImageMemoryBarrier* image) const {
	if (auto* encoder = Recording()) {
		if (encoder->LegacyBarriers(source, destination, flags, {memory, memories}, {buffer, buffers}, {image, images})) return;
	} else { Direct().pipelineBarrier(source, destination, flags, memories, memory, buffers, buffer, images, image); return; }
	Synchronous([&](vk::CommandBuffer native) {
		native.pipelineBarrier(source, destination, flags, memories, memory, buffers, buffer, images, image);
	});
}
void NativeCommands::pipelineBarrier2(const vk::DependencyInfo* info) const {
	if (auto* encoder = Recording()) { if (encoder->Barriers(*info)) return; } else { Direct().pipelineBarrier2(info); return; }
	Synchronous([&](vk::CommandBuffer native) { native.pipelineBarrier2(info); });
}
void NativeCommands::beginRendering(const vk::RenderingInfo* info) const {
	if (auto* encoder = Recording()) { if (encoder->NativeRendering(*info)) return; } else { Direct().beginRendering(info); return; }
	Synchronous([&](vk::CommandBuffer native) { native.beginRendering(info); }, Drain::Native, 1);
}
void NativeCommands::endRendering() const {
	if (auto* encoder = Recording()) { if (encoder->EndRendering()) return; } else { Direct().endRendering(); return; }
	Synchronous([](vk::CommandBuffer native) { native.endRendering(); }, Drain::Native, -1);
}
void NativeCommands::fillBuffer(vk::Buffer buffer, uint64_t offset, uint64_t size, uint32_t value) const {
	if (auto* encoder = Recording()) {
		Transfer p {}; p.destination_buffer = buffer; p.offset = offset; p.size = size; p.value = value;
		if (encoder->Value(Op::FillBuffer, p)) return;
	} else { Direct().fillBuffer(buffer, offset, size, value); return; }
	Synchronous([&](vk::CommandBuffer native) { native.fillBuffer(buffer, offset, size, value); }, Drain::Upload);
}
void NativeCommands::bindPipeline(vk::PipelineBindPoint point, vk::Pipeline pipeline) const {
	if (auto* encoder = Recording()) { if (encoder->Value(Op::BindPipeline, Pipeline {point, pipeline})) return; } else { Direct().bindPipeline(point, pipeline); return; }
	Synchronous([&](vk::CommandBuffer native) { native.bindPipeline(point, pipeline); });
}
void NativeCommands::dispatch(uint32_t x, uint32_t y, uint32_t z) const {
	if (auto* encoder = Recording()) { if (encoder->Value(Op::Dispatch, Dispatch {x, y, z})) return; } else { Direct().dispatch(x, y, z); return; }
	Synchronous([&](vk::CommandBuffer native) { native.dispatch(x, y, z); });
}
void NativeCommands::draw(uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) const {
	if (auto* encoder = Recording()) { if (encoder->Value(Op::Draw, Draw {count, instances, first, first_instance})) return; } else { Direct().draw(count, instances, first, first_instance); return; }
	Synchronous([&](vk::CommandBuffer native) { native.draw(count, instances, first, first_instance); });
}
void NativeCommands::pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, uint32_t offset, uint32_t size, const void* data) const {
	if (auto* encoder = Recording()) {
		if (encoder->PushConstants(layout, stages, offset, {static_cast<const std::byte*>(data), size})) return;
	} else { Direct().pushConstants(layout, stages, offset, size, data); return; }
	Synchronous([&](vk::CommandBuffer native) { native.pushConstants(layout, stages, offset, size, data); });
}
void NativeCommands::pushDescriptorSetKHR(vk::PipelineBindPoint point, vk::PipelineLayout layout, uint32_t set, uint32_t count, const vk::WriteDescriptorSet* writes) const {
	if (auto* encoder = Recording()) { if (encoder->Descriptors(true, point, layout, set, {writes, count})) return; } else { Direct().pushDescriptorSetKHR(point, layout, set, count, writes); return; }
	Synchronous([&](vk::CommandBuffer native) { native.pushDescriptorSetKHR(point, layout, set, count, writes); });
}
void NativeCommands::bindDescriptorSets(vk::PipelineBindPoint point, vk::PipelineLayout layout, uint32_t first,
    uint32_t count, const vk::DescriptorSet* sets, uint32_t offsets, const uint32_t* dynamic) const {
	if (auto* encoder = Recording()) { if (encoder->BindDescriptors(point, layout, first, {sets, count}, {dynamic, offsets})) return; } else { Direct().bindDescriptorSets(point, layout, first, count, sets, offsets, dynamic); return; }
	Synchronous([&](vk::CommandBuffer native) { native.bindDescriptorSets(point, layout, first, count, sets, offsets, dynamic); });
}
void NativeCommands::UpdateDescriptors(vk::Device device, uint32_t count, const vk::WriteDescriptorSet* writes) const {
	if (!Recording()) { device.updateDescriptorSets(count, writes, 0, nullptr); return; }
	while (count) {
		const auto chunk = std::min(count, 64u);
		if (!Recording()->Descriptors(false, vk::PipelineBindPoint::eGraphics, {}, 0, {writes, chunk})) {
			Synchronous([&](vk::CommandBuffer) { device.updateDescriptorSets(chunk, writes, 0, nullptr); });
		}
		count -= chunk; writes += chunk;
	}
}
void NativeCommands::beginConditionalRenderingEXT(const vk::ConditionalRenderingBeginInfoEXT* info) const {
	if (auto* encoder = Recording()) { if (encoder->ConditionalBegin(*info)) return; } else { Direct().beginConditionalRenderingEXT(info); return; }
	Synchronous([&](vk::CommandBuffer native) { native.beginConditionalRenderingEXT(info); });
}
void NativeCommands::endConditionalRenderingEXT() const {
	if (auto* encoder = Recording()) { if (encoder->ConditionalEnd()) return; } else { Direct().endConditionalRenderingEXT(); return; }
	Synchronous([](vk::CommandBuffer native) { native.endConditionalRenderingEXT(); });
}
void NativeCommands::clearAttachments(uint32_t count, const vk::ClearAttachment* attachments, uint32_t rects, const vk::ClearRect* rectangles) const {
	if (auto* encoder = Recording()) { if (encoder->ClearAttachments({attachments, count}, {rectangles, rects})) return; } else { Direct().clearAttachments(count, attachments, rects, rectangles); return; }
	Synchronous([&](vk::CommandBuffer native) { native.clearAttachments(count, attachments, rects, rectangles); });
}
} // namespace Libs::Graphics

