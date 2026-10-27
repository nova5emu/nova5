#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/vkRecorderBackend.h"

namespace Libs::Graphics {
using namespace VkRecording;

void CommandBuffer::BindPipeline(vk::PipelineBindPoint point, vk::Pipeline pipeline) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::BindPipeline, Pipeline {point, pipeline}));
	} else Handle().bindPipeline(point, pipeline);
}
void CommandBuffer::PushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                  uint32_t offset, uint32_t bytes, const void* data) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().PushConstants(layout, stages, offset,
		    {static_cast<const std::byte*>(data), bytes}));
	} else Handle().pushConstants(layout, stages, offset, bytes, data);
}
void CommandBuffer::BindVertexBuffers(uint32_t first, uint32_t count, const vk::Buffer* buffers,
                                      const uint64_t* offsets, const uint64_t* sizes) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().BindVerticesSized(first,
		    {buffers, count}, {offsets, count}, {sizes, count}));
	} else Handle().bindVertexBuffers2(first, count, buffers, offsets, sizes, nullptr);
}
void CommandBuffer::BindIndexBuffer(vk::Buffer buffer, uint64_t offset, uint64_t size,
                                    vk::IndexType type, bool bounded) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::BindIndex, Index {buffer, offset, type, size, bounded}));
	} else if (bounded) Handle().bindIndexBuffer2KHR(buffer, offset, size, type);
	else Handle().bindIndexBuffer(buffer, offset, type);
}
void CommandBuffer::CommitDynamicState(DynamicState& state) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Dynamic(state));
		// Consume exactly the dirty bits Commit would consume, including bits
		// intentionally retained while depth/stencil tests are disabled.
		state.Acknowledge(m_graphics);
	} else state.Commit(m_graphics, Handle());
}
void CommandBuffer::Barrier(const vk::DependencyInfo& info) const {
	Commands().pipelineBarrier2(info);
}
void CommandBuffer::EmitMemoryBarrier(vk::PipelineStageFlags source, vk::PipelineStageFlags destination,
                                  const vk::MemoryBarrier& barrier) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(barrier.pNext != nullptr);
		vk::MemoryBarrier2 memory {};
		memory.srcStageMask = vk::PipelineStageFlags2 {static_cast<VkPipelineStageFlags>(source)};
		memory.dstStageMask = vk::PipelineStageFlags2 {static_cast<VkPipelineStageFlags>(destination)};
		memory.srcAccessMask = vk::AccessFlags2 {static_cast<VkAccessFlags>(barrier.srcAccessMask)};
		memory.dstAccessMask = vk::AccessFlags2 {static_cast<VkAccessFlags>(barrier.dstAccessMask)};
		vk::DependencyInfo info {};
		info.memoryBarrierCount = 1; info.pMemoryBarriers = &memory;
		Barrier(info);
	} else Handle().pipelineBarrier(source, destination, {}, 1, &barrier, 0, nullptr, 0, nullptr);
}
void CommandBuffer::Draw(uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::Draw, VkRecording::Draw {count, instances, first, first_instance}));
	} else Handle().draw(count, instances, first, first_instance);
}
void CommandBuffer::DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t vertex, uint32_t first_instance) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::DrawIndexed, VkRecording::Draw {count, instances, first, first_instance, vertex}));
	} else Handle().drawIndexed(count, instances, first, vertex, first_instance);
}
void CommandBuffer::DrawIndirect(vk::Buffer buffer, uint64_t offset, uint32_t count, uint32_t stride) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::DrawIndirect, Indirect {buffer, {}, offset, 0, count, stride}));
	} else Handle().drawIndirect(buffer, offset, count, stride);
}
void CommandBuffer::DrawIndexedIndirect(vk::Buffer buffer, uint64_t offset, uint32_t count, uint32_t stride) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::DrawIndexedIndirect, Indirect {buffer, {}, offset, 0, count, stride}));
	} else Handle().drawIndexedIndirect(buffer, offset, count, stride);
}
void CommandBuffer::DrawIndirectCount(vk::Buffer buffer, uint64_t offset, vk::Buffer counts, uint64_t count_offset, uint32_t count, uint32_t stride) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::DrawIndirectCount, Indirect {buffer, counts, offset, count_offset, count, stride}));
	} else Handle().drawIndirectCount(buffer, offset, counts, count_offset, count, stride);
}
void CommandBuffer::DrawIndexedIndirectCount(vk::Buffer buffer, uint64_t offset, vk::Buffer counts, uint64_t count_offset, uint32_t count, uint32_t stride) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::DrawIndexedIndirectCount, Indirect {buffer, counts, offset, count_offset, count, stride}));
	} else Handle().drawIndexedIndirectCount(buffer, offset, counts, count_offset, count, stride);
}
void CommandBuffer::DrawMeshTasksIndirect(vk::Buffer buffer, uint64_t offset, uint32_t count, uint32_t stride) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::DrawMeshIndirect, Indirect {buffer, {}, offset, 0, count, stride}));
	} else Handle().drawMeshTasksIndirectEXT(buffer, offset, count, stride);
}
void CommandBuffer::Dispatch(uint32_t x, uint32_t y, uint32_t z) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::Dispatch, VkRecording::Dispatch {x, y, z}));
	} else Handle().dispatch(x, y, z);
}
void CommandBuffer::DrawMeshTasks(uint32_t x, uint32_t y, uint32_t z) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::DrawMesh, VkRecording::Dispatch {x, y, z}));
	} else Handle().drawMeshTasksEXT(x, y, z);
}
void CommandBuffer::DispatchBase(uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::DispatchBase, DispatchOffset {{bx, by, bz}, {x, y, z}}));
	} else Handle().dispatchBase(bx, by, bz, x, y, z);
}
void CommandBuffer::DispatchIndirect(vk::Buffer buffer, uint64_t offset) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::DispatchIndirect, Indirect {buffer, {}, offset}));
	} else Handle().dispatchIndirect(buffer, offset);
}
void CommandBuffer::BeginQuery(vk::QueryPool pool, uint32_t query, vk::QueryControlFlags flags) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::BeginQuery, Query {pool, query, flags}));
	} else Handle().beginQuery(pool, query, flags);
}
void CommandBuffer::EndQuery(vk::QueryPool pool, uint32_t query) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(Op::EndQuery, Query {pool, query, {}}));
	} else Handle().endQuery(pool, query);
}
} // namespace Libs::Graphics
