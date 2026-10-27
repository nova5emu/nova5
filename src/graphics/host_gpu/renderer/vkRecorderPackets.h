#ifndef NOVA5_GRAPHICS_VK_RECORDER_PACKETS_H
#define NOVA5_GRAPHICS_VK_RECORDER_PACKETS_H

#include "common/commandRecorder.h"
#include "graphics/host_gpu/renderer/bindingEmission.h"
#include "graphics/host_gpu/renderer/dynamicState.h"

#include <array>
#include <cstring>

namespace Libs::Graphics::VkRecording {

// Deep-copy contract for the worker-owned vkRecorderBackend. No raw command
// buffer is accepted here; producer-side cache/coherence stays in the scheduler.
enum class Op : uint16_t {
	BindPipeline, BindDescriptors, PushConstants, PushDescriptors, UpdateDescriptors,
	BeginRendering, EndRendering, DynamicState, Barrier2, Draw, DrawIndexed,
	DrawIndirect, DrawIndexedIndirect, DrawIndirectCount, DrawIndexedIndirectCount,
	DrawMesh, DrawMeshIndirect, BindIndex, BindVertices, Dispatch, DispatchIndirect, Bindings,
	ResetQueries, Timestamp, BindVerticesSized, DispatchBase, BeginQuery, EndQuery,
	LegacyBarrier, CopyBuffer, BufferToImage, ImageToBuffer, CopyImage, ResolveImage, BlitImage,
	FillBuffer, ClearColor, ClearDepth, NativeRendering, Viewport, Scissor,
	ConditionalBegin, ConditionalEnd, ClearAttachments
};
struct Pipeline { vk::PipelineBindPoint point {}; vk::Pipeline pipeline {}; };
struct DescriptorSets { vk::PipelineLayout layout {}; vk::PipelineBindPoint point {}; uint32_t first = 0, sets = 0, offsets = 0; };
struct Constants { vk::PipelineLayout layout {}; vk::ShaderStageFlags stages {}; uint32_t offset = 0, bytes = 0; };
struct Writes { vk::PipelineLayout layout {}; vk::PipelineBindPoint point {}; uint32_t set = 0, count = 0; };
struct Write { vk::DescriptorSet set {}; uint32_t binding = 0, element = 0, count = 0; vk::DescriptorType type {}; };
struct Barrier { vk::DependencyFlags flags {}; uint32_t memory = 0, buffers = 0, images = 0; };
struct Draw { uint32_t count = 0, instances = 0, first = 0, first_instance = 0; int32_t vertex_offset = 0; };
struct Indirect { vk::Buffer buffer {}, count_buffer {}; uint64_t offset = 0, count_offset = 0; uint32_t count = 0, stride = 0; };
struct Dispatch { uint32_t x = 0, y = 0, z = 0; };
struct Index { vk::Buffer buffer {}; uint64_t offset = 0; vk::IndexType type {}; uint64_t size = VK_WHOLE_SIZE; bool bounded = false; };
struct DispatchOffset { Dispatch base, groups; };
struct Query { vk::QueryPool pool {}; uint32_t query = 0; vk::QueryControlFlags flags {}; };
struct Vertices { uint32_t first = 0, count = 0; };
struct QueryReset { vk::QueryPool pool {}; uint32_t first = 0, count = 0; };
struct Timestamp { vk::PipelineStageFlagBits stage {}; vk::QueryPool pool {}; uint32_t query = 0; };
struct LegacyBarrier {
	vk::PipelineStageFlags source {}, destination {}; Barrier counts;
};
struct Transfer {
	vk::Buffer source_buffer {}, destination_buffer {};
	vk::Image source_image {}, destination_image {};
	vk::ImageLayout source_layout {}, destination_layout {};
	vk::Filter filter {}; uint32_t count = 0, first = 0, value = 0;
	uint64_t offset = 0, size = 0; vk::ClearValue clear {};
};
struct Rendering { vk::RenderingInfo info {}; bool depth = false, stencil = false; };
struct Conditional { vk::Buffer buffer {}; uint64_t offset = 0; vk::ConditionalRenderingFlagsEXT flags {}; };
struct Attachments { uint32_t count = 0, rectangles = 0; };

// Reader copies into consumer-owned, suitably aligned storage. Packet pointers
// are never retained past the callback; no assumptions about byte-array aliasing.
class Reader final {
public:
	explicit Reader(std::span<const std::byte> bytes): m_bytes(bytes) {}
	template <typename T> bool Read(T& value) { return ReadArray(std::span(&value, 1)); }
	template <typename T, size_t Extent> bool ReadArray(std::span<T, Extent> values) {
		static_assert(std::is_trivially_copyable_v<T>);
		if (values.size_bytes() > m_bytes.size()) return false;
		if (!values.empty()) std::memcpy(values.data(), m_bytes.data(), values.size_bytes());
		m_bytes = m_bytes.subspan(values.size_bytes());
		return true;
	}
	bool Empty() const { return m_bytes.empty(); }
private:
	std::span<const std::byte> m_bytes;
};

class Encoder final {
public:
	explicit Encoder(Common::CommandRecording::Queue& queue): m_queue(queue) {}
	bool ConditionalBegin(const vk::ConditionalRenderingBeginInfoEXT& info) {
		if (info.pNext) return false;
		return Value(Op::ConditionalBegin, Conditional {info.buffer, info.offset, info.flags});
	}
	bool ConditionalEnd() { return m_queue.Append(static_cast<uint16_t>(Op::ConditionalEnd), {}); }
	bool ClearAttachments(std::span<const vk::ClearAttachment> attachments, std::span<const vk::ClearRect> rectangles) {
		if (attachments.size() > 16 || rectangles.size() > 256) return false;
		const Attachments header {static_cast<uint32_t>(attachments.size()), static_cast<uint32_t>(rectangles.size())};
		const std::array parts {Bytes(header), std::as_bytes(attachments), std::as_bytes(rectangles)};
		return Parts(Op::ClearAttachments, parts);
	}
	template <typename T> bool TransferArray(Op op, Transfer header, std::span<const T> data) {
		static_assert(std::is_same_v<T, vk::BufferCopy> || std::is_same_v<T, vk::BufferImageCopy> ||
		    std::is_same_v<T, vk::ImageCopy> || std::is_same_v<T, vk::ImageResolve> ||
		    std::is_same_v<T, vk::ImageBlit> || std::is_same_v<T, vk::ImageSubresourceRange> ||
		    std::is_same_v<T, vk::Viewport> || std::is_same_v<T, vk::Rect2D>);
		if (data.size() > 512) return false;
		header.count = static_cast<uint32_t>(data.size());
		const std::array parts {Bytes(header), std::as_bytes(data)};
		return Parts(op, parts);
	}
	bool LegacyBarriers(vk::PipelineStageFlags source, vk::PipelineStageFlags destination,
	    vk::DependencyFlags flags, std::span<const vk::MemoryBarrier> memory,
	    std::span<const vk::BufferMemoryBarrier> buffers, std::span<const vk::ImageMemoryBarrier> images) {
		if (memory.size() > 256 || buffers.size() > 256 || images.size() > 256) return false;
		for (const auto& b: memory) if (b.pNext) return false;
		for (const auto& b: buffers) if (b.pNext) return false;
		for (const auto& b: images) if (b.pNext) return false;
		const LegacyBarrier header {source, destination, {flags, static_cast<uint32_t>(memory.size()),
		    static_cast<uint32_t>(buffers.size()), static_cast<uint32_t>(images.size())}};
		const std::array parts {Bytes(header), std::as_bytes(memory), std::as_bytes(buffers), std::as_bytes(images)};
		return Parts(Op::LegacyBarrier, parts);
	}
	bool NativeRendering(const vk::RenderingInfo& info) {
		if (info.pNext || info.colorAttachmentCount > 8 || (info.colorAttachmentCount && !info.pColorAttachments)) return false;
		std::span colors(info.pColorAttachments, info.colorAttachmentCount);
		for (const auto& a: colors) if (a.pNext) return false;
		if ((info.pDepthAttachment && info.pDepthAttachment->pNext) || (info.pStencilAttachment && info.pStencilAttachment->pNext)) return false;
		Rendering header {info, info.pDepthAttachment != nullptr, info.pStencilAttachment != nullptr};
		header.info.pColorAttachments = header.info.pDepthAttachment = header.info.pStencilAttachment = nullptr;
		const std::array parts {Bytes(header), std::as_bytes(colors),
		    std::as_bytes(std::span(info.pDepthAttachment, header.depth ? 1u : 0u)),
		    std::as_bytes(std::span(info.pStencilAttachment, header.stencil ? 1u : 0u))};
		return Parts(Op::NativeRendering, parts);
	}
	template <typename T> bool Value(Op op, const T& value) {
		// Do not let a convenient generic overload shallow-copy pointer-bearing
		// Vulkan SDK structs. Variable payloads must use the encoders below.
		static_assert(std::is_same_v<T, Pipeline> || std::is_same_v<T, Draw> ||
		    std::is_same_v<T, Indirect> || std::is_same_v<T, Dispatch> || std::is_same_v<T, Index> ||
		    std::is_same_v<T, QueryReset> || std::is_same_v<T, Timestamp> ||
		    std::is_same_v<T, DispatchOffset> || std::is_same_v<T, Query> ||
		    std::is_same_v<T, Transfer> || std::is_same_v<T, Conditional> ||
		    std::is_same_v<T, RenderState> || std::is_same_v<T, DynamicState>);
		return m_queue.AppendValue(static_cast<uint16_t>(op), value);
	}
	bool BeginRendering(const RenderState& state) { return Value(Op::BeginRendering, state); }
	bool EndRendering() { return m_queue.Append(static_cast<uint16_t>(Op::EndRendering), {}); }
	bool Dynamic(const DynamicState& state) { return Value(Op::DynamicState, state); }
	bool PushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, uint32_t offset, std::span<const std::byte> data) {
		if (data.size() > UINT32_MAX || data.size() % 4 || offset % 4) return false;
		const Constants header {layout, stages, offset, static_cast<uint32_t>(data.size())};
		const std::array parts {Bytes(header), data};
		return Parts(Op::PushConstants, parts);
	}
	bool BindDescriptors(vk::PipelineBindPoint point, vk::PipelineLayout layout, uint32_t first,
	                     std::span<const vk::DescriptorSet> sets, std::span<const uint32_t> offsets = {}) {
		if (sets.size() > UINT32_MAX || offsets.size() > UINT32_MAX) return false;
		const DescriptorSets header {layout, point, first, static_cast<uint32_t>(sets.size()), static_cast<uint32_t>(offsets.size())};
		const std::array parts {Bytes(header), std::as_bytes(sets), std::as_bytes(offsets)};
		return Parts(Op::BindDescriptors, parts);
	}
	bool BindVertices(uint32_t first, std::span<const vk::Buffer> buffers, std::span<const uint64_t> offsets) {
		if (buffers.size() != offsets.size() || buffers.size() > UINT32_MAX) return false;
		const Vertices header {first, static_cast<uint32_t>(buffers.size())};
		const std::array parts {Bytes(header), std::as_bytes(buffers), std::as_bytes(offsets)};
		return Parts(Op::BindVertices, parts);
	}
	bool BindVerticesSized(uint32_t first, std::span<const vk::Buffer> buffers,
	                       std::span<const uint64_t> offsets, std::span<const uint64_t> sizes) {
		if (buffers.size() != offsets.size() || buffers.size() != sizes.size() || buffers.size() > UINT32_MAX) return false;
		const Vertices header {first, static_cast<uint32_t>(buffers.size())};
		const std::array parts {Bytes(header), std::as_bytes(buffers), std::as_bytes(offsets), std::as_bytes(sizes)};
		return Parts(Op::BindVerticesSized, parts);
	}
	// pNext chains require an explicit future encoder. Returning false appends
	// NOTHING: the caller must drain before its synchronous fallback.
	bool Barriers(const vk::DependencyInfo& info) {
		if (info.pNext || (info.memoryBarrierCount && !info.pMemoryBarriers) ||
		    (info.bufferMemoryBarrierCount && !info.pBufferMemoryBarriers) ||
		    (info.imageMemoryBarrierCount && !info.pImageMemoryBarriers)) return false;
		const std::span memory(info.pMemoryBarriers, info.memoryBarrierCount);
		const std::span buffers(info.pBufferMemoryBarriers, info.bufferMemoryBarrierCount);
		const std::span images(info.pImageMemoryBarriers, info.imageMemoryBarrierCount);
		for (const auto& barrier: memory) if (barrier.pNext) return false;
		for (const auto& barrier: buffers) if (barrier.pNext) return false;
		for (const auto& barrier: images) if (barrier.pNext) return false;
		const Barrier header {info.dependencyFlags, info.memoryBarrierCount, info.bufferMemoryBarrierCount, info.imageMemoryBarrierCount};
		const std::array parts {Bytes(header), std::as_bytes(memory), std::as_bytes(buffers), std::as_bytes(images)};
		return Parts(Op::Barrier2, parts);
	}
	bool Descriptors(bool push, vk::PipelineBindPoint point, vk::PipelineLayout layout, uint32_t set,
	                 std::span<const vk::WriteDescriptorSet> writes) {
		const Writes header {layout, point, set, static_cast<uint32_t>(writes.size())};
		return DescriptorParts(push ? Op::PushDescriptors : Op::UpdateDescriptors, {}, header, writes);
	}
	bool Bindings(const BindingState& state, std::span<const std::byte> constants,
	              std::span<const vk::WriteDescriptorSet> writes) {
		if (constants.size() != state.constant_bytes || constants.size() % 4 || state.constant_offset % 4 ||
		    (!writes.empty() && !state.push_descriptors && !state.set)) return false;
		if (!state.push_descriptors) {
			for (const auto& write: writes) if (write.dstSet != state.set) return false;
		}
		const Writes header {state.layout, state.point, 0, static_cast<uint32_t>(writes.size())};
		const std::array prefix {Bytes(state), constants};
		return DescriptorParts(Op::Bindings, prefix, header, writes);
	}
private:
	bool DescriptorParts(Op op, std::span<const std::span<const std::byte>> prefix,
	                     const Writes& header, std::span<const vk::WriteDescriptorSet> writes) {
		constexpr size_t MaxWrites = 64;
		if (writes.size() > MaxWrites || prefix.size() > 2) return false;
		std::array<Write, MaxWrites> flat {};
		std::array<std::span<const std::byte>, 3 + 2 * MaxWrites> parts {};
		std::copy(prefix.begin(), prefix.end(), parts.begin());
		const auto first = prefix.size();
		parts[first] = Bytes(header);
		for (size_t i = 0; i < writes.size(); ++i) {
			const auto& w = writes[i];
			if (w.pNext || !w.descriptorCount) return false;
			flat[i] = {w.dstSet, w.dstBinding, w.dstArrayElement, w.descriptorCount, w.descriptorType};
			parts[first + 1 + 2 * i] = Bytes(flat[i]);
			auto& data = parts[first + 2 + 2 * i];
			switch (w.descriptorType) {
			case vk::DescriptorType::eUniformBuffer: case vk::DescriptorType::eStorageBuffer:
			case vk::DescriptorType::eUniformBufferDynamic: case vk::DescriptorType::eStorageBufferDynamic:
				if (!w.pBufferInfo) return false;
				data = std::as_bytes(std::span(w.pBufferInfo, w.descriptorCount)); break;
			case vk::DescriptorType::eSampler: case vk::DescriptorType::eCombinedImageSampler:
			case vk::DescriptorType::eSampledImage: case vk::DescriptorType::eStorageImage: case vk::DescriptorType::eInputAttachment:
				if (!w.pImageInfo) return false;
				data = std::as_bytes(std::span(w.pImageInfo, w.descriptorCount)); break;
			case vk::DescriptorType::eUniformTexelBuffer: case vk::DescriptorType::eStorageTexelBuffer:
				if (!w.pTexelBufferView) return false;
				data = std::as_bytes(std::span(w.pTexelBufferView, w.descriptorCount)); break;
			default: return false;
			}
		}
		return Parts(op, std::span(parts).first(first + 1 + 2 * writes.size()));
	}
	template <typename T> static std::span<const std::byte> Bytes(const T& value) {
		static_assert(std::is_trivially_copyable_v<T>);
		return std::as_bytes(std::span(&value, 1));
	}
	bool Parts(Op op, std::span<const std::span<const std::byte>> parts) { return m_queue.AppendParts(static_cast<uint16_t>(op), parts); }
	Common::CommandRecording::Queue& m_queue;
};
} // namespace Libs::Graphics::VkRecording
#endif
